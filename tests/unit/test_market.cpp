#include "eventbook/api/market.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>

#include "fixtures.hpp"

using eventbook::is_multivariate;
using eventbook::MarketParseErrorKind;
using eventbook::MarketStatus;
using eventbook::parse_market;
using eventbook::parse_market_page;
using eventbook::parse_market_response;
using eventbook::Price;
using eventbook::PriceDelta;
using eventbook::Quantity;
using eventbook::testing::read_fixture;

TEST_CASE("a real binary market parses into domain types") {
    // Fixture captured live from GET /markets, not hand-written, so a schema
    // change shows up here rather than in production.
    const auto page = parse_market_page(read_fixture("markets_page1.json"));
    REQUIRE(page.has_value());
    REQUIRE(page->markets.size() == 2);

    const auto& market = page->markets.front();
    CHECK(market.ticker.value == "KXFED-27APR-T4.25");
    CHECK(market.event_ticker.value == "KXFED-27APR");
    CHECK(market.market_type == "binary");
    CHECK(market.status == MarketStatus::Active);
    CHECK(market.can_close_early);

    // Prices arrive as decimal strings and land as exact integers.
    CHECK(market.yes_bid == Price{1600});
    CHECK(market.yes_ask == Price{3500});
    CHECK(market.last_price == Price{1700});

    // Sizes are genuinely fractional: 95.12 contracts, not 95.
    CHECK(market.yes_bid_size == Quantity{9512});
    CHECK(market.yes_ask_size == Quantity{1378});
    CHECK(market.volume == Quantity{1'025'197});
    CHECK(market.open_interest == Quantity{205'001});
}

TEST_CASE("sibling strikes share one event ticker") {
    // The M5 partitioning case, in real data: two contracts on the same FOMC
    // decision whose prices are mechanically related.
    const auto page = parse_market_page(read_fixture("markets_page1.json"));
    REQUIRE(page.has_value());
    REQUIRE(page->markets.size() == 2);

    CHECK(page->markets[0].ticker != page->markets[1].ticker);
    CHECK(page->markets[0].event_ticker == page->markets[1].event_ticker);
}

TEST_CASE("the price grid comes from the market, not from an assumption") {
    const auto page = parse_market_page(read_fixture("markets_page1.json"));
    REQUIRE(page.has_value());
    const auto& market = page->markets.front();

    CHECK(market.price_level_structure == "linear_cent");
    REQUIRE(market.price_ranges.size() == 1);
    CHECK(market.price_ranges[0].start == Price{0});
    CHECK(market.price_ranges[0].end == Price{10000});
    // One cent here -- but deci_cent markets step by $0.0010, which is why the
    // tick is read per market rather than hard-coded.
    CHECK(market.price_ranges[0].step == PriceDelta{100});
}

TEST_CASE("a deci-cent market steps ten times finer") {
    const auto page = parse_market_page(read_fixture("markets_page2.json"));
    REQUIRE(page.has_value());
    REQUIRE(page->markets.size() == 1);
    const auto& market = page->markets.front();

    CHECK(market.price_level_structure == "deci_cent");
    REQUIRE(market.price_ranges.size() == 1);
    CHECK(market.price_ranges[0].step == PriceDelta{10});
}

TEST_CASE("multivariate markets are identifiable") {
    const auto plain = parse_market_page(read_fixture("markets_page1.json"));
    REQUIRE(plain.has_value());
    CHECK_FALSE(is_multivariate(plain->markets.front()));
    CHECK_FALSE(plain->markets.front().mve_collection_ticker.has_value());

    const auto combo = parse_market_page(read_fixture("markets_page2.json"));
    REQUIRE(combo.has_value());
    CHECK(is_multivariate(combo->markets.front()));
    REQUIRE(combo->markets.front().mve_collection_ticker.has_value());
    CHECK_FALSE(combo->markets.front().mve_collection_ticker->empty());
}

TEST_CASE("timestamps parse from the RFC 3339 fields") {
    const auto page = parse_market_page(read_fixture("markets_page1.json"));
    REQUIRE(page.has_value());
    const auto& market = page->markets.front();

    CHECK(format_rfc3339(market.open_time) == "2025-10-13T14:00:00.000000Z");
    CHECK(format_rfc3339(market.close_time) == "2027-04-28T17:55:00.000000Z");
    REQUIRE(market.expected_expiration_time.has_value());
    CHECK(format_rfc3339(*market.expected_expiration_time) == "2027-04-28T18:05:00.000000Z");
}

TEST_CASE("the cursor marks whether more pages exist") {
    const auto first = parse_market_page(read_fixture("markets_page1.json"));
    REQUIRE(first.has_value());
    CHECK_FALSE(first->cursor.empty());

    const auto last = parse_market_page(read_fixture("markets_page2.json"));
    REQUIRE(last.has_value());
    CHECK(last->cursor.empty());
}

TEST_CASE("every status the API can report is recognized") {
    // Observed live: the field vocabulary differs from the query vocabulary.
    CHECK(eventbook::market_status_from_string("initialized") == MarketStatus::Initialized);
    CHECK(eventbook::market_status_from_string("active") == MarketStatus::Active);
    CHECK(eventbook::market_status_from_string("closed") == MarketStatus::Closed);
    CHECK(eventbook::market_status_from_string("determined") == MarketStatus::Determined);
    CHECK(eventbook::market_status_from_string("finalized") == MarketStatus::Finalized);

    // A state this build has never seen is Unknown rather than a parse failure:
    // a recorder should keep recording. The eligibility filter rejects Unknown
    // explicitly so nothing unrecognized reaches the research universe.
    CHECK(eventbook::market_status_from_string("hibernating") == MarketStatus::Unknown);
}

TEST_CASE("unknown JSON fields are ignored") {
    // Kalshi adds fields over time. Refusing to parse because of one would take
    // the collector down for no reason.
    const std::string body = R"({
      "ticker":"X-1","event_ticker":"X","market_type":"binary","status":"active",
      "price_level_structure":"linear_cent",
      "price_ranges":[{"start":"0.0000","end":"1.0000","step":"0.0100"}],
      "open_time":"2026-01-01T00:00:00Z","close_time":"2026-02-01T00:00:00Z",
      "yes_bid_dollars":"0.4000","yes_ask_dollars":"0.4200",
      "yes_bid_size_fp":"10.00","yes_ask_size_fp":"20.00",
      "last_price_dollars":"0.4100","volume_fp":"1.00","volume_24h_fp":"1.00",
      "open_interest_fp":"1.00","can_close_early":true,
      "a_field_invented_next_year":{"nested":[1,2,3]}
    })";
    const auto market = parse_market(body);
    REQUIRE(market.has_value());
    CHECK(market->ticker.value == "X-1");
}

TEST_CASE("a parse failure names the field that broke") {
    // "some market failed to parse" is not actionable; naming close_time is.
    const std::string base = R"({
      "ticker":"X-1","event_ticker":"X","market_type":"binary","status":"active",
      "price_level_structure":"linear_cent",
      "price_ranges":[{"start":"0.0000","end":"1.0000","step":"0.0100"}],
      "open_time":"2026-01-01T00:00:00Z","close_time":"2026-02-01T00:00:00Z",
      "yes_bid_dollars":"0.4000","yes_ask_dollars":"0.4200",
      "yes_bid_size_fp":"10.00","yes_ask_size_fp":"20.00",
      "last_price_dollars":"0.4100","volume_fp":"1.00","volume_24h_fp":"1.00",
      "open_interest_fp":"1.00","can_close_early":true})";

    SECTION("missing field") {
        std::string body = base;
        body.replace(body.find("\"close_time\""), 12, "\"absent_time\"");
        const auto market = parse_market(body);
        REQUIRE_FALSE(market.has_value());
        CHECK(market.error().kind == MarketParseErrorKind::MissingField);
        CHECK(market.error().field == "close_time");
    }

    SECTION("wrong JSON type") {
        std::string body = base;
        body.replace(body.find(R"("yes_bid_dollars":"0.4000")"), 26,
                     R"("yes_bid_dollars":0.4000000)");
        const auto market = parse_market(body);
        REQUIRE_FALSE(market.has_value());
        CHECK(market.error().kind == MarketParseErrorKind::WrongFieldType);
        CHECK(market.error().field == "yes_bid_dollars");
    }

    SECTION("unparseable price") {
        std::string body = base;
        body.replace(body.find(R"("0.4000")"), 8, R"("4/10ths")");
        const auto market = parse_market(body);
        REQUIRE_FALSE(market.has_value());
        CHECK(market.error().kind == MarketParseErrorKind::InvalidPrice);
    }

    SECTION("unparseable timestamp") {
        std::string body = base;
        body.replace(body.find(R"("2026-02-01T00:00:00Z")"), 22, R"("next February.......")");
        const auto market = parse_market(body);
        REQUIRE_FALSE(market.has_value());
        CHECK(market.error().kind == MarketParseErrorKind::InvalidTimestamp);
        CHECK(market.error().field == "close_time");
    }
}

TEST_CASE("malformed JSON is reported, not thrown") {
    const auto market = parse_market("{not json at all");
    REQUIRE_FALSE(market.has_value());
    CHECK(market.error().kind == MarketParseErrorKind::MalformedJson);

    const auto page = parse_market_page("[]");
    REQUIRE_FALSE(page.has_value());
    CHECK(page.error().kind == MarketParseErrorKind::NotAnObject);

    const auto missing = parse_market_page(R"({"cursor":""})");
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error().kind == MarketParseErrorKind::MissingField);
    CHECK(missing.error().field == "markets");
}

TEST_CASE("the single-market endpoint wraps its object") {
    // GET /markets/{ticker} returns {"market": {...}} while the list endpoint
    // returns the object bare. Handing this body to parse_market would fail on
    // every required field at once, which is a confusing way to discover a
    // shape difference.
    const auto page = parse_market_page(read_fixture("markets_page1.json"));
    REQUIRE(page.has_value());

    const std::string wrapped =
        R"({"market":{"ticker":"X-1","event_ticker":"X","market_type":"binary",)"
        R"("status":"active","price_level_structure":"linear_cent",)"
        R"("price_ranges":[{"start":"0.0000","end":"1.0000","step":"0.0100"}],)"
        R"("open_time":"2026-01-01T00:00:00Z","close_time":"2026-02-01T00:00:00Z",)"
        R"("yes_bid_dollars":"0.4000","yes_ask_dollars":"0.4200",)"
        R"("yes_bid_size_fp":"10.00","yes_ask_size_fp":"20.00",)"
        R"("last_price_dollars":"0.4100","volume_fp":"1.00","volume_24h_fp":"1.00",)"
        R"("open_interest_fp":"1.00","can_close_early":true}})";

    const auto market = parse_market_response(wrapped);
    REQUIRE(market.has_value());
    CHECK(market->ticker.value == "X-1");
    CHECK(market->price_ranges.size() == 1);
    CHECK(market->price_ranges[0].step == eventbook::PriceDelta{100});

    // The wrapper is required, not optional.
    const auto bare = parse_market_response(R"({"ticker":"X-1"})");
    REQUIRE_FALSE(bare.has_value());
    CHECK(bare.error().kind == MarketParseErrorKind::MissingField);
    CHECK(bare.error().field == "market");
}
