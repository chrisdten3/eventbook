#include "eventbook/book/market_state.hpp"

#include <utility>
#include <variant>

namespace eventbook {

MarketState::MarketState(MarketTicker ticker, std::vector<PriceRange> grid)
    : book_(std::move(ticker), std::move(grid)) {}

void MarketState::note_validity(LocalTimestamp observed_at) {
    if (!book_.is_valid() && !invalid_since_.has_value()) {
        invalid_since_ = observed_at;
    } else if (book_.is_valid() && invalid_since_.has_value()) {
        stats_.invalid_time += observed_at - *invalid_since_;
        invalid_since_.reset();
    }
    if (book_.is_crossed()) {
        ++stats_.crossed_observations;
    }
}

std::optional<BookRejection> MarketState::apply(const MarketEvent& event,
                                                LocalTimestamp observed_at) {
    std::optional<BookRejection> rejection;

    if (const auto* snapshot = std::get_if<BookSnapshot>(&event)) {
        ++stats_.snapshots;
        rejection = book_.apply(*snapshot);
        if (rejection.has_value()) {
            ++stats_.rejected_snapshots;
        }
    } else if (const auto* delta = std::get_if<BookDelta>(&event)) {
        ++stats_.deltas;
        rejection = book_.apply(*delta);
        if (rejection.has_value()) {
            ++stats_.rejected_deltas;
            if (*rejection == BookRejection::SequenceGap) {
                ++stats_.sequence_gaps;
            }
        }
    } else if (std::holds_alternative<PublicTrade>(event)) {
        ++stats_.trades;
    } else if (std::holds_alternative<SubscriptionAck>(event)) {
        ++stats_.subscription_acks;
    } else if (std::holds_alternative<StreamError>(event)) {
        ++stats_.stream_errors;
    } else if (std::holds_alternative<UnhandledMessage>(event)) {
        ++stats_.unhandled_messages;
    }

    note_validity(observed_at);
    return rejection;
}

void MarketState::on_disconnected(LocalTimestamp observed_at) {
    ++stats_.disconnections;
    book_.invalidate();
    note_validity(observed_at);
}

void MarketState::finish(LocalTimestamp observed_at) {
    if (invalid_since_.has_value()) {
        stats_.invalid_time += observed_at - *invalid_since_;
        invalid_since_.reset();
    }
}

}  // namespace eventbook
