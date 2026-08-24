#include "eventbook/api/eligibility.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <string>
#include <vector>

#include "fixtures.hpp"

using eventbook::check_eligibility;
using eventbook::EligibilityCriteria;
using eventbook::EventTicker;
using eventbook::IneligibilityReason;
using eventbook::is_on_price_grid;
using eventbook::is_structural;
using eventbook::Market;
using eventbook::markets_by_event;
using eventbook::MarketStatus;
using eventbook::parse_market_page;
using eventbook::Price;
using eventbook::PriceDelta;
using eventbook::PriceRange;
using eventbook::Quantity;
using eventbook::select_universe;
using eventbook::testing::read_fixture;

namespace {

// Two sibling KXFED strikes: plain binary, active, linear_cent grid.
std::vector<Market> plain_markets() {
    const auto page = parse_market_page(read_fixture("markets_page1.json"));
    REQUIRE(page.has_value());
    return page->markets;
}

// One multivariate combo market, deci_cent grid.
Market combo_market() {
    const auto page = parse_market_page(read_fixture("markets_page2.json"));
    REQUIRE(page.has_value());
    REQUIRE(page->markets.size() == 1);
    return page->markets.front();
}

// Well before the fixtures' 2027 close times.
eventbook::ExchangeTimestamp as_of() {
    const auto stamp = eventbook::parse_rfc3339("2026-08-24T00:00:00Z");
    REQUIRE(stamp.has_value());
    return *stamp;
}

}  // namespace

TEST_CASE("a real binary market passes the inclusion rule") {
    const auto markets = plain_markets();
    for (const auto& market : markets) {
        INFO("ticker=" << market.ticker.value);
        CHECK_FALSE(check_eligibility(market, EligibilityCriteria{}, as_of()).has_value());
    }
}

TEST_CASE("multivariate combo markets are excluded") {
    // The reason this filter exists: every one of the 200 markets returned by a
    // default status=open query was a combo market.
    const auto reason = check_eligibility(combo_market(), EligibilityCriteria{}, as_of());
    REQUIRE(reason.has_value());
    CHECK(*reason == IneligibilityReason::Multivariate);
    CHECK(is_structural(*reason));
}

TEST_CASE("structural reasons are reported ahead of selection reasons") {
    // A combo market that would ALSO fail a selection rule must still report
    // Multivariate. "Insufficient open interest" would be true and useless.
    Market market = combo_market();
    market.open_interest = Quantity{0};

    EligibilityCriteria criteria;
    criteria.minimum_open_interest = Quantity{100'000};

    const auto reason = check_eligibility(market, criteria, as_of());
    REQUIRE(reason.has_value());
    CHECK(*reason == IneligibilityReason::Multivariate);
}

TEST_CASE("non-binary market types are excluded") {
    Market market = plain_markets().front();
    market.market_type = "scalar";

    const auto reason = check_eligibility(market, EligibilityCriteria{}, as_of());
    REQUIRE(reason.has_value());
    CHECK(*reason == IneligibilityReason::NotBinaryMarket);
}

TEST_CASE("an unrecognized status is excluded rather than guessed at") {
    // parse_market tolerates a new venue state so the recorder keeps running;
    // the filter is where that tolerance stops, so nothing unrecognized reaches
    // the research universe.
    Market market = plain_markets().front();
    market.status = MarketStatus::Unknown;

    const auto reason = check_eligibility(market, EligibilityCriteria{}, as_of());
    REQUIRE(reason.has_value());
    CHECK(*reason == IneligibilityReason::UnknownStatus);
    CHECK(is_structural(*reason));
}

TEST_CASE("price grids that cannot be represented are excluded") {
    Market market = plain_markets().front();

    SECTION("no grid published") {
        market.price_ranges.clear();
        const auto reason = check_eligibility(market, EligibilityCriteria{}, as_of());
        REQUIRE(reason.has_value());
        CHECK(*reason == IneligibilityReason::NoPriceGrid);
    }

    SECTION("zero tick") {
        market.price_ranges = {PriceRange{Price{0}, Price{10000}, PriceDelta{0}}};
        const auto reason = check_eligibility(market, EligibilityCriteria{}, as_of());
        REQUIRE(reason.has_value());
        CHECK(*reason == IneligibilityReason::MalformedPriceGrid);
    }

    SECTION("width is not a whole number of ticks") {
        // The top level would be ambiguous: is the partial step tradeable?
        market.price_ranges = {PriceRange{Price{0}, Price{9999}, PriceDelta{100}}};
        const auto reason = check_eligibility(market, EligibilityCriteria{}, as_of());
        REQUIRE(reason.has_value());
        CHECK(*reason == IneligibilityReason::MalformedPriceGrid);
    }

    SECTION("grid leaves the settlement range") {
        market.price_ranges = {PriceRange{Price{0}, Price{20000}, PriceDelta{100}}};
        const auto reason = check_eligibility(market, EligibilityCriteria{}, as_of());
        REQUIRE(reason.has_value());
        CHECK(*reason == IneligibilityReason::PriceGridOutOfBounds);
    }

    SECTION("bands overlap") {
        // A price inside two bands would sit on two tick grids at once.
        market.price_ranges = {PriceRange{Price{0}, Price{6000}, PriceDelta{100}},
                               PriceRange{Price{5000}, Price{10000}, PriceDelta{10}}};
        const auto reason = check_eligibility(market, EligibilityCriteria{}, as_of());
        REQUIRE(reason.has_value());
        CHECK(*reason == IneligibilityReason::OverlappingPriceBands);
    }
}

TEST_CASE("closed and nearly-closed markets are excluded") {
    Market market = plain_markets().front();
    const auto close = eventbook::parse_rfc3339("2026-09-01T12:00:00Z");
    REQUIRE(close.has_value());
    market.close_time = *close;

    SECTION("already closed") {
        const auto after = eventbook::parse_rfc3339("2026-09-01T12:00:01Z");
        REQUIRE(after.has_value());
        const auto reason = check_eligibility(market, EligibilityCriteria{}, *after);
        REQUIRE(reason.has_value());
        CHECK(*reason == IneligibilityReason::AlreadyClosed);
    }

    SECTION("closes too soon to yield a usable session") {
        const auto shortly_before = eventbook::parse_rfc3339("2026-09-01T11:59:00Z");
        REQUIRE(shortly_before.has_value());
        const auto reason = check_eligibility(market, EligibilityCriteria{}, *shortly_before);
        REQUIRE(reason.has_value());
        CHECK(*reason == IneligibilityReason::ClosesTooSoon);
        CHECK_FALSE(is_structural(*reason));
    }

    SECTION("comfortably open") {
        const auto well_before = eventbook::parse_rfc3339("2026-09-01T06:00:00Z");
        REQUIRE(well_before.has_value());
        CHECK_FALSE(check_eligibility(market, EligibilityCriteria{}, *well_before).has_value());
    }
}

TEST_CASE("the open-interest gate is off by default") {
    // Liquidity correlates with everything the study measures, so a threshold
    // has to be a deliberate, pre-registered act rather than a silent default.
    Market market = plain_markets().front();
    market.open_interest = Quantity{0};

    CHECK_FALSE(check_eligibility(market, EligibilityCriteria{}, as_of()).has_value());
    CHECK(EligibilityCriteria{}.minimum_open_interest == Quantity{0});

    EligibilityCriteria strict;
    strict.minimum_open_interest = Quantity{100};
    const auto reason = check_eligibility(market, strict, as_of());
    REQUIRE(reason.has_value());
    CHECK(*reason == IneligibilityReason::InsufficientOpenInterest);
    CHECK_FALSE(is_structural(*reason));
}

TEST_CASE("the status filter can be relaxed") {
    Market market = plain_markets().front();
    market.status = MarketStatus::Finalized;

    const auto reason = check_eligibility(market, EligibilityCriteria{}, as_of());
    REQUIRE(reason.has_value());
    CHECK(*reason == IneligibilityReason::StatusNotRequested);

    EligibilityCriteria any_status;
    any_status.required_status.reset();
    CHECK_FALSE(check_eligibility(market, any_status, as_of()).has_value());
}

TEST_CASE("select_universe keeps both halves and accounts for every market") {
    std::vector<Market> markets = plain_markets();
    markets.push_back(combo_market());

    Market wrong_type = plain_markets().front();
    wrong_type.market_type = "scalar";
    wrong_type.ticker = eventbook::MarketTicker{"SCALAR-1"};
    markets.push_back(wrong_type);

    const auto selection = select_universe(markets, EligibilityCriteria{}, as_of());

    CHECK(selection.eligible.size() == 2);
    CHECK(selection.rejected.size() == 2);
    // Nothing is silently dropped: the two halves must account for the input.
    CHECK(selection.considered() == markets.size());

    CHECK(selection.rejected_for(IneligibilityReason::Multivariate) == 1);
    CHECK(selection.rejected_for(IneligibilityReason::NotBinaryMarket) == 1);
    CHECK(selection.rejected_for(IneligibilityReason::ClosesTooSoon) == 0);
}

TEST_CASE("the rejection breakdown is stable and omits empty reasons") {
    std::vector<Market> markets = plain_markets();
    markets.push_back(combo_market());

    const auto breakdown =
        select_universe(markets, EligibilityCriteria{}, as_of()).rejection_breakdown();
    REQUIRE(breakdown.size() == 1);
    CHECK(breakdown.front().first == IneligibilityReason::Multivariate);
    CHECK(breakdown.front().second == 1);
}

TEST_CASE("a universe is described by events, not only by market count") {
    // Twenty markets across two events is a far smaller sample than twenty
    // across twenty, because sibling strikes on one event move together.
    const auto grouped = markets_by_event(plain_markets());
    REQUIRE(grouped.size() == 1);
    CHECK(grouped.front().first == EventTicker{"KXFED-27APR"});
    CHECK(grouped.front().second == 2);
}

TEST_CASE("is_on_price_grid respects the market's own tick") {
    const std::vector<PriceRange> linear_cent{PriceRange{Price{0}, Price{10000}, PriceDelta{100}}};
    CHECK(is_on_price_grid(Price{0}, linear_cent));
    CHECK(is_on_price_grid(Price{1600}, linear_cent));
    CHECK(is_on_price_grid(Price{10000}, linear_cent));
    CHECK_FALSE(is_on_price_grid(Price{1650}, linear_cent));
    CHECK_FALSE(is_on_price_grid(Price{10100}, linear_cent));

    // The same price is legal on a deci-cent grid and not on a cent grid, which
    // is exactly why the tick is read per market instead of assumed.
    const std::vector<PriceRange> deci_cent{PriceRange{Price{0}, Price{10000}, PriceDelta{10}}};
    CHECK(is_on_price_grid(Price{1650}, deci_cent));
    CHECK_FALSE(is_on_price_grid(Price{1655}, deci_cent));
}

TEST_CASE("is_on_price_grid uses the real grids the venue publishes") {
    const auto cent_market = plain_markets().front();
    CHECK(is_on_price_grid(cent_market.yes_bid, cent_market.price_ranges));
    CHECK_FALSE(is_on_price_grid(Price{1605}, cent_market.price_ranges));

    const auto deci_market = combo_market();
    CHECK(is_on_price_grid(Price{1650}, deci_market.price_ranges));
}

TEST_CASE("assume_exchange_clock names the cross-clock assumption") {
    const auto local = eventbook::local_time_from_epoch_micros(1'787'529'600'000'000);
    const auto assumed = eventbook::assume_exchange_clock(local);
    CHECK(eventbook::epoch_micros(assumed) == eventbook::epoch_micros(local));
}
