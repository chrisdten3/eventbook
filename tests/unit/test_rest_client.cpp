#include "eventbook/api/rest_client.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>

#include "fake_http_transport.hpp"

using eventbook::HttpError;
using eventbook::HttpMethod;
using eventbook::HttpResponse;
using eventbook::KalshiEnvironment;
using eventbook::KalshiRestClient;
using eventbook::RestErrorKind;
using eventbook::testing::FakeHttpTransport;

TEST_CASE("the client prefixes the versioned API base path") {
    FakeHttpTransport transport;
    transport.enqueue_ok(200, "{}");
    KalshiRestClient client{transport};

    REQUIRE(client.get("/markets").has_value());
    CHECK(transport.last_request().target == "/trade-api/v2/markets");
}

TEST_CASE("the client appends a query string only when there is one") {
    FakeHttpTransport transport;
    transport.enqueue_ok(200, "{}");
    transport.enqueue_ok(200, "{}");
    KalshiRestClient client{transport};

    REQUIRE(client.get("/markets", "limit=1").has_value());
    CHECK(transport.last_request().target == "/trade-api/v2/markets?limit=1");

    REQUIRE(client.get("/exchange/status").has_value());
    CHECK(transport.last_request().target == "/trade-api/v2/exchange/status");
}

TEST_CASE("environment selects the host, and the two differ in TLD") {
    FakeHttpTransport production_transport;
    production_transport.enqueue_ok(200, "{}");
    KalshiRestClient production{production_transport, KalshiEnvironment::Production};
    REQUIRE(production.get("/markets").has_value());
    CHECK(production_transport.last_request().host == "external-api.kalshi.com");

    FakeHttpTransport demo_transport;
    demo_transport.enqueue_ok(200, "{}");
    KalshiRestClient demo{demo_transport, KalshiEnvironment::Demo};
    REQUIRE(demo.get("/markets").has_value());
    CHECK(demo_transport.last_request().host == "external-api.demo.kalshi.co");
}

TEST_CASE("production is the default environment") {
    FakeHttpTransport transport;
    KalshiRestClient client{transport};
    CHECK(client.environment() == KalshiEnvironment::Production);
}

TEST_CASE("a 2xx response is passed through with its body intact") {
    FakeHttpTransport transport;
    transport.enqueue_ok(200, R"({"markets":[]})");
    KalshiRestClient client{transport};

    const auto response = client.get("/markets");
    REQUIRE(response.has_value());
    CHECK(response->status_code == 200);
    CHECK(response->body == R"({"markets":[]})");
}

TEST_CASE("every non-2xx status becomes a typed error") {
    // The point is that an error page can never reach a JSON parser and be
    // mistaken for market data.
    struct Case {
        int status;
        RestErrorKind expected;
    };

    for (const auto& scenario :
         {Case{429, RestErrorKind::RateLimited}, Case{404, RestErrorKind::NotFound},
          Case{400, RestErrorKind::ClientError}, Case{403, RestErrorKind::ClientError},
          Case{500, RestErrorKind::ServerError}, Case{503, RestErrorKind::ServerError},
          Case{302, RestErrorKind::UnexpectedStatus}}) {
        FakeHttpTransport transport;
        transport.enqueue_ok(scenario.status, "irrelevant");
        KalshiRestClient client{transport};

        const auto response = client.get("/markets");
        INFO("status=" << scenario.status);
        REQUIRE_FALSE(response.has_value());
        CHECK(response.error().kind == scenario.expected);
        CHECK(response.error().status_code == scenario.status);
    }
}

TEST_CASE("rate limiting is distinguishable from other client errors") {
    // 429 needs its own kind because the response is to back off and retry,
    // whereas a 400 means the request itself was wrong. Kalshi sends no
    // Retry-After header, so the caller must supply exponential backoff.
    FakeHttpTransport transport;
    transport.enqueue_ok(429, R"({"error":"too many requests"})");
    KalshiRestClient client{transport};

    const auto response = client.get("/markets");
    REQUIRE_FALSE(response.has_value());
    CHECK(response.error().kind == RestErrorKind::RateLimited);
    CHECK(response.error().kind != RestErrorKind::ClientError);
}

TEST_CASE("a transport failure keeps its underlying cause") {
    FakeHttpTransport transport;
    transport.enqueue(eventbook::Failure{HttpError::TlsHandshakeFailed});
    KalshiRestClient client{transport};

    const auto response = client.get("/markets");
    REQUIRE_FALSE(response.has_value());
    CHECK(response.error().kind == RestErrorKind::Transport);
    CHECK(response.error().transport_error == HttpError::TlsHandshakeFailed);
}

TEST_CASE("the client only ever issues GET") {
    // AGENTS.md requires proof that the default configuration cannot send a
    // write request. The strong guarantee is structural -- HttpMethod has no
    // enumerator other than Get, so there is nothing else to select -- and this
    // asserts the client does not somehow contrive one anyway.
    FakeHttpTransport transport;
    for (int i = 0; i < 4; ++i) {
        transport.enqueue_ok(200, "{}");
    }
    KalshiRestClient client{transport};

    (void)client.get("/markets");
    (void)client.get("/markets", "limit=1");
    (void)client.get("/exchange/status");
    (void)client.get("/series", "limit=1");

    REQUIRE(transport.requests().size() == 4);
    for (const auto& request : transport.requests()) {
        CHECK(request.method == HttpMethod::Get);
    }
}
