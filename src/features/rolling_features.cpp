#include "eventbook/features/rolling_features.hpp"

#include <algorithm>
#include <cmath>
#include <utility>
#include <variant>

namespace eventbook {

RollingFeatureState::RollingFeatureState(std::vector<std::chrono::seconds> windows)
    : windows_(std::move(windows)) {
    std::sort(windows_.begin(), windows_.end());
    if (!windows_.empty()) {
        longest_ = windows_.back();
    }
}

void RollingFeatureState::observe(const MarketEvent& event, LocalTimestamp at) {
    if (const auto* trade = std::get_if<PublicTrade>(&event)) {
        trades_.push_back(TradeEntry{at, trade->taker_side, trade->quantity});
    } else if (const auto* delta = std::get_if<BookDelta>(&event)) {
        if (delta->delta.units == 0) {
            return;  // a no-op delta is neither an add nor a remove
        }
        deltas_.push_back(DeltaEntry{at, delta->delta.units > 0});
    }
}

void RollingFeatureState::observe_sample(LocalTimestamp at, std::optional<double> midpoint) {
    samples_.push_back(SampleEntry{at, midpoint});
}

std::vector<WindowFeatures> RollingFeatureState::summarize(LocalTimestamp now) const {
    std::vector<WindowFeatures> result;
    result.reserve(windows_.size());

    for (const auto window : windows_) {
        WindowFeatures features;
        features.window = window;
        const auto cutoff = local_time_from_epoch_micros(epoch_micros(now) -
                                                         std::chrono::microseconds{window}.count());

        std::int64_t total_units = 0;
        for (auto entry = trades_.rbegin(); entry != trades_.rend(); ++entry) {
            if (entry->at <= cutoff) {
                break;
            }
            ++features.trades;
            total_units += entry->quantity.units;
            features.signed_trade_volume +=
                entry->side == TradeSide::BuyYes ? entry->quantity.units : -entry->quantity.units;
        }
        features.trade_volume = Quantity{total_units};
        if (total_units > 0) {
            features.trade_flow_imbalance = static_cast<double>(features.signed_trade_volume) /
                                            static_cast<double>(total_units);
        }

        for (auto entry = deltas_.rbegin(); entry != deltas_.rend(); ++entry) {
            if (entry->at <= cutoff) {
                break;
            }
            if (entry->added) {
                ++features.book_adds;
            } else {
                ++features.book_removes;
            }
        }

        // Squared changes between CONSECUTIVE valid midpoints. A gap where the
        // book was invalid contributes nothing rather than a spurious jump
        // spanning the outage.
        double sum_squares = 0.0;
        std::size_t contributions = 0;
        std::optional<double> newer;
        for (auto entry = samples_.rbegin(); entry != samples_.rend(); ++entry) {
            if (entry->at <= cutoff) {
                break;
            }
            if (!entry->midpoint.has_value()) {
                newer.reset();
                continue;
            }
            if (newer.has_value()) {
                const double change = *newer - *entry->midpoint;
                sum_squares += change * change;
                ++contributions;
            }
            newer = entry->midpoint;
        }
        if (contributions > 0) {
            features.realized_volatility = std::sqrt(sum_squares);
        }

        result.push_back(features);
    }
    return result;
}

void RollingFeatureState::expire(LocalTimestamp now) {
    if (longest_.count() == 0) {
        return;
    }
    const auto cutoff = local_time_from_epoch_micros(epoch_micros(now) -
                                                     std::chrono::microseconds{longest_}.count());

    while (!trades_.empty() && trades_.front().at <= cutoff) {
        trades_.pop_front();
    }
    while (!deltas_.empty() && deltas_.front().at <= cutoff) {
        deltas_.pop_front();
    }
    // One extra sample is kept so the oldest in-window change still has a
    // partner to difference against.
    while (samples_.size() > 1 && samples_[1].at <= cutoff) {
        samples_.pop_front();
    }
}

}  // namespace eventbook
