#include "eventbook/api/market_query.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>

#include "fake_http_transport.hpp"
#include "fixtures.hpp"

using eventbook::build_market_query;
using eventbook::fetch_all_markets;
using eventbook::fetch_market_page;
using eventbook::HttpError;
using eventbook::KalshiRestClient;
using eventbook::MarketQuery;
using eventbook::MarketQueryErrorKind;
using eventbook::MarketStatusFilter;
using eventbook::SeriesTicker;
using eventbook::testing::FakeHttpTransport;
using eventbook::testing::read_fixture;

TEST_CASE("query strings carry only the parameters that were set") {
    CHECK(build_market_query(MarketQuery{}, "") == "limit=100");

    MarketQuery query;
    query.limit = 1000;
    query.series_ticker = SeriesTicker{"KXFED"};
    query.status = MarketStatusFilter::Open;
    CHECK(build_market_query(query, "") == "limit=1000&series_ticker=KXFED&status=open");
    CHECK(build_market_query(query, "ABC123") ==
          "limit=1000&series_ticker=KXFED&status=open&cursor=ABC123");
}

TEST_CASE("the status filter uses the query vocabulary, not the field vocabulary") {
    // Querying status=active matches nothing and silently returns everything,
    // which is why MarketStatusFilter is a separate type from MarketStatus.
    MarketQuery query;
    query.status = MarketStatusFilter::Open;
    CHECK(build_market_query(query, "").find("status=open") != std::string::npos);

    query.status = MarketStatusFilter::Settled;
    CHECK(build_market_query(query, "").find("status=settled") != std::string::npos);
}

TEST_CASE("query values are percent-encoded") {
    MarketQuery query;
    query.series_ticker = SeriesTicker{"KX FED&limit=9999"};
    const auto encoded = build_market_query(query, "");

    // Without encoding, a value containing '&' would inject a parameter.
    CHECK(encoded == "limit=100&series_ticker=KX%20FED%26limit%3D9999");

    // Base64 cursors can contain '+', '/', and '='.
    CHECK(build_market_query(MarketQuery{}, "a+b/c=") == "limit=100&cursor=a%2Bb%2Fc%3D");
}

TEST_CASE("fetching one page hits the markets endpoint and parses the body") {
    FakeHttpTransport transport;
    transport.enqueue_ok(200, read_fixture("markets_page1.json"));
    KalshiRestClient client{transport};

    const auto page = fetch_market_page(client, MarketQuery{});
    REQUIRE(page.has_value());
    CHECK(page->markets.size() == 2);
    CHECK(transport.last_request().target == "/trade-api/v2/markets?limit=100");
}

TEST_CASE("pagination follows the cursor until it comes back empty") {
    FakeHttpTransport transport;
    transport.enqueue_ok(200, read_fixture("markets_page1.json"));  // cursor: PAGE2CURSOR
    transport.enqueue_ok(200, read_fixture("markets_page2.json"));  // cursor: ""
    KalshiRestClient client{transport};

    const auto markets = fetch_all_markets(client, MarketQuery{});
    REQUIRE(markets.has_value());
    CHECK(markets->size() == 3);

    REQUIRE(transport.requests().size() == 2);
    // The first request carries no cursor; the second carries the one page 1
    // handed back.
    CHECK(transport.requests()[0].target == "/trade-api/v2/markets?limit=100");
    CHECK(transport.requests()[1].target == "/trade-api/v2/markets?limit=100&cursor=PAGE2CURSOR");
}

TEST_CASE("a cursor that never advances is caught instead of looping forever") {
    // The dangerous failure: every individual response looks valid, so nothing
    // short of this check would notice.
    FakeHttpTransport transport;
    for (int i = 0; i < 8; ++i) {
        transport.enqueue_ok(200, R"({"markets":[],"cursor":"STUCK"})");
    }
    KalshiRestClient client{transport};

    const auto markets = fetch_all_markets(client, MarketQuery{});
    REQUIRE_FALSE(markets.has_value());
    CHECK(markets.error().kind == MarketQueryErrorKind::CursorNotAdvancing);

    // Stopped on the second page, having recognized the repeat.
    CHECK(transport.requests().size() == 2);
}

TEST_CASE("exhausting the page budget is an error, not a silent truncation") {
    // Returning a short list would understate the universe and bias anything
    // computed from it, with no indication that anything was missing.
    FakeHttpTransport transport;
    for (int i = 0; i < 10; ++i) {
        transport.enqueue_ok(200, R"({"markets":[],"cursor":")" + std::to_string(i) + R"("})");
    }
    KalshiRestClient client{transport};

    const auto markets = fetch_all_markets(client, MarketQuery{}, /*max_pages=*/3);
    REQUIRE_FALSE(markets.has_value());
    CHECK(markets.error().kind == MarketQueryErrorKind::PageLimitReached);
    CHECK(transport.requests().size() == 3);
}

TEST_CASE("a single page ends pagination immediately") {
    FakeHttpTransport transport;
    transport.enqueue_ok(200, read_fixture("markets_page2.json"));
    KalshiRestClient client{transport};

    const auto markets = fetch_all_markets(client, MarketQuery{});
    REQUIRE(markets.has_value());
    CHECK(markets->size() == 1);
    CHECK(transport.requests().size() == 1);
}

TEST_CASE("REST failures propagate with their cause intact") {
    FakeHttpTransport transport;
    transport.enqueue_ok(429, R"({"error":"too many requests"})");
    KalshiRestClient client{transport};

    const auto markets = fetch_all_markets(client, MarketQuery{});
    REQUIRE_FALSE(markets.has_value());
    CHECK(markets.error().kind == MarketQueryErrorKind::Rest);
    CHECK(markets.error().rest.kind == eventbook::RestErrorKind::RateLimited);
}

TEST_CASE("transport failures propagate too") {
    FakeHttpTransport transport;
    transport.enqueue(eventbook::Failure{HttpError::Timeout});
    KalshiRestClient client{transport};

    const auto page = fetch_market_page(client, MarketQuery{});
    REQUIRE_FALSE(page.has_value());
    CHECK(page.error().kind == MarketQueryErrorKind::Rest);
    CHECK(page.error().rest.transport_error == HttpError::Timeout);
}

TEST_CASE("a page that does not parse fails the whole query") {
    // Half a universe is worse than none: it would silently bias the study.
    FakeHttpTransport transport;
    transport.enqueue_ok(200, read_fixture("markets_page1.json"));
    transport.enqueue_ok(200, R"({"markets":[{"ticker":"broken"}],"cursor":""})");
    KalshiRestClient client{transport};

    const auto markets = fetch_all_markets(client, MarketQuery{});
    REQUIRE_FALSE(markets.has_value());
    CHECK(markets.error().kind == MarketQueryErrorKind::Parse);
    CHECK(markets.error().parse.field == "event_ticker");
}
