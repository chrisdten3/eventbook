#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "eventbook/common/identifiers.hpp"
#include "eventbook/common/price.hpp"
#include "eventbook/common/quantity.hpp"
#include "eventbook/common/time.hpp"
#include "eventbook/features/rolling_features.hpp"

namespace eventbook {

/// One observation of a market's state, sampled on a fixed interval.
///
/// Monetary fields keep the project's exact integer representation. Derived
/// ratios are doubles, which AGENTS.md permits only for statistics explicitly
/// documented as such -- these are, in docs/data_dictionary.md. Nothing here is
/// a canonical price.
///
/// Every field that can be undefined is optional rather than zero-filled. An
/// empty book side and a zero-size book side are different states, and a study
/// that cannot tell them apart will draw conclusions about the wrong thing.
struct FeatureRow {
    MarketTicker market_ticker;

    /// The interval boundary this row describes: state as of this instant,
    /// derived only from events at or before it. Sampling is driven by event
    /// timestamps, never by a wall clock, so a replay reproduces the dataset.
    LocalTimestamp sample_time;

    /// Exchange time of the most recent event applied before this boundary,
    /// so a derived row can be traced back to the raw record that produced it.
    std::optional<ExchangeTimestamp> last_exchange_time;

    /// False when the book could not be trusted at this instant -- before its
    /// first snapshot, or between a gap or disconnection and the recovery.
    /// AGENTS.md forbids invalid intervals producing normal feature rows, so
    /// every other field below is empty when this is false. The row is still
    /// emitted: a complete time series makes the hole visible, whereas a
    /// missing row is indistinguishable from a market nobody was watching.
    bool book_valid{false};

    std::optional<Price> best_bid;
    std::optional<Price> best_ask;

    /// Midpoint in Price units. A double because the midpoint of two adjacent
    /// ticks lands on a half unit.
    std::optional<double> midpoint;
    std::optional<PriceDelta> spread;

    Quantity bid_depth_1;
    Quantity bid_depth_3;
    Quantity bid_depth_5;
    Quantity ask_depth_1;
    Quantity ask_depth_3;
    Quantity ask_depth_5;

    /// (B_k - A_k) / (B_k + A_k), dimensionless in [-1, 1].
    ///
    /// Empty when both sides are zero rather than divided by an epsilon:
    /// AGENTS.md requires that case be reported missing, because a fabricated
    /// zero would be indistinguishable from a genuinely balanced book.
    std::optional<double> imbalance_1;
    std::optional<double> imbalance_3;
    std::optional<double> imbalance_5;

    /// (a * Q_b + b * Q_a) / (Q_b + Q_a), in Price units.
    ///
    /// The size-weighted midpoint: it leans toward the side with less resting
    /// size, which is the side more likely to be taken out. Empty unless both
    /// sides are quoted.
    std::optional<double> microprice;

    /// Microprice minus midpoint. The displacement is the signal; the level is
    /// mostly a restatement of the midpoint.
    std::optional<double> microprice_displacement;

    std::optional<std::int64_t> bid_levels;
    std::optional<std::int64_t> ask_levels;

    /// Sequence of the last event applied, so a row is traceable to a journal
    /// record without timestamp matching.
    std::optional<SequenceNumber> last_sequence;

    /// Trailing-window activity, one entry per configured window, shortest
    /// first. Populated even when the book is invalid: trades and quote churn
    /// are facts about the message stream, not about whether our reconstruction
    /// of the book can be trusted.
    std::vector<WindowFeatures> windows;
};

/// Column names, in the order the CSV exporter writes them. Window columns are
/// suffixed with their length in seconds.
[[nodiscard]] std::string feature_row_header(const std::vector<std::chrono::seconds>& windows);

/// One CSV line. Prices are written in DOLLARS for readability; the exact
/// integer representation is an implementation detail of the pipeline, not of
/// the published dataset. Empty fields are written as empty strings, never as
/// zero or NaN.
[[nodiscard]] std::string to_csv(const FeatureRow& row);

}  // namespace eventbook
