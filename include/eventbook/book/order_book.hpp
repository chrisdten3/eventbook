#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string_view>
#include <vector>

#include "eventbook/common/identifiers.hpp"
#include "eventbook/common/price.hpp"
#include "eventbook/common/quantity.hpp"
#include "eventbook/data/events.hpp"

namespace eventbook {

/// Whether the book currently describes the market.
enum class BookStatus {
    /// No snapshot has been applied, or the book invalidated itself and is
    /// waiting for a fresh one. Deltas are refused in this state and every
    /// derived quantity is unavailable: AGENTS.md requires that state is not
    /// considered valid between a detected gap and a fresh snapshot, and a
    /// feature row computed from a half-known book is worse than no row.
    AwaitingSnapshot,
    Valid,
};

[[nodiscard]] std::string_view to_string(BookStatus status);

/// Why a message could not be applied.
enum class BookRejection {
    WrongMarket,              ///< addressed to a different ticker
    SubscriptionMismatch,     ///< a different sid, so a different sequence space
    DeltaBeforeSnapshot,      ///< no valid base state to apply it to
    SequenceGap,              ///< a message was missed
    SequenceRegressed,        ///< a repeat or out-of-order arrival
    PriceOutOfBounds,         ///< outside [$0.0000, $1.0000]
    PriceOffGrid,             ///< inside the range but not on the market's tick
    NegativeSnapshotLevel,    ///< a snapshot quoting a negative size
    QuantityWouldGoNegative,  ///< the delta removes more size than exists
    QuantityOverflow,
};

[[nodiscard]] std::string_view to_string(BookRejection rejection);

/// Whether a rejection means the book can no longer be trusted.
///
/// Some rejections say the message was wrong -- a snapshot for another market
/// changes nothing about this one. Others say our *state* is wrong: a sequence
/// gap or a delta removing size that was never there is evidence we already
/// missed something, and continuing to apply deltas to a book we know is
/// desynchronized would produce quietly false data for as long as the session
/// lasts.
[[nodiscard]] bool invalidates_book(BookRejection rejection);

/// An aggregated L2 book for one market, on the YES price scale.
///
/// Ordered maps keyed by price, so bids iterate best-first and asks best-first
/// without sorting. AGENTS.md suggests exactly this and warns against
/// optimizing the representation before a benchmark exists; the price domain is
/// bounded at 10,001 distinct values, so a flat array is a plausible future
/// alternative, but only once something measures it.
///
/// This type knows nothing about REST market objects, WebSocket framing, or
/// JSON. It consumes normalized events, which is what lets live collection and
/// offline replay share one implementation -- AGENTS.md's "one event model, two
/// data sources".
class OrderBook {
public:
    using BidLevels = std::map<Price, Quantity, std::greater<Price>>;
    using AskLevels = std::map<Price, Quantity, std::less<Price>>;

    /// `grid` is the market's published price_ranges. An empty grid disables
    /// tick checking but still enforces settlement bounds, which is the right
    /// behaviour when a market's grid is genuinely unknown rather than a licence
    /// to skip validation.
    OrderBook(MarketTicker ticker, std::vector<PriceRange> grid);

    /// Replace all state. This is also the only route back from an invalid
    /// book, which is why a snapshot never checks the sequence against what
    /// came before: recovering from a gap means accepting a discontinuity.
    [[nodiscard]] std::optional<BookRejection> apply(const BookSnapshot& snapshot);

    [[nodiscard]] std::optional<BookRejection> apply(const BookDelta& delta);

    /// Drop all state, e.g. on disconnect. A reconnect must re-snapshot.
    void invalidate();

    [[nodiscard]] BookStatus status() const {
        return status_;
    }

    [[nodiscard]] bool is_valid() const {
        return status_ == BookStatus::Valid;
    }

    [[nodiscard]] const MarketTicker& ticker() const {
        return ticker_;
    }

    [[nodiscard]] std::optional<SequenceNumber> last_sequence() const {
        return last_sequence_;
    }

    /// Top of book. Empty when the book is invalid or that side is empty --
    /// an empty side is an ordinary market condition, not an error, so it is
    /// reported with optional rather than an exception.
    [[nodiscard]] std::optional<Price> best_bid() const;
    [[nodiscard]] std::optional<Price> best_ask() const;

    /// Best ask minus best bid. Empty unless both sides are quoted.
    [[nodiscard]] std::optional<PriceDelta> spread() const;

    /// Total size across the best `levels` price levels of one side.
    [[nodiscard]] Quantity depth(BookSide side, std::size_t levels) const;

    [[nodiscard]] std::size_t level_count(BookSide side) const;

    /// Whether the best bid is at or above the best ask.
    ///
    /// Deliberately reported rather than rejected. A crossed book should be
    /// impossible in a continuous market, but AGENTS.md warns against
    /// discarding locked, crossed, or transitioning states as impossible before
    /// checking them against lifecycle messages. Treating this as fatal would
    /// throw away exactly the evidence needed to understand it, so the book
    /// stays valid and the condition is exposed for counting.
    [[nodiscard]] bool is_crossed() const;

    /// A deterministic digest of the book's observable state.
    ///
    /// Exists so replay can prove itself: applying one event log twice must
    /// yield the same hash. It covers validity, sequence, and every level, and
    /// depends on nothing incidental -- no addresses, no insertion order, no
    /// container capacity -- because std::map iterates in key order.
    [[nodiscard]] std::uint64_t state_hash() const;

    [[nodiscard]] const BidLevels& bids() const {
        return bids_;
    }

    [[nodiscard]] const AskLevels& asks() const {
        return asks_;
    }

private:
    [[nodiscard]] std::optional<BookRejection> validate_price(Price price) const;

    MarketTicker ticker_;
    std::vector<PriceRange> grid_;
    BidLevels bids_;
    AskLevels asks_;
    BookStatus status_{BookStatus::AwaitingSnapshot};
    std::optional<SequenceNumber> last_sequence_;
    std::optional<SubscriptionId> subscription_;
};

}  // namespace eventbook
