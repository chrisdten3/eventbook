#include "eventbook/common/identifiers.hpp"

#include <catch2/catch_test_macros.hpp>

#include <map>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>

using eventbook::EventTicker;
using eventbook::MarketTicker;
using eventbook::SeriesTicker;

// The whole point of these types: one cannot stand in for another. If these
// ever start passing, the M5 split can silently partition by the wrong key.
static_assert(!std::is_convertible_v<MarketTicker, EventTicker>);
static_assert(!std::is_convertible_v<EventTicker, MarketTicker>);
static_assert(!std::is_convertible_v<EventTicker, SeriesTicker>);
static_assert(!std::is_constructible_v<EventTicker, MarketTicker>);

// Nor can a bare string drift in unnoticed.
static_assert(!std::is_convertible_v<std::string, MarketTicker>);

TEST_CASE("tickers compare by value") {
    CHECK(MarketTicker{"KXFED-26DEC-T3.00"} == MarketTicker{"KXFED-26DEC-T3.00"});
    CHECK(MarketTicker{"KXFED-26DEC-T3.00"} != MarketTicker{"KXFED-26DEC-T3.25"});
    CHECK(MarketTicker{"A"} < MarketTicker{"B"});
}

TEST_CASE("tickers key an ordered map") {
    std::map<MarketTicker, int> depth;
    depth[MarketTicker{"KXFED-26DEC-T3.00"}] = 42;
    depth[MarketTicker{"KXFED-26DEC-T3.25"}] = 7;

    REQUIRE(depth.size() == 2);
    CHECK(depth.at(MarketTicker{"KXFED-26DEC-T3.00"}) == 42);
}

TEST_CASE("tickers key an unordered map") {
    // The shape per-market state takes during collection and replay.
    std::unordered_map<MarketTicker, int> messages;
    messages[MarketTicker{"KXFED-26DEC-T3.00"}] += 1;
    messages[MarketTicker{"KXFED-26DEC-T3.00"}] += 1;
    messages[MarketTicker{"KXFED-26DEC-T3.25"}] += 1;

    REQUIRE(messages.size() == 2);
    CHECK(messages.at(MarketTicker{"KXFED-26DEC-T3.00"}) == 2);
}

TEST_CASE("sibling markets group under one event ticker") {
    // The M5 partition unit. Two strikes on the same FOMC decision are separate
    // markets whose prices are mechanically related, so they must never land on
    // opposite sides of a train/test split.
    const EventTicker event{"KXFED-26DEC"};
    std::unordered_map<EventTicker, std::unordered_set<MarketTicker>> markets_by_event;
    markets_by_event[event].insert(MarketTicker{"KXFED-26DEC-T3.00"});
    markets_by_event[event].insert(MarketTicker{"KXFED-26DEC-T3.25"});

    REQUIRE(markets_by_event.size() == 1);
    CHECK(markets_by_event.at(event).size() == 2);
}

TEST_CASE("event and series tickers are independently usable") {
    const SeriesTicker series{"KXFED"};
    const EventTicker event{"KXFED-26DEC"};

    CHECK(series.value == "KXFED");
    CHECK(event.value == "KXFED-26DEC");

    // Note what is absent: no derivation of series from event. Ticker text is a
    // human-readable label, and splitting on '-' would be keying behaviour off
    // structure the API does not guarantee.
    CHECK(std::hash<SeriesTicker>{}(series) == std::hash<SeriesTicker>{}(SeriesTicker{"KXFED"}));
}
