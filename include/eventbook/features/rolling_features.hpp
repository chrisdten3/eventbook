#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <optional>
#include <vector>

#include "eventbook/common/quantity.hpp"
#include "eventbook/common/time.hpp"
#include "eventbook/data/events.hpp"

namespace eventbook {

/// Activity summarised over one trailing window.
struct WindowFeatures {
    std::chrono::seconds window{};

    std::uint64_t trades{};
    Quantity trade_volume;

    /// Signed by taker aggression: buying YES is positive, selling YES
    /// negative. In Quantity units, so it stays exact.
    std::int64_t signed_trade_volume{};

    /// signed_trade_volume / trade_volume, in [-1, 1]. Empty when nothing
    /// traded -- which is most seconds, since we measured roughly 1,500 book
    /// updates per trade. A zero here would claim balanced flow where there was
    /// no flow at all.
    std::optional<double> trade_flow_imbalance;

    /// Quote churn: deltas that added and removed displayed size. These
    /// dominate the message stream and are what an execution simulator must
    /// model, since most size disappears by cancellation rather than by trade.
    std::uint64_t book_adds{};
    std::uint64_t book_removes{};

    /// Root sum of squared midpoint changes across the sampled series, in Price
    /// units.
    ///
    /// Absolute changes, NOT log returns. A one-cent move is a 69% log return
    /// at $0.01 and 2% at $0.50, so log returns would report enormous
    /// volatility for every contract near the settlement bounds purely as an
    /// artifact of price level. AGENTS.md warns about exactly this for relative
    /// measures on binary contracts.
    ///
    /// Empty until at least two valid midpoints exist inside the window.
    std::optional<double> realized_volatility;
};

/// Trailing-window state, advanced by event timestamps only.
///
/// Never reads a clock, for the same reason MarketState does not: a replayed
/// dataset must be identical to a live one, and a hidden clock read would make
/// every window boundary differ between runs.
class RollingFeatureState {
public:
    explicit RollingFeatureState(std::vector<std::chrono::seconds> windows);

    /// Feed one market event. Trades and book deltas are recorded; everything
    /// else is ignored.
    void observe(const MarketEvent& event, LocalTimestamp at);

    /// Record the midpoint at a sampling boundary. Realized volatility is
    /// computed across the sampled series rather than across every event, so it
    /// measures movement per unit time rather than per message -- otherwise a
    /// busy second and a quiet second would not be comparable.
    void observe_sample(LocalTimestamp at, std::optional<double> midpoint);

    /// Summarise every configured window as of `now`.
    [[nodiscard]] std::vector<WindowFeatures> summarize(LocalTimestamp now) const;

    /// Drop entries older than the longest window. Called after each sample so
    /// memory stays bounded by rate times window, not by session length.
    void expire(LocalTimestamp now);

    [[nodiscard]] const std::vector<std::chrono::seconds>& windows() const {
        return windows_;
    }

private:
    struct TradeEntry {
        LocalTimestamp at;
        TradeSide side;
        Quantity quantity;
    };

    struct DeltaEntry {
        LocalTimestamp at;
        bool added;
    };

    struct SampleEntry {
        LocalTimestamp at;
        std::optional<double> midpoint;
    };

    std::vector<std::chrono::seconds> windows_;
    std::chrono::seconds longest_{0};
    std::deque<TradeEntry> trades_;
    std::deque<DeltaEntry> deltas_;
    std::deque<SampleEntry> samples_;
};

}  // namespace eventbook
