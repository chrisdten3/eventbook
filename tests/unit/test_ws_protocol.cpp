#include "eventbook/api/ws_protocol.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <variant>

#include "fixtures.hpp"

using eventbook::BookDelta;
using eventbook::BookSide;
using eventbook::BookSnapshot;
using eventbook::MarketEvent;
using eventbook::parse_ws_message;
using eventbook::Price;
using eventbook::PriceConvention;
using eventbook::PublicTrade;
using eventbook::Quantity;
using eventbook::QuantityDelta;
using eventbook::SequenceNumber;
using eventbook::StreamError;
using eventbook::SubscriptionAck;
using eventbook::SubscriptionId;
using eventbook::TradeSide;
using eventbook::UnhandledMessage;
using eventbook::WsParseErrorKind;
using eventbook::yes_price_from_book_entry;
using eventbook::testing::read_fixture;

namespace {

MarketEvent parsed(std::string_view text,
                   PriceConvention convention = PriceConvention::NoLegPricing) {
    auto event = parse_ws_message(text, convention);
    REQUIRE(event.has_value());
    return *event;
}

WsParseErrorKind rejected(std::string_view text) {
    const auto event = parse_ws_message(text, PriceConvention::NoLegPricing);
    REQUIRE_FALSE(event.has_value());
    return event.error().kind;
}

}  // namespace

TEST_CASE("a NO bid becomes a YES ask") {
    // The single conversion everything downstream depends on. "Buy NO at $0.30"
    // and "sell YES at $0.70" are the same order.
    CHECK(yes_price_from_book_entry(Price{3000}, BookSide::Ask, PriceConvention::NoLegPricing) ==
          Price{7000});

    // A bid is quoted in YES dollars under either convention, so it never moves.
    CHECK(yes_price_from_book_entry(Price{3000}, BookSide::Bid, PriceConvention::NoLegPricing) ==
          Price{3000});
    CHECK(yes_price_from_book_entry(Price{3000}, BookSide::Bid, PriceConvention::YesLegPricing) ==
          Price{3000});

    // Under use_yes_price the venue has already reflected it.
    CHECK(yes_price_from_book_entry(Price{3000}, BookSide::Ask, PriceConvention::YesLegPricing) ==
          Price{3000});
}

TEST_CASE("a snapshot normalizes both sides onto the YES scale") {
    const auto event = parsed(read_fixture("ws_orderbook_snapshot.json"));
    const auto* snapshot = std::get_if<BookSnapshot>(&event);
    REQUIRE(snapshot != nullptr);

    CHECK(snapshot->market_ticker.value == "FED-23DEC-T3.00");
    CHECK(snapshot->subscription == SubscriptionId{2});
    CHECK(snapshot->sequence == SequenceNumber{2});

    // YES entries are bids at their published price, best first.
    REQUIRE(snapshot->bids.size() == 2);
    CHECK(snapshot->bids[0].price == Price{2200});
    CHECK(snapshot->bids[0].quantity == Quantity{33300});
    CHECK(snapshot->bids[1].price == Price{800});

    // NO entries become asks at $1.00 minus their price. Note the reversal:
    // the LOWEST no price (0.5400) is the HIGHEST yes ask (0.4600), so the
    // published order is backwards and has to be re-sorted.
    REQUIRE(snapshot->asks.size() == 2);
    CHECK(snapshot->asks[0].price == Price{4400});  // from no 0.5600
    CHECK(snapshot->asks[0].quantity == Quantity{14600});
    CHECK(snapshot->asks[1].price == Price{4600});  // from no 0.5400
}

TEST_CASE("the book is not crossed after normalization") {
    const auto event = parsed(read_fixture("ws_orderbook_snapshot.json"));
    const auto* snapshot = std::get_if<BookSnapshot>(&event);
    REQUIRE(snapshot != nullptr);
    REQUIRE_FALSE(snapshot->bids.empty());
    REQUIRE_FALSE(snapshot->asks.empty());

    // Best bid strictly below best ask. Reflecting the wrong side, or failing
    // to reflect at all, produces a crossed book -- which is the cheapest
    // available signal that the convention was applied incorrectly.
    CHECK(snapshot->bids.front().price < snapshot->asks.front().price);
}

TEST_CASE("the price convention changes the book entirely, not slightly") {
    const auto text = read_fixture("ws_orderbook_snapshot.json");

    const auto no_leg = parsed(text, PriceConvention::NoLegPricing);
    const auto yes_leg = parsed(text, PriceConvention::YesLegPricing);

    const auto* under_no_leg = std::get_if<BookSnapshot>(&no_leg);
    const auto* under_yes_leg = std::get_if<BookSnapshot>(&yes_leg);
    REQUIRE(under_no_leg != nullptr);
    REQUIRE(under_yes_leg != nullptr);

    // Best ask $0.44 versus $0.54 from identical bytes. This is why the
    // convention is a required parameter and not inferred: 0.5400 is a legal
    // price under either reading, so nothing in the message reveals the error.
    CHECK(under_no_leg->asks.front().price == Price{4400});
    CHECK(under_yes_leg->asks.front().price == Price{5400});

    // Bids are unaffected either way.
    CHECK(under_no_leg->bids.front().price == under_yes_leg->bids.front().price);
}

TEST_CASE("an absent side is empty rather than an error") {
    const auto event = parsed(R"({"type":"orderbook_snapshot","sid":1,"seq":1,
        "msg":{"market_ticker":"X","yes_dollars_fp":[["0.5000","10.00"]]}})");
    const auto* snapshot = std::get_if<BookSnapshot>(&event);
    REQUIRE(snapshot != nullptr);
    CHECK(snapshot->bids.size() == 1);
    CHECK(snapshot->asks.empty());
}

TEST_CASE("a yes-side delta is a bid at the published price") {
    const auto event = parsed(read_fixture("ws_orderbook_delta.json"));
    const auto* delta = std::get_if<BookDelta>(&event);
    REQUIRE(delta != nullptr);

    CHECK(delta->market_ticker.value == "FED-23DEC-T3.00");
    CHECK(delta->subscription == SubscriptionId{2});
    CHECK(delta->sequence == SequenceNumber{3});
    CHECK(delta->side == BookSide::Bid);

    // "0.960" carries three decimals, not four. The decimal parser pads a short
    // fractional run out to the scale, so this is $0.9600 and not $0.0960.
    CHECK(delta->price == Price{9600});
    CHECK(delta->delta == QuantityDelta{-5400});

    REQUIRE(delta->exchange_time.has_value());
    CHECK(eventbook::epoch_micros(*delta->exchange_time) == 1'669'149'841'000'000);
}

TEST_CASE("a no-side delta is an ask, reflected onto the YES scale") {
    const std::string text = R"({"type":"orderbook_delta","sid":2,"seq":4,
        "msg":{"market_ticker":"X","price_dollars":"0.3000","delta_fp":"25.00","side":"no"}})";

    const auto no_leg = parsed(text, PriceConvention::NoLegPricing);
    const auto* reflected = std::get_if<BookDelta>(&no_leg);
    REQUIRE(reflected != nullptr);
    CHECK(reflected->side == BookSide::Ask);
    CHECK(reflected->price == Price{7000});
    // The quantity change keeps its sign through the side conversion.
    CHECK(reflected->delta == QuantityDelta{2500});

    const auto yes_leg = parsed(text, PriceConvention::YesLegPricing);
    const auto* direct = std::get_if<BookDelta>(&yes_leg);
    REQUIRE(direct != nullptr);
    CHECK(direct->price == Price{3000});
}

TEST_CASE("a trade carries direction on the YES scale") {
    const auto event = parsed(read_fixture("ws_trade.json"));
    const auto* trade = std::get_if<PublicTrade>(&event);
    REQUIRE(trade != nullptr);

    CHECK(trade->market_ticker.value == "FED-23DEC-T3.00");
    CHECK(trade->yes_price == Price{6700});
    CHECK(trade->quantity == Quantity{5400});
    CHECK(trade->taker_side == TradeSide::BuyYes);
    CHECK_FALSE(trade->is_block_trade);
}

TEST_CASE("a taker who bought NO has sold YES") {
    // Signed trade flow in M4 depends entirely on this mapping, and a sign
    // error there would invert every conclusion drawn from it.
    const auto event = parsed(R"({"type":"trade","sid":3,
        "msg":{"trade_id":"t","market_ticker":"X","yes_price_dollars":"0.6700",
        "count_fp":"1.00","taker_side":"no"}})");
    const auto* trade = std::get_if<PublicTrade>(&event);
    REQUIRE(trade != nullptr);
    CHECK(trade->taker_side == TradeSide::SellYes);
}

TEST_CASE("trade direction survives any one of the three taker fields") {
    // Accepting all three means a rename or removal costs us nothing. Direction
    // is useless if its sign is uncertain.
    for (const char* field : {"taker_side", "taker_outcome_side"}) {
        const std::string text = std::string{R"({"type":"trade","sid":3,
            "msg":{"trade_id":"t","market_ticker":"X","yes_price_dollars":"0.5000",
            "count_fp":"1.00",")"} +
                                 field + R"(":"no"}})";
        const auto event = parsed(text);
        const auto* trade = std::get_if<PublicTrade>(&event);
        INFO("field=" << field);
        REQUIRE(trade != nullptr);
        CHECK(trade->taker_side == TradeSide::SellYes);
    }

    // taker_book_side speaks bid/ask rather than yes/no, and bid is yes.
    const auto event = parsed(R"({"type":"trade","sid":3,
        "msg":{"trade_id":"t","market_ticker":"X","yes_price_dollars":"0.5000",
        "count_fp":"1.00","taker_book_side":"ask"}})");
    const auto* trade = std::get_if<PublicTrade>(&event);
    REQUIRE(trade != nullptr);
    CHECK(trade->taker_side == TradeSide::SellYes);
}

TEST_CASE("subscription acknowledgements carry the assigned sid") {
    const auto event =
        parsed(R"({"id":1,"type":"subscribed","msg":{"channel":"orderbook_delta","sid":7}})");
    const auto* ack = std::get_if<SubscriptionAck>(&event);
    REQUIRE(ack != nullptr);
    CHECK(ack->command_id == 1);
    CHECK(ack->channel == "orderbook_delta");
    CHECK(ack->subscription == SubscriptionId{7});
}

TEST_CASE("venue errors are events, not parse failures") {
    const auto event = parsed(
        R"({"id":123,"type":"error","msg":{"code":6,"msg":"Already subscribed",
        "market_ticker":"X-1"}})");
    const auto* failure = std::get_if<StreamError>(&event);
    REQUIRE(failure != nullptr);
    CHECK(failure->code == 6);
    CHECK(failure->message == "Already subscribed");
    REQUIRE(failure->market_ticker.has_value());
    CHECK(failure->market_ticker->value == "X-1");
}

TEST_CASE("an unrecognized message type is carried, not rejected") {
    // The venue adds channels over time. A recorder that stopped on an unknown
    // type would stop for no reason, so this is counted rather than fatal.
    const auto event = parsed(R"({"type":"ticker_v2","sid":9,"msg":{"anything":1}})");
    const auto* unhandled = std::get_if<UnhandledMessage>(&event);
    REQUIRE(unhandled != nullptr);
    CHECK(unhandled->type == "ticker_v2");
}

TEST_CASE("only events that carry a sequence report one") {
    // Acks and errors sit outside the per-subscription sequence, so gap
    // detection must skip them rather than read a missing seq as zero.
    CHECK(eventbook::sequence_of(parsed(read_fixture("ws_orderbook_delta.json"))).has_value());
    CHECK(eventbook::sequence_of(parsed(read_fixture("ws_orderbook_snapshot.json"))).has_value());
    CHECK_FALSE(eventbook::sequence_of(parsed(read_fixture("ws_trade.json"))).has_value());
    CHECK_FALSE(eventbook::sequence_of(
                    parsed(R"({"id":1,"type":"subscribed","msg":{"channel":"c","sid":1}})"))
                    .has_value());
}

TEST_CASE("malformed messages are reported with the offending field") {
    CHECK(rejected("not json") == WsParseErrorKind::MalformedJson);
    CHECK(rejected("[]") == WsParseErrorKind::NotAnObject);
    CHECK(rejected(R"({"sid":1})") == WsParseErrorKind::MissingField);
    CHECK(rejected(R"({"type":42})") == WsParseErrorKind::WrongFieldType);
    CHECK(rejected(R"({"type":"orderbook_delta","sid":1,"seq":1})") ==
          WsParseErrorKind::MissingField);

    const auto bad_side = parse_ws_message(
        R"({"type":"orderbook_delta","sid":1,"seq":1,
        "msg":{"market_ticker":"X","price_dollars":"0.5000","delta_fp":"1.00","side":"maybe"}})",
        PriceConvention::NoLegPricing);
    REQUIRE_FALSE(bad_side.has_value());
    CHECK(bad_side.error().kind == WsParseErrorKind::InvalidSide);
    CHECK(bad_side.error().field == "side");

    const auto bad_price = parse_ws_message(
        R"({"type":"orderbook_delta","sid":1,"seq":1,
        "msg":{"market_ticker":"X","price_dollars":"cheap","delta_fp":"1.00","side":"yes"}})",
        PriceConvention::NoLegPricing);
    REQUIRE_FALSE(bad_price.has_value());
    CHECK(bad_price.error().kind == WsParseErrorKind::InvalidPrice);
    CHECK(bad_price.error().field == "price_dollars");

    const auto bad_level = parse_ws_message(
        R"({"type":"orderbook_snapshot","sid":1,"seq":1,
        "msg":{"market_ticker":"X","yes_dollars_fp":[["0.5000"]]}})",
        PriceConvention::NoLegPricing);
    REQUIRE_FALSE(bad_level.has_value());
    CHECK(bad_level.error().kind == WsParseErrorKind::MalformedLevel);
}
