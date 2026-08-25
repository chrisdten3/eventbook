#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <vector>

#include "eventbook/book/order_book.hpp"
#include "eventbook/common/identifiers.hpp"
#include "eventbook/common/price.hpp"
#include "eventbook/common/time.hpp"
#include "eventbook/data/events.hpp"

namespace eventbook {

/// Everything a session needs to report about how well it went.
///
/// AGENTS.md requires per-session counts of connections, message types,
/// sequence gaps, snapshot refreshes, parse failures, invalid deltas, and time
/// spent with an invalid book. These are the ones derivable from the event
/// stream; the transport-level counters live on the session.
struct MarketStateStats {
    std::uint64_t snapshots{};
    std::uint64_t deltas{};
    std::uint64_t trades{};
    std::uint64_t subscription_acks{};
    std::uint64_t stream_errors{};
    std::uint64_t unhandled_messages{};

    std::uint64_t sequence_gaps{};
    std::uint64_t rejected_deltas{};
    std::uint64_t rejected_snapshots{};
    std::uint64_t disconnections{};
    std::uint64_t crossed_observations{};

    /// Total time the book could not be trusted. Research that ignores this is
    /// research on a book with holes in it.
    std::chrono::microseconds invalid_time{0};
};

/// Applies normalized events to an order book and counts what happened.
///
/// This type exists so that live collection and offline replay share ONE
/// implementation of "what does this event do to the book". AGENTS.md forbids
/// separate live and historical versions of order-book logic, and the way that
/// forbidding is enforced is by there being only one place the logic lives.
///
/// It never reads a clock. The observation time is a parameter, so live passes
/// local_now() and replay passes the timestamp recorded in the journal. That is
/// the difference between a replay that reproduces a session and one that
/// merely resembles it: a hidden clock read would make invalid_time -- and
/// therefore the reported metrics -- different on every run.
class MarketState {
public:
    MarketState(MarketTicker ticker, std::vector<PriceRange> grid);

    /// Apply one event. Returns the rejection when the book refused it, so a
    /// caller can react -- typically by asking for a fresh snapshot.
    [[nodiscard]] std::optional<BookRejection> apply(const MarketEvent& event,
                                                     LocalTimestamp observed_at);

    /// Record that the connection dropped. The book cannot be trusted across a
    /// disconnection, so this invalidates it: whatever the venue did while we
    /// were away is unknown, and a book that silently carries on would be
    /// quietly wrong for the rest of the session.
    void on_disconnected(LocalTimestamp observed_at);

    /// Close out the invalid-time accounting at end of session.
    void finish(LocalTimestamp observed_at);

    [[nodiscard]] const OrderBook& book() const {
        return book_;
    }

    [[nodiscard]] const MarketStateStats& stats() const {
        return stats_;
    }

    [[nodiscard]] std::uint64_t state_hash() const {
        return book_.state_hash();
    }

private:
    void note_validity(LocalTimestamp observed_at);

    OrderBook book_;
    MarketStateStats stats_;
    std::optional<LocalTimestamp> invalid_since_;
};

}  // namespace eventbook
