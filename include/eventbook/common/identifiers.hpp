#pragma once

#include <cstddef>
#include <functional>
#include <string>

namespace eventbook {

/// One tradeable binary contract, e.g. "KXFED-26DEC-T3.00".
///
/// This is the `ticker` field of a market object and the `market_ticker` field
/// of every WebSocket message. It identifies a single yes/no proposition with
/// its own order book.
struct MarketTicker {
    std::string value;

    [[nodiscard]] friend auto operator<=>(const MarketTicker&, const MarketTicker&) = default;
};

/// One real-world occurrence that several markets resolve against, e.g. the
/// December 2026 FOMC decision.
///
/// This is the `event_ticker` field. Several markets typically hang off one
/// event -- sibling threshold contracts at different strikes -- and their prices
/// are mechanically related, because they describe the same underlying outcome.
///
/// That relationship is why this is a separate type rather than another string.
/// M5 must partition train/validation/test by *event*, keeping siblings
/// together: splitting by market would put "Fed cuts by more than 3.00%" in
/// train and "more than 3.25%" in test, which leaks. With distinct types the
/// split function takes an EventTicker and passing a MarketTicker fails to
/// compile, so the leak cannot happen silently.
struct EventTicker {
    std::string value;

    [[nodiscard]] friend auto operator<=>(const EventTicker&, const EventTicker&) = default;
};

/// A recurring family that events repeat under, e.g. "KXFED".
///
/// The research universe is frozen at this level: version one studies one
/// homogeneous recurring series rather than a mix of unrelated categories.
///
/// Note that the market object does not return this field -- it exists in the
/// REST API only as a query parameter. It is deliberately NOT derived by
/// splitting a market or event ticker on '-'. Ticker text is a human-readable
/// label, and AGENTS.md warns against keying behaviour off such structure; if a
/// series ticker is needed alongside a market, it must be carried from the
/// request that selected it.
struct SeriesTicker {
    std::string value;

    [[nodiscard]] friend auto operator<=>(const SeriesTicker&, const SeriesTicker&) = default;
};

}  // namespace eventbook

// Hash specializations so these can key an unordered_map -- the natural shape
// for per-market state during collection and replay. The defaulted operator<=>
// above already covers ordered containers.
template <>
struct std::hash<eventbook::MarketTicker> {
    [[nodiscard]] std::size_t operator()(const eventbook::MarketTicker& ticker) const noexcept {
        return std::hash<std::string>{}(ticker.value);
    }
};

template <>
struct std::hash<eventbook::EventTicker> {
    [[nodiscard]] std::size_t operator()(const eventbook::EventTicker& ticker) const noexcept {
        return std::hash<std::string>{}(ticker.value);
    }
};

template <>
struct std::hash<eventbook::SeriesTicker> {
    [[nodiscard]] std::size_t operator()(const eventbook::SeriesTicker& ticker) const noexcept {
        return std::hash<std::string>{}(ticker.value);
    }
};
