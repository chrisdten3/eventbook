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
#include "test_rsa_key.hpp"

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

// --- M2: authenticated WebSocket ---
//
// These need credentials. They are skipped, not failed, when the environment
// does not supply them, so the target stays runnable on a machine that has no
// key: a missing credential is a configuration fact, not a defect.

#include <atomic>
#include <thread>
#include <variant>

#include "eventbook/api/ws_session.hpp"
#include "eventbook/book/order_book.hpp"

TEST_CASE("an authenticated session receives a snapshot and builds a book") {
    auto signer = eventbook::load_signer_from_environment();
    if (!signer) {
        WARN("skipped: " << to_string(signer.error().kind));
        return;
    }

    eventbook::WsSessionConfig config;
    config.subscription.market_ticker = eventbook::MarketTicker{"KXFED-27APR-T4.25"};
    config.subscription.channels = {"orderbook_delta"};
    config.subscription.use_yes_price = true;
    config.max_reconnect_attempts = 1;

    eventbook::WebSocketSession session{config, *std::move(signer)};

    eventbook::OrderBook book{config.subscription.market_ticker,
                              {eventbook::PriceRange{eventbook::Price{0}, eventbook::Price{10000},
                                                     eventbook::PriceDelta{100}}}};
    std::atomic<bool> got_snapshot{false};

    session.on_event([&](const eventbook::MarketEvent& event) {
        if (const auto* snapshot = std::get_if<eventbook::BookSnapshot>(&event)) {
            (void)book.apply(*snapshot);
            got_snapshot = true;
            session.stop();
        }
    });

    // A hard deadline so a silent venue cannot hang the suite.
    std::thread guard{[&] {
        for (int i = 0; i < 200 && !got_snapshot; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds{100});
        }
        session.stop();
    }};

    session.run();
    guard.join();

    INFO("connections=" << session.stats().connections
                        << " messages=" << session.stats().messages_received
                        << " parse_failures=" << session.stats().parse_failures);

    REQUIRE(got_snapshot.load());
    CHECK(session.stats().connections == 1);
    CHECK(session.stats().parse_failures == 0);

    // The book must be usable and sane: two-sided and not crossed.
    CHECK(book.is_valid());
    REQUIRE(book.best_bid().has_value());
    REQUIRE(book.best_ask().has_value());
    CHECK_FALSE(book.is_crossed());
}

TEST_CASE("the session refuses to connect with a bad signature") {
    // Proves authentication is actually being enforced rather than the venue
    // accepting anything. A key that is valid but not registered must fail.
    auto signer = eventbook::load_signer_from_environment();
    if (!signer) {
        WARN("skipped: no credentials");
        return;
    }

    const auto throwaway = eventbook::testing::generate_rsa_key_pair();
    auto wrong_key = eventbook::RsaPrivateKey::from_pem(throwaway.private_pem);
    REQUIRE(wrong_key.has_value());

    eventbook::WsSessionConfig config;
    config.subscription.market_ticker = eventbook::MarketTicker{"KXFED-27APR-T4.25"};
    config.max_reconnect_attempts = 1;
    config.initial_backoff = std::chrono::milliseconds{50};

    eventbook::WebSocketSession session{
        config, eventbook::RequestSigner{signer->key_id(), *std::move(wrong_key)}};

    std::atomic<bool> saw_event{false};
    session.on_event([&](const eventbook::MarketEvent&) { saw_event = true; });

    std::thread guard{[&] {
        std::this_thread::sleep_for(std::chrono::seconds{10});
        session.stop();
    }};
    session.run();
    guard.join();

    CHECK_FALSE(saw_event.load());
    CHECK(session.stats().connections == 0);
}
