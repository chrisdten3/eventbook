#include "eventbook/book/order_book.hpp"

#include <utility>

namespace eventbook {
namespace {

// FNV-1a, 64-bit. Chosen for being short, dependency-free, and completely
// specified, which is what a reproducibility digest needs. It is not a
// cryptographic hash and is not used as one: nothing here defends against an
// adversary choosing book states, it only has to make an accidental divergence
// between two replays overwhelmingly likely to show up.
constexpr std::uint64_t kFnvOffsetBasis = 1469598103934665603ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

constexpr void hash_byte(std::uint64_t& digest, unsigned char byte) {
    digest ^= byte;
    digest *= kFnvPrime;
}

constexpr void hash_integer(std::uint64_t& digest, std::uint64_t value) {
    for (int shift = 0; shift < 64; shift += 8) {
        hash_byte(digest, static_cast<unsigned char>((value >> shift) & 0xFFU));
    }
}

void hash_text(std::uint64_t& digest, std::string_view text) {
    for (const char character : text) {
        hash_byte(digest, static_cast<unsigned char>(character));
    }
}

}  // namespace

std::string_view to_string(BookStatus status) {
    switch (status) {
        case BookStatus::AwaitingSnapshot:
            return "awaiting_snapshot";
        case BookStatus::Valid:
            return "valid";
    }
    return "unknown";
}

std::string_view to_string(BookRejection rejection) {
    switch (rejection) {
        case BookRejection::WrongMarket:
            return "message addressed to a different market";
        case BookRejection::SubscriptionMismatch:
            return "message from a different subscription";
        case BookRejection::DeltaBeforeSnapshot:
            return "delta received before a valid snapshot";
        case BookRejection::SequenceGap:
            return "sequence gap";
        case BookRejection::SequenceRegressed:
            return "sequence number went backwards";
        case BookRejection::PriceOutOfBounds:
            return "price outside settlement bounds";
        case BookRejection::PriceOffGrid:
            return "price not on the market's tick grid";
        case BookRejection::NegativeSnapshotLevel:
            return "snapshot level has negative size";
        case BookRejection::QuantityWouldGoNegative:
            return "delta removes more size than the level holds";
        case BookRejection::QuantityOverflow:
            return "resulting size out of range";
    }
    return "unknown book rejection";
}

bool invalidates_book(BookRejection rejection) {
    switch (rejection) {
        // These say the message was wrong. A snapshot for another market tells
        // us nothing about this one, so this book's state is untouched.
        case BookRejection::WrongMarket:
        case BookRejection::DeltaBeforeSnapshot:
            return false;

        // These say our state is wrong. A gap means we already missed
        // something; a delta removing size that was never there means the same.
        // Continuing to apply deltas to a book known to be desynchronized
        // produces quietly false data for the rest of the session.
        case BookRejection::SubscriptionMismatch:
        case BookRejection::SequenceGap:
        case BookRejection::SequenceRegressed:
        case BookRejection::PriceOutOfBounds:
        case BookRejection::PriceOffGrid:
        case BookRejection::NegativeSnapshotLevel:
        case BookRejection::QuantityWouldGoNegative:
        case BookRejection::QuantityOverflow:
            return true;
    }
    return true;
}

OrderBook::OrderBook(MarketTicker ticker, std::vector<PriceRange> grid)
    : ticker_(std::move(ticker)), grid_(std::move(grid)) {}

std::optional<BookRejection> OrderBook::validate_price(Price price) const {
    if (!is_valid_yes_price(price)) {
        return BookRejection::PriceOutOfBounds;
    }
    // An empty grid means the market's ticks are genuinely unknown rather than
    // that checking is optional, so bounds still apply.
    if (!grid_.empty() && !is_on_price_grid(price, grid_)) {
        return BookRejection::PriceOffGrid;
    }
    return std::nullopt;
}

void OrderBook::invalidate() {
    bids_.clear();
    asks_.clear();
    status_ = BookStatus::AwaitingSnapshot;
    last_sequence_.reset();
    subscription_.reset();
}

std::optional<BookRejection> OrderBook::apply(const BookSnapshot& snapshot) {
    if (snapshot.market_ticker != ticker_) {
        return BookRejection::WrongMarket;
    }

    // Validate everything before mutating anything. A snapshot that is rejected
    // half-way through would leave the book in a state that never existed on
    // the venue, which is worse than either accepting or refusing it whole.
    for (const auto& side : {snapshot.bids, snapshot.asks}) {
        for (const auto& level : side) {
            if (const auto problem = validate_price(level.price)) {
                invalidate();
                return problem;
            }
            if (level.quantity.units < 0) {
                invalidate();
                return BookRejection::NegativeSnapshotLevel;
            }
        }
    }

    bids_.clear();
    asks_.clear();
    // Zero-quantity levels are dropped on the way in, so the invariant that no
    // level is ever zero holds from the first message rather than being
    // restored later.
    for (const auto& level : snapshot.bids) {
        if (level.quantity.units > 0) {
            bids_[level.price] = level.quantity;
        }
    }
    for (const auto& level : snapshot.asks) {
        if (level.quantity.units > 0) {
            asks_[level.price] = level.quantity;
        }
    }

    // A snapshot deliberately does not check continuity with what came before.
    // It is the only way back from an invalid book, and recovering from a gap
    // means accepting a discontinuity by definition.
    status_ = BookStatus::Valid;
    last_sequence_ = snapshot.sequence;
    subscription_ = snapshot.subscription;
    return std::nullopt;
}

std::optional<BookRejection> OrderBook::apply(const BookDelta& delta) {
    if (delta.market_ticker != ticker_) {
        return BookRejection::WrongMarket;
    }
    if (status_ != BookStatus::Valid) {
        return BookRejection::DeltaBeforeSnapshot;
    }
    if (subscription_.has_value() && delta.subscription != *subscription_) {
        // Sequence numbers are scoped to a subscription, so a message from a
        // different sid cannot be checked for continuity against ours.
        invalidate();
        return BookRejection::SubscriptionMismatch;
    }

    const std::uint64_t expected = last_sequence_->value + 1;
    if (delta.sequence.value != expected) {
        const auto rejection = delta.sequence.value > expected ? BookRejection::SequenceGap
                                                               : BookRejection::SequenceRegressed;
        invalidate();
        return rejection;
    }

    if (const auto problem = validate_price(delta.price)) {
        invalidate();
        return problem;
    }

    auto update = [&](auto& levels) -> std::optional<BookRejection> {
        const auto position = levels.find(delta.price);
        const Quantity existing = position == levels.end() ? Quantity{} : position->second;

        const auto updated = apply_delta(existing, delta.delta);
        if (!updated) {
            // Not a bad number: evidence that the book we hold is not the book
            // the venue holds. The arithmetic reports impossibility; deciding
            // that it means desynchronization is this layer's judgement.
            return updated.error() == QuantityDeltaError::WouldGoNegative
                       ? BookRejection::QuantityWouldGoNegative
                       : BookRejection::QuantityOverflow;
        }

        if (updated->units == 0) {
            if (position != levels.end()) {
                levels.erase(position);
            }
        } else if (position != levels.end()) {
            position->second = *updated;
        } else {
            levels.emplace(delta.price, *updated);
        }
        return std::nullopt;
    };

    const auto problem = delta.side == BookSide::Bid ? update(bids_) : update(asks_);
    if (problem) {
        invalidate();
        return problem;
    }

    last_sequence_ = delta.sequence;
    return std::nullopt;
}

std::optional<Price> OrderBook::best_bid() const {
    if (!is_valid() || bids_.empty()) {
        return std::nullopt;
    }
    return bids_.begin()->first;
}

std::optional<Price> OrderBook::best_ask() const {
    if (!is_valid() || asks_.empty()) {
        return std::nullopt;
    }
    return asks_.begin()->first;
}

std::optional<PriceDelta> OrderBook::spread() const {
    const auto bid = best_bid();
    const auto ask = best_ask();
    if (!bid.has_value() || !ask.has_value()) {
        return std::nullopt;
    }
    return *ask - *bid;
}

Quantity OrderBook::depth(BookSide side, std::size_t levels) const {
    if (!is_valid()) {
        return Quantity{};
    }
    Quantity total{};
    std::size_t counted = 0;
    if (side == BookSide::Bid) {
        for (const auto& [price, quantity] : bids_) {
            if (counted++ >= levels) {
                break;
            }
            total = total + quantity;
        }
    } else {
        for (const auto& [price, quantity] : asks_) {
            if (counted++ >= levels) {
                break;
            }
            total = total + quantity;
        }
    }
    return total;
}

std::size_t OrderBook::level_count(BookSide side) const {
    if (!is_valid()) {
        return 0;
    }
    return side == BookSide::Bid ? bids_.size() : asks_.size();
}

bool OrderBook::is_crossed() const {
    const auto bid = best_bid();
    const auto ask = best_ask();
    return bid.has_value() && ask.has_value() && *bid >= *ask;
}

std::uint64_t OrderBook::state_hash() const {
    std::uint64_t digest = kFnvOffsetBasis;
    hash_text(digest, ticker_.value);
    hash_byte(digest, static_cast<unsigned char>(status_ == BookStatus::Valid ? 1 : 0));
    hash_integer(digest, last_sequence_.has_value() ? last_sequence_->value : 0);
    // A marker between the sides so that a level moving from bid to ask cannot
    // produce the same digest.
    hash_byte(digest, 'B');
    for (const auto& [price, quantity] : bids_) {
        hash_integer(digest, static_cast<std::uint64_t>(price.units));
        hash_integer(digest, static_cast<std::uint64_t>(quantity.units));
    }
    hash_byte(digest, 'A');
    for (const auto& [price, quantity] : asks_) {
        hash_integer(digest, static_cast<std::uint64_t>(price.units));
        hash_integer(digest, static_cast<std::uint64_t>(quantity.units));
    }
    return digest;
}

}  // namespace eventbook
