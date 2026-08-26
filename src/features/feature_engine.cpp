#include "eventbook/features/feature_engine.hpp"

#include <utility>

namespace eventbook {
namespace {

/// (B_k - A_k) / (B_k + A_k). Empty when both sides are zero.
std::optional<double> imbalance(Quantity bid_depth, Quantity ask_depth) {
    const auto total = bid_depth.units + ask_depth.units;
    if (total == 0) {
        return std::nullopt;
    }
    return static_cast<double>(bid_depth.units - ask_depth.units) / static_cast<double>(total);
}

}  // namespace

FeatureRow compute_features(const MarketState& state, MarketTicker ticker, LocalTimestamp at,
                            std::optional<ExchangeTimestamp> exchange_time,
                            std::optional<SequenceNumber> sequence) {
    FeatureRow row;
    row.market_ticker = std::move(ticker);
    row.sample_time = at;
    row.last_exchange_time = exchange_time;
    row.last_sequence = sequence;
    row.book_valid = state.book().is_valid();

    // Everything below is left empty when the book cannot be trusted. A row
    // that reported a stale midpoint as current would be worse than no row.
    if (!row.book_valid) {
        return row;
    }

    const auto& book = state.book();
    row.best_bid = book.best_bid();
    row.best_ask = book.best_ask();
    row.spread = book.spread();
    row.bid_levels = static_cast<std::int64_t>(book.level_count(BookSide::Bid));
    row.ask_levels = static_cast<std::int64_t>(book.level_count(BookSide::Ask));

    row.bid_depth_1 = book.depth(BookSide::Bid, 1);
    row.bid_depth_3 = book.depth(BookSide::Bid, 3);
    row.bid_depth_5 = book.depth(BookSide::Bid, 5);
    row.ask_depth_1 = book.depth(BookSide::Ask, 1);
    row.ask_depth_3 = book.depth(BookSide::Ask, 3);
    row.ask_depth_5 = book.depth(BookSide::Ask, 5);

    row.imbalance_1 = imbalance(row.bid_depth_1, row.ask_depth_1);
    row.imbalance_3 = imbalance(row.bid_depth_3, row.ask_depth_3);
    row.imbalance_5 = imbalance(row.bid_depth_5, row.ask_depth_5);

    if (row.best_bid.has_value() && row.best_ask.has_value()) {
        const double bid = static_cast<double>(row.best_bid->units);
        const double ask = static_cast<double>(row.best_ask->units);
        row.midpoint = (bid + ask) / 2.0;

        const double bid_size = static_cast<double>(row.bid_depth_1.units);
        const double ask_size = static_cast<double>(row.ask_depth_1.units);
        const double total_size = bid_size + ask_size;
        if (total_size > 0.0) {
            // Weighted toward the side with LESS resting size, because that is
            // the side more likely to be consumed next.
            row.microprice = (ask * bid_size + bid * ask_size) / total_size;
            row.microprice_displacement = *row.microprice - *row.midpoint;
        }
    }
    return row;
}

FeatureSampler::FeatureSampler(MarketTicker ticker, std::chrono::seconds interval,
                               std::vector<std::chrono::seconds> windows)
    : ticker_(std::move(ticker)), interval_(interval), rolling_(std::move(windows)) {}

void FeatureSampler::observe_event(const MarketEvent& event, LocalTimestamp at) {
    rolling_.observe(event, at);
}

void FeatureSampler::observe(std::optional<ExchangeTimestamp> exchange_time,
                             std::optional<SequenceNumber> sequence) {
    if (exchange_time.has_value()) {
        last_exchange_time_ = exchange_time;
    }
    if (sequence.has_value()) {
        last_sequence_ = sequence;
    }
}

void FeatureSampler::advance(const MarketState& state, LocalTimestamp now, const RowHandler& sink) {
    if (!next_boundary_.has_value()) {
        // Align to whole multiples of the interval since the epoch so two
        // journals of the same market produce rows on the same grid.
        const auto ticks = epoch_micros(now) / std::chrono::microseconds{interval_}.count();
        const auto aligned = (ticks + 1) * std::chrono::microseconds{interval_}.count();
        next_boundary_ = local_time_from_epoch_micros(aligned);
    }

    while (now >= *next_boundary_) {
        auto row =
            compute_features(state, ticker_, *next_boundary_, last_exchange_time_, last_sequence_);
        // Summarised as of the boundary, using only events at or before it.
        row.windows = rolling_.summarize(*next_boundary_);
        // The midpoint feeds realized volatility, measured across the sampled
        // series so a busy second and a quiet second stay comparable.
        rolling_.observe_sample(*next_boundary_, row.midpoint);
        rolling_.expire(*next_boundary_);
        ++rows_emitted_;
        if (!row.book_valid) {
            ++invalid_rows_;
        }
        sink(row);
        next_boundary_ = local_time_from_epoch_micros(epoch_micros(*next_boundary_) +
                                                      std::chrono::microseconds{interval_}.count());
    }
}

void FeatureSampler::finish(const MarketState& state, LocalTimestamp now, const RowHandler& sink) {
    auto row = compute_features(state, ticker_, now, last_exchange_time_, last_sequence_);
    row.windows = rolling_.summarize(now);
    ++rows_emitted_;
    if (!row.book_valid) {
        ++invalid_rows_;
    }
    sink(row);
}

}  // namespace eventbook
