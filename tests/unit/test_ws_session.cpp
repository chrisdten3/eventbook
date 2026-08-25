#include "eventbook/api/ws_session.hpp"

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include "test_rsa_key.hpp"

#include <chrono>
#include <string>

using eventbook::build_subscribe_command;
using eventbook::KalshiEnvironment;
using eventbook::kWebSocketPath;
using eventbook::MarketTicker;
using eventbook::PriceConvention;
using eventbook::reconnect_backoff;
using eventbook::websocket_host;
using eventbook::WebSocketSession;
using eventbook::WsSubscription;

using namespace std::chrono_literals;

TEST_CASE("the websocket lives on a different subdomain from REST") {
    // REST is external-api.kalshi.com; this is external-api-ws. Production is
    // .com and demo .co. Four near-identical hostnames, which is exactly why
    // they are produced from an enum rather than typed at call sites.
    CHECK(websocket_host(KalshiEnvironment::Production) == "external-api-ws.kalshi.com");
    CHECK(websocket_host(KalshiEnvironment::Demo) == "external-api-ws.demo.kalshi.co");
    CHECK(kWebSocketPath == "/trade-api/ws/v2");
}

TEST_CASE("the subscribe command matches the documented shape") {
    WsSubscription subscription;
    subscription.market_ticker = MarketTicker{"KXFED-27APR-T4.25"};
    subscription.channels = {"orderbook_delta", "trade"};
    subscription.use_yes_price = true;

    const auto command = nlohmann::json::parse(build_subscribe_command(1, subscription));

    CHECK(command["id"] == 1);
    CHECK(command["cmd"] == "subscribe");
    CHECK(command["params"]["market_ticker"] == "KXFED-27APR-T4.25");
    CHECK(command["params"]["channels"] == nlohmann::json::array({"orderbook_delta", "trade"}));

    // use_yes_price belongs in params, and is sent explicitly rather than
    // relying on a default that may change.
    CHECK(command["params"]["use_yes_price"] == true);
}

TEST_CASE("use_yes_price is sent as a boolean, not a string") {
    WsSubscription subscription;
    subscription.market_ticker = MarketTicker{"X"};
    subscription.use_yes_price = false;

    const auto command = nlohmann::json::parse(build_subscribe_command(7, subscription));
    REQUIRE(command["params"]["use_yes_price"].is_boolean());
    CHECK(command["params"]["use_yes_price"] == false);
    CHECK(command["id"] == 7);
}

TEST_CASE("backoff doubles and then holds at the cap") {
    CHECK(reconnect_backoff(0, 500ms, 30'000ms) == 500ms);
    CHECK(reconnect_backoff(1, 500ms, 30'000ms) == 1'000ms);
    CHECK(reconnect_backoff(2, 500ms, 30'000ms) == 2'000ms);
    CHECK(reconnect_backoff(6, 500ms, 30'000ms) == 30'000ms);

    // A large attempt count must clamp rather than overflow on the way there.
    CHECK(reconnect_backoff(1'000'000, 500ms, 30'000ms) == 30'000ms);
}

TEST_CASE("backoff handles degenerate configuration") {
    CHECK(reconnect_backoff(5, 0ms, 30'000ms) == 0ms);
    // An initial delay already past the cap is clamped, not doubled.
    CHECK(reconnect_backoff(3, 60'000ms, 30'000ms) == 30'000ms);
}

TEST_CASE("the session derives its parse convention from the subscription") {
    // The subscribe flag and the normalizer's convention must agree; disagreeing
    // would silently mirror every ask. Deriving one from the other makes that
    // impossible rather than merely unlikely, so this asserts the actual
    // derivation rather than the flag it comes from.
    const auto build = [](bool use_yes_price) {
        eventbook::WsSessionConfig config;
        config.subscription.market_ticker = MarketTicker{"X"};
        config.subscription.use_yes_price = use_yes_price;

        auto key = eventbook::RsaPrivateKey::from_pem(
            eventbook::testing::shared_rsa_key_pair().private_pem);
        REQUIRE(key.has_value());
        return WebSocketSession{config, eventbook::RequestSigner{"unused", *std::move(key)}};
    };

    CHECK(build(true).price_convention() == PriceConvention::YesLegPricing);
    CHECK(build(false).price_convention() == PriceConvention::NoLegPricing);
}

TEST_CASE("the default subscription follows the project's conventions") {
    const WsSubscription defaults;
    // AGENTS.md: subscribe on the YES-price scale explicitly.
    CHECK(defaults.use_yes_price);
    // Order book and trades are what M2 requires; user channels are absent, and
    // nothing here can reach an order-entry endpoint.
    CHECK(defaults.channels == std::vector<std::string>{"orderbook_delta", "trade"});
}
