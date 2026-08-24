#include "eventbook/api/eligibility.hpp"

#include <algorithm>
#include <map>

namespace eventbook {
namespace {

/// Bands are validated independently, then checked against one another.
///
/// Observed live across 4,000 markets spanning every lifecycle state: every
/// market carries exactly one band, and only two grids exist -- deci_cent
/// stepping $0.0010 and linear_cent stepping $0.0100, in a near-even split.
/// The multi-band handling below is therefore defensive rather than exercised
/// by production data, which is why it errs toward rejecting anything it does
/// not fully understand instead of guessing.
std::optional<IneligibilityReason> validate_price_grid(const std::vector<PriceRange>& ranges) {
    if (ranges.empty()) {
        return IneligibilityReason::NoPriceGrid;
    }

    for (const auto& band : ranges) {
        if (band.step.units <= 0 || band.start >= band.end) {
            return IneligibilityReason::MalformedPriceGrid;
        }
        if (!is_valid_yes_price(band.start) || !is_valid_yes_price(band.end)) {
            return IneligibilityReason::PriceGridOutOfBounds;
        }
        // A band whose width is not a whole number of ticks has an ambiguous
        // top level: it is unclear whether the last partial step is tradeable.
        if ((band.end.units - band.start.units) % band.step.units != 0) {
            return IneligibilityReason::MalformedPriceGrid;
        }
    }

    // A price falling inside two bands would sit on two different tick grids at
    // once, so its legality would depend on which band was consulted. Rejecting
    // is safer than picking one. Quadratic, which is free: bands number one.
    for (std::size_t left = 0; left < ranges.size(); ++left) {
        for (std::size_t right = left + 1; right < ranges.size(); ++right) {
            const bool disjoint =
                ranges[left].end < ranges[right].start || ranges[right].end < ranges[left].start;
            if (!disjoint) {
                return IneligibilityReason::OverlappingPriceBands;
            }
        }
    }

    return std::nullopt;
}

}  // namespace

std::string_view to_string(IneligibilityReason reason) {
    switch (reason) {
        case IneligibilityReason::Multivariate:
            return "multivariate combo market";
        case IneligibilityReason::NotBinaryMarket:
            return "market_type is not binary";
        case IneligibilityReason::UnknownStatus:
            return "unrecognized lifecycle status";
        case IneligibilityReason::NoPriceGrid:
            return "no price_ranges published";
        case IneligibilityReason::MalformedPriceGrid:
            return "price grid is malformed";
        case IneligibilityReason::PriceGridOutOfBounds:
            return "price grid falls outside settlement bounds";
        case IneligibilityReason::OverlappingPriceBands:
            return "price bands overlap";
        case IneligibilityReason::StatusNotRequested:
            return "status not the one requested";
        case IneligibilityReason::AlreadyClosed:
            return "market has already closed";
        case IneligibilityReason::ClosesTooSoon:
            return "closes too soon to record";
        case IneligibilityReason::InsufficientOpenInterest:
            return "open interest below the configured minimum";
    }
    return "unknown ineligibility reason";
}

bool is_structural(IneligibilityReason reason) {
    switch (reason) {
        case IneligibilityReason::Multivariate:
        case IneligibilityReason::NotBinaryMarket:
        case IneligibilityReason::UnknownStatus:
        case IneligibilityReason::NoPriceGrid:
        case IneligibilityReason::MalformedPriceGrid:
        case IneligibilityReason::PriceGridOutOfBounds:
        case IneligibilityReason::OverlappingPriceBands:
            return true;
        case IneligibilityReason::StatusNotRequested:
        case IneligibilityReason::AlreadyClosed:
        case IneligibilityReason::ClosesTooSoon:
        case IneligibilityReason::InsufficientOpenInterest:
            return false;
    }
    return false;
}

ExchangeTimestamp assume_exchange_clock(LocalTimestamp local) {
    return ExchangeTimestamp{local.value};
}

bool is_on_price_grid(Price price, const std::vector<PriceRange>& ranges) {
    for (const auto& band : ranges) {
        if (band.step.units <= 0 || price < band.start || price > band.end) {
            continue;
        }
        if ((price.units - band.start.units) % band.step.units == 0) {
            return true;
        }
    }
    return false;
}

std::optional<IneligibilityReason> check_eligibility(const Market& market,
                                                     const EligibilityCriteria& criteria,
                                                     ExchangeTimestamp as_of) {
    // Structural checks run first so the reason reported is the most
    // fundamental one. Telling an operator that a combo market has
    // "insufficient open interest" would be true and useless; the informative
    // answer is that we cannot represent it at all.
    if (is_multivariate(market)) {
        return IneligibilityReason::Multivariate;
    }
    if (market.market_type != "binary") {
        return IneligibilityReason::NotBinaryMarket;
    }
    if (market.status == MarketStatus::Unknown) {
        return IneligibilityReason::UnknownStatus;
    }
    if (const auto grid_problem = validate_price_grid(market.price_ranges)) {
        return grid_problem;
    }

    if (criteria.required_status.has_value() && market.status != *criteria.required_status) {
        return IneligibilityReason::StatusNotRequested;
    }
    if (market.close_time <= as_of) {
        return IneligibilityReason::AlreadyClosed;
    }
    if (market.close_time - as_of < criteria.minimum_time_to_close) {
        return IneligibilityReason::ClosesTooSoon;
    }
    if (market.open_interest < criteria.minimum_open_interest) {
        return IneligibilityReason::InsufficientOpenInterest;
    }

    return std::nullopt;
}

std::size_t UniverseSelection::rejected_for(IneligibilityReason reason) const {
    return static_cast<std::size_t>(
        std::count_if(rejected.begin(), rejected.end(),
                      [reason](const MarketRejection& entry) { return entry.reason == reason; }));
}

std::vector<std::pair<IneligibilityReason, std::size_t>> UniverseSelection::rejection_breakdown()
    const {
    std::map<IneligibilityReason, std::size_t> counts;
    for (const auto& entry : rejected) {
        ++counts[entry.reason];
    }
    return {counts.begin(), counts.end()};
}

UniverseSelection select_universe(const std::vector<Market>& markets,
                                  const EligibilityCriteria& criteria, ExchangeTimestamp as_of) {
    UniverseSelection selection;
    for (const auto& market : markets) {
        if (const auto reason = check_eligibility(market, criteria, as_of)) {
            selection.rejected.push_back(MarketRejection{market.ticker, *reason});
        } else {
            selection.eligible.push_back(market);
        }
    }
    return selection;
}

std::vector<std::pair<EventTicker, std::size_t>> markets_by_event(
    const std::vector<Market>& markets) {
    std::map<EventTicker, std::size_t> counts;
    for (const auto& market : markets) {
        ++counts[market.event_ticker];
    }
    return {counts.begin(), counts.end()};
}

}  // namespace eventbook
