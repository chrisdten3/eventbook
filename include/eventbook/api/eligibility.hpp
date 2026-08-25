#pragma once

#include <chrono>
#include <cstddef>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include "eventbook/api/market.hpp"
#include "eventbook/common/identifiers.hpp"
#include "eventbook/common/price.hpp"
#include "eventbook/common/quantity.hpp"
#include "eventbook/common/time.hpp"

namespace eventbook {

/// Why a market is not in the research universe.
///
/// Every exclusion is recorded and counted. AGENTS.md requires unsupported
/// markets to be "rejected for an explicit documented reason", and the reason
/// is what makes a claim such as "studied 847 markets, excluded 12,405, here is
/// the breakdown" auditable instead of merely asserted. A filter that quietly
/// returns a shorter list cannot support that sentence at all.
enum class IneligibilityReason {
    // --- Structural: this system cannot faithfully represent the market. ---
    Multivariate,
    NotBinaryMarket,
    UnknownStatus,
    NoPriceGrid,
    MalformedPriceGrid,
    PriceGridOutOfBounds,
    OverlappingPriceBands,

    // --- Selection: a scope decision, frozen before analysis. ---
    StatusNotRequested,
    AlreadyClosed,
    ClosesTooSoon,
    InsufficientOpenInterest,
};

[[nodiscard]] std::string_view to_string(IneligibilityReason reason);

/// Whether a reason describes a limitation of this software (structural) or a
/// decision about what to study (selection).
///
/// This changes nothing about how the filter runs and everything about how
/// results are reported. A structural exclusion says "we cannot represent
/// this"; a selection exclusion says "we chose not to look at this", and every
/// one of those has to be pre-registered and restated beside any finding drawn
/// from the surviving sample.
[[nodiscard]] bool is_structural(IneligibilityReason reason);

/// The predefined inclusion rule.
///
/// AGENTS.md requires freezing the inclusion rule before final analysis and
/// then capturing every eligible market, rather than selecting favourable
/// periods after the fact. This struct is the thing that gets frozen: record it
/// with the experiment configuration and report it with any derived result.
///
/// Structural rules are deliberately absent from this struct. Whether a
/// multivariate market can be represented is a fact about the software, not a
/// preference, so there is no knob that could quietly admit one.
struct EligibilityCriteria {
    /// Restrict to one lifecycle state. Live collection wants Active.
    std::optional<MarketStatus> required_status{MarketStatus::Active};

    /// A market must stay open at least this long to be worth recording.
    /// A contract closing in thirty seconds yields a handful of messages and
    /// no usable session.
    std::chrono::seconds minimum_time_to_close{std::chrono::hours{1}};

    /// Minimum open interest. Defaults to zero, meaning disabled, on purpose.
    ///
    /// This is a liquidity threshold, and liquidity correlates with nearly
    /// everything the study measures: spread, depth, volatility, and how
    /// informative order flow is. Raising it after seeing results is
    /// cherry-picking, which AGENTS.md forbids under "Report only the best
    /// hyperparameter or market subset". Leaving it off by default makes any
    /// use of it a deliberate, visible act that has to be justified in the
    /// experiment protocol before the test set is touched.
    Quantity minimum_open_interest{0};
};

/// Read a local clock reading as though it were the venue's clock.
///
/// `close_time` is on Kalshi's clock and `local_now()` is on ours; they are
/// unsynchronized, which is precisely why slice 1.2 made them different types.
/// This function names the assumption instead of hiding it in a cast, so it can
/// be found by grep: the offset between two NTP-disciplined machines is
/// milliseconds, which is negligible against a minimum_time_to_close measured
/// in hours.
///
/// It would NOT be negligible for ordering events or measuring latency, and it
/// must never be used for either.
[[nodiscard]] ExchangeTimestamp assume_exchange_clock(LocalTimestamp local);

/// Apply the inclusion rule to one market. Returns nullopt when it is eligible.
[[nodiscard]] std::optional<IneligibilityReason> check_eligibility(
    const Market& market, const EligibilityCriteria& criteria, ExchangeTimestamp as_of);

struct MarketRejection {
    MarketTicker ticker;
    IneligibilityReason reason;
};

/// The outcome of applying the inclusion rule to a set of markets.
///
/// Both halves are kept. The rejected half is not waste -- it is the evidence
/// that the surviving universe was not chosen to flatter a result.
struct UniverseSelection {
    std::vector<Market> eligible;
    std::vector<MarketRejection> rejected;

    [[nodiscard]] std::size_t considered() const {
        return eligible.size() + rejected.size();
    }

    [[nodiscard]] std::size_t rejected_for(IneligibilityReason reason) const;

    /// Rejection counts, ordered by the enumerator so a report is stable
    /// between runs. Reasons with no rejections are omitted.
    [[nodiscard]] std::vector<std::pair<IneligibilityReason, std::size_t>> rejection_breakdown()
        const;
};

[[nodiscard]] UniverseSelection select_universe(const std::vector<Market>& markets,
                                                const EligibilityCriteria& criteria,
                                                ExchangeTimestamp as_of);

/// Eligible markets grouped by event, sorted by ticker for determinism.
///
/// The event is the unit M5 partitions on, so describing a universe by market
/// count alone understates how correlated it is: twenty markets across two
/// events is a much smaller sample than twenty across twenty.
[[nodiscard]] std::vector<std::pair<EventTicker, std::size_t>> markets_by_event(
    const std::vector<Market>& markets);

}  // namespace eventbook
