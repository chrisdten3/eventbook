#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>

#include "eventbook/book/market_state.hpp"
#include "eventbook/common/identifiers.hpp"
#include "eventbook/common/time.hpp"
#include "eventbook/features/feature_row.hpp"
#include "eventbook/features/rolling_features.hpp"

namespace eventbook {

/// Derive a feature row from a book at one instant.
///
/// Separated from the sampler so the arithmetic can be tested against a
/// hand-built book without any notion of time.
[[nodiscard]] FeatureRow compute_features(const MarketState& state, MarketTicker ticker,
                                          LocalTimestamp at,
                                          std::optional<ExchangeTimestamp> exchange_time,
                                          std::optional<SequenceNumber> sequence);

/// Emits one row per fixed interval, driven by event timestamps.
///
/// Boundaries are aligned to whole multiples of the interval since the epoch,
/// not to the first event, so two journals of the same market align row for row
/// and can be compared or concatenated without resampling.
///
/// The sampler is advanced BEFORE each event is applied, so a row describes the
/// book as of the boundary using only events at or before it. Advancing
/// afterwards would let information from after the boundary leak into the row
/// -- a small leak, and exactly the kind M5's validity depends on not having.
///
/// A quiet market still produces rows. If no event arrives for a minute, the
/// next event emits sixty rows describing an unchanged book, because a gap in a
/// time series and a market that did not move are different facts.
class FeatureSampler {
public:
    using RowHandler = std::function<void(const FeatureRow&)>;

    explicit FeatureSampler(MarketTicker ticker,
                            std::chrono::seconds interval = std::chrono::seconds{1},
                            std::vector<std::chrono::seconds> windows = {std::chrono::seconds{10},
                                                                         std::chrono::seconds{60}});

    /// Feed an event into the trailing windows. Called for every event, not
    /// only at sampling boundaries.
    void observe_event(const MarketEvent& event, LocalTimestamp at);

    [[nodiscard]] const std::vector<std::chrono::seconds>& windows() const {
        return rolling_.windows();
    }

    /// Emit rows for every boundary at or before `now`.
    void advance(const MarketState& state, LocalTimestamp now, const RowHandler& sink);

    /// Note the provenance of the event about to be applied, so the next row
    /// can be traced to it.
    void observe(std::optional<ExchangeTimestamp> exchange_time,
                 std::optional<SequenceNumber> sequence);

    /// Emit a final row at `now` so the last partial interval is not lost.
    void finish(const MarketState& state, LocalTimestamp now, const RowHandler& sink);

    [[nodiscard]] std::uint64_t rows_emitted() const {
        return rows_emitted_;
    }

    [[nodiscard]] std::uint64_t invalid_rows() const {
        return invalid_rows_;
    }

private:
    MarketTicker ticker_;
    std::chrono::seconds interval_;
    std::optional<LocalTimestamp> next_boundary_;
    std::optional<ExchangeTimestamp> last_exchange_time_;
    std::optional<SequenceNumber> last_sequence_;
    RollingFeatureState rolling_;
    std::uint64_t rows_emitted_{0};
    std::uint64_t invalid_rows_{0};
};

}  // namespace eventbook
