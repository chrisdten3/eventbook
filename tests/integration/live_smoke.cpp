// Opt-in live smoke test. Read-only, short, and clearly labelled.
//
// This target is built by CI but deliberately NOT registered with CTest, so
// `ctest` never reaches the network: an offline machine, a flaky link, or a
// Kalshi outage must not turn into a red build. Run it by hand:
//
//     ./build/dev/bin/eventbook_live_tests
//
// It verifies only that the transport can complete a TLS exchange with the
// venue and that a public endpoint still answers in the shape we expect. It
// sends no credentials, requests no account data, and -- because HttpMethod has
// no enumerator but Get -- cannot issue a write of any kind.

#include <catch2/catch_test_macros.hpp>

#include <chrono>

#include "eventbook/api/beast_http_transport.hpp"
#include "eventbook/api/rest_client.hpp"

using eventbook::BeastHttpTransport;
using eventbook::KalshiRestClient;

TEST_CASE("the exchange status endpoint answers over TLS") {
    BeastHttpTransport transport{std::chrono::seconds{15}};
    KalshiRestClient client{transport};

    const auto response = client.get("/exchange/status");
    if (!response) {
        INFO("kind=" << to_string(response.error().kind)
                     << " transport=" << to_string(response.error().transport_error)
                     << " status=" << response.error().status_code);
        FAIL("live request failed");
    }

    CHECK(response->status_code == 200);
    CHECK_FALSE(response->body.empty());
    CHECK(response->body.front() == '{');
}

TEST_CASE("the markets endpoint is reachable unauthenticated") {
    // Market data is public: this is what makes slice 1.3 need TLS but not
    // RSA-PSS request signing. If Kalshi ever changes that, this fails with a
    // 401 and tells us the authentication work has become necessary.
    BeastHttpTransport transport{std::chrono::seconds{15}};
    KalshiRestClient client{transport};

    const auto response = client.get("/markets", "limit=1");
    REQUIRE(response.has_value());
    CHECK(response->status_code == 200);
    CHECK(response->body.find("\"markets\"") != std::string::npos);
}

TEST_CASE("certificate verification is actually enforced") {
    // The single most important property of this transport. A host whose
    // certificate does not match must fail the handshake rather than quietly
    // returning data. badssl.com maintains these endpoints for exactly this.
    BeastHttpTransport transport{std::chrono::seconds{15}};

    eventbook::HttpRequest request;
    request.host = "wrong.host.badssl.com";
    request.target = "/";

    const auto response = transport.send(request);
    REQUIRE_FALSE(response.has_value());
    CHECK(response.error() == eventbook::HttpError::TlsHandshakeFailed);
}
