#include "eventbook/api/ws_protocol.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <optional>

namespace eventbook {
namespace {

using nlohmann::json;

/// Reads typed fields, remembering the first failure.
///
/// The same shape as the FieldReader in market.cpp, and deliberately a separate
/// one: the two error taxonomies differ, and unifying them would mean either a
/// template with a mapping policy or a lowest-common-denominator error that
/// loses the field-level detail both parsers depend on. If a third parser wants
/// this, that is the point to extract it properly.
class FieldReader {
public:
    explicit FieldReader(const json& object) : object_(object) {}

    [[nodiscard]] std::string string_field(const char* field) {
        const json* value = find(field, &json::is_string);
        return value != nullptr ? value->get<std::string>() : std::string{};
    }

    [[nodiscard]] std::int64_t integer_field(const char* field) {
        const json* value = find(field, &json::is_number_integer);
        return value != nullptr ? value->get<std::int64_t>() : 0;
    }

    [[nodiscard]] bool bool_field(const char* field, bool fallback = false) {
        if (!object_.contains(field)) {
            return fallback;
        }
        const json* value = find(field, &json::is_boolean);
        return value != nullptr ? value->get<bool>() : fallback;
    }

    [[nodiscard]] Price price_field(const char* field) {
        const json* value = find(field, &json::is_string);
        if (value == nullptr) {
            return Price{};
        }
        const auto price = parse_price(value->get_ref<const std::string&>());
        if (!price) {
            record(WsParseErrorKind::InvalidPrice, field);
            return Price{};
        }
        return *price;
    }

    [[nodiscard]] Quantity quantity_field(const char* field) {
        const json* value = find(field, &json::is_string);
        if (value == nullptr) {
            return Quantity{};
        }
        const auto quantity = parse_quantity(value->get_ref<const std::string&>());
        if (!quantity) {
            record(WsParseErrorKind::InvalidQuantity, field);
            return Quantity{};
        }
        return *quantity;
    }

    [[nodiscard]] QuantityDelta quantity_delta_field(const char* field) {
        const json* value = find(field, &json::is_string);
        if (value == nullptr) {
            return QuantityDelta{};
        }
        const auto delta = parse_quantity_delta(value->get_ref<const std::string&>());
        if (!delta) {
            record(WsParseErrorKind::InvalidQuantity, field);
            return QuantityDelta{};
        }
        return *delta;
    }

    /// The venue sends both `ts` (RFC 3339, deprecated) and `ts_ms`. Prefer the
    /// millisecond integer: one-second resolution is useless for anything this
    /// project measures.
    [[nodiscard]] std::optional<ExchangeTimestamp> exchange_time() {
        if (const auto position = object_.find("ts_ms");
            position != object_.end() && position->is_number_integer()) {
            return exchange_time_from_epoch_millis(position->get<std::int64_t>());
        }
        if (const auto position = object_.find("ts");
            position != object_.end() && position->is_string()) {
            const auto parsed = parse_rfc3339(position->get_ref<const std::string&>());
            if (parsed) {
                return *parsed;
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] std::optional<std::string> optional_string_field(const char* field) {
        const auto position = object_.find(field);
        if (position == object_.end() || !position->is_string()) {
            return std::nullopt;
        }
        return position->get<std::string>();
    }

    [[nodiscard]] bool ok() const {
        return !error_.has_value();
    }

    [[nodiscard]] const WsParseError& error() const {
        return *error_;
    }

    [[nodiscard]] const json& object() const {
        return object_;
    }

    void record(WsParseErrorKind kind, const char* field) {
        if (!error_.has_value()) {
            error_ = WsParseError{kind, field};
        }
    }

private:
    const json* find(const char* field, bool (json::*predicate)() const) {
        const auto position = object_.find(field);
        if (position == object_.end()) {
            record(WsParseErrorKind::MissingField, field);
            return nullptr;
        }
        if (!(*position.*predicate)()) {
            record(WsParseErrorKind::WrongFieldType, field);
            return nullptr;
        }
        return &(*position);
    }

    const json& object_;
    std::optional<WsParseError> error_;
};

/// Read a `[price, quantity]` pair as published by a snapshot.
bool read_level(const json& entry, BookLevel& level) {
    if (!entry.is_array() || entry.size() != 2 || !entry[0].is_string() || !entry[1].is_string()) {
        return false;
    }
    const auto price = parse_price(entry[0].get_ref<const std::string&>());
    const auto quantity = parse_quantity(entry[1].get_ref<const std::string&>());
    if (!price || !quantity) {
        return false;
    }
    level = BookLevel{*price, *quantity};
    return true;
}

/// Read one side of a snapshot. An absent array means an empty side, which is
/// normal for a market quoted on one side only.
bool read_levels(const json& message, const char* field, BookSide side, PriceConvention convention,
                 std::vector<BookLevel>& out) {
    const auto position = message.find(field);
    if (position == message.end() || position->is_null()) {
        return true;
    }
    if (!position->is_array()) {
        return false;
    }

    out.reserve(position->size());
    for (const auto& entry : *position) {
        BookLevel level;
        if (!read_level(entry, level)) {
            return false;
        }
        level.price = yes_price_from_book_entry(level.price, side, convention);
        out.push_back(level);
    }
    return true;
}

std::optional<BookSide> book_side_from_string(std::string_view text) {
    // The venue names sides by outcome, not by book position. `bid` is
    // equivalent to `yes` and `ask` to `no`, so both spellings are accepted.
    if (text == "yes" || text == "bid") {
        return BookSide::Bid;
    }
    if (text == "no" || text == "ask") {
        return BookSide::Ask;
    }
    return std::nullopt;
}

Result<MarketEvent, WsParseError> parse_snapshot(const json& root, const json& message,
                                                 PriceConvention convention) {
    FieldReader header{root};
    FieldReader body{message};

    BookSnapshot snapshot;
    snapshot.subscription = SubscriptionId{header.integer_field("sid")};
    snapshot.sequence = SequenceNumber{static_cast<std::uint64_t>(header.integer_field("seq"))};
    snapshot.market_ticker = MarketTicker{body.string_field("market_ticker")};

    if (!header.ok()) {
        return Failure{header.error()};
    }
    if (!body.ok()) {
        return Failure{body.error()};
    }

    // YES entries are bids and are already on the YES scale. NO entries are
    // offers to sell YES, and become asks.
    if (!read_levels(message, "yes_dollars_fp", BookSide::Bid, convention, snapshot.bids)) {
        return Failure{WsParseError{WsParseErrorKind::MalformedLevel, "yes_dollars_fp"}};
    }
    if (!read_levels(message, "no_dollars_fp", BookSide::Ask, convention, snapshot.asks)) {
        return Failure{WsParseError{WsParseErrorKind::MalformedLevel, "no_dollars_fp"}};
    }

    // Reflecting NO levels onto the YES scale reverses their order, so ordering
    // is imposed here rather than inherited: best bid first, best ask first.
    std::sort(
        snapshot.bids.begin(), snapshot.bids.end(),
        [](const BookLevel& left, const BookLevel& right) { return left.price > right.price; });
    std::sort(
        snapshot.asks.begin(), snapshot.asks.end(),
        [](const BookLevel& left, const BookLevel& right) { return left.price < right.price; });

    return MarketEvent{std::move(snapshot)};
}

Result<MarketEvent, WsParseError> parse_delta(const json& root, const json& message,
                                              PriceConvention convention) {
    FieldReader header{root};
    FieldReader body{message};

    BookDelta delta;
    delta.subscription = SubscriptionId{header.integer_field("sid")};
    delta.sequence = SequenceNumber{static_cast<std::uint64_t>(header.integer_field("seq"))};
    delta.market_ticker = MarketTicker{body.string_field("market_ticker")};

    const auto side = book_side_from_string(body.string_field("side"));
    if (!side.has_value()) {
        body.record(WsParseErrorKind::InvalidSide, "side");
    }

    const Price published = body.price_field("price_dollars");
    delta.delta = body.quantity_delta_field("delta_fp");
    delta.exchange_time = body.exchange_time();

    if (!header.ok()) {
        return Failure{header.error()};
    }
    if (!body.ok()) {
        return Failure{body.error()};
    }

    delta.side = *side;
    delta.price = yes_price_from_book_entry(published, delta.side, convention);
    return MarketEvent{std::move(delta)};
}

Result<MarketEvent, WsParseError> parse_trade(const json& root, const json& message) {
    FieldReader header{root};
    FieldReader body{message};

    PublicTrade trade;
    trade.subscription = SubscriptionId{header.integer_field("sid")};
    trade.market_ticker = MarketTicker{body.string_field("market_ticker")};
    trade.trade_id = body.string_field("trade_id");
    trade.yes_price = body.price_field("yes_price_dollars");
    trade.quantity = body.quantity_field("count_fp");
    trade.is_block_trade = body.bool_field("is_block_trade");
    trade.exchange_time = body.exchange_time();

    // The venue publishes the taker's direction three ways. Any one suffices,
    // and accepting all three means a field being renamed or dropped does not
    // cost us trade direction -- which is the input to signed trade flow, and
    // useless if its sign is uncertain.
    std::optional<BookSide> taker;
    for (const char* field : {"taker_side", "taker_outcome_side", "taker_book_side"}) {
        if (const auto text = body.optional_string_field(field)) {
            taker = book_side_from_string(*text);
            if (taker.has_value()) {
                break;
            }
        }
    }
    if (!taker.has_value()) {
        body.record(WsParseErrorKind::InvalidSide, "taker_side");
    }

    if (!header.ok()) {
        return Failure{header.error()};
    }
    if (!body.ok()) {
        return Failure{body.error()};
    }

    // A taker who bought NO has, in YES terms, sold.
    trade.taker_side = *taker == BookSide::Bid ? TradeSide::BuyYes : TradeSide::SellYes;
    return MarketEvent{std::move(trade)};
}

Result<MarketEvent, WsParseError> parse_subscribed(const json& root, const json& message) {
    FieldReader header{root};
    FieldReader body{message};

    SubscriptionAck ack;
    ack.command_id = header.integer_field("id");
    ack.channel = body.string_field("channel");
    ack.subscription = SubscriptionId{body.integer_field("sid")};

    if (!header.ok()) {
        return Failure{header.error()};
    }
    if (!body.ok()) {
        return Failure{body.error()};
    }
    return MarketEvent{std::move(ack)};
}

Result<MarketEvent, WsParseError> parse_error(const json& root, const json& message) {
    FieldReader header{root};
    FieldReader body{message};

    StreamError failure;
    failure.command_id = header.integer_field("id");
    failure.code = static_cast<int>(body.integer_field("code"));
    failure.message = body.string_field("msg");
    if (const auto ticker = body.optional_string_field("market_ticker")) {
        failure.market_ticker = MarketTicker{*ticker};
    }

    // The command id is optional in practice: some errors are not a reply to
    // anything we sent.
    if (!body.ok()) {
        return Failure{body.error()};
    }
    return MarketEvent{std::move(failure)};
}

}  // namespace

std::string_view to_string(WsParseErrorKind kind) {
    switch (kind) {
        case WsParseErrorKind::MalformedJson:
            return "malformed JSON";
        case WsParseErrorKind::NotAnObject:
            return "expected a JSON object";
        case WsParseErrorKind::MissingField:
            return "required field missing";
        case WsParseErrorKind::WrongFieldType:
            return "field has the wrong JSON type";
        case WsParseErrorKind::InvalidPrice:
            return "field is not a valid price";
        case WsParseErrorKind::InvalidQuantity:
            return "field is not a valid quantity";
        case WsParseErrorKind::InvalidSide:
            return "unrecognized side";
        case WsParseErrorKind::MalformedLevel:
            return "orderbook level is malformed";
    }
    return "unknown websocket parse error";
}

Price yes_price_from_book_entry(Price published, BookSide side, PriceConvention convention) {
    // A bid is a YES bid and is always quoted in YES dollars, on either
    // convention. Only the NO side ever needs reflecting.
    if (side == BookSide::Bid || convention == PriceConvention::YesLegPricing) {
        return published;
    }
    return yes_price_from_no(published);
}

Result<MarketEvent, WsParseError> parse_ws_message(std::string_view json_text,
                                                   PriceConvention convention) {
    const auto root = json::parse(json_text, nullptr, /*allow_exceptions=*/false);
    if (root.is_discarded()) {
        return Failure{WsParseError{WsParseErrorKind::MalformedJson, ""}};
    }
    if (!root.is_object()) {
        return Failure{WsParseError{WsParseErrorKind::NotAnObject, ""}};
    }

    const auto type_field = root.find("type");
    if (type_field == root.end()) {
        return Failure{WsParseError{WsParseErrorKind::MissingField, "type"}};
    }
    if (!type_field->is_string()) {
        return Failure{WsParseError{WsParseErrorKind::WrongFieldType, "type"}};
    }
    const auto& type = type_field->get_ref<const std::string&>();

    const auto message_field = root.find("msg");
    const bool has_message = message_field != root.end() && message_field->is_object();

    if (type == "orderbook_snapshot" || type == "orderbook_delta" || type == "trade" ||
        type == "subscribed" || type == "error") {
        if (!has_message) {
            return Failure{WsParseError{WsParseErrorKind::MissingField, "msg"}};
        }
    }

    if (type == "orderbook_snapshot") {
        return parse_snapshot(root, *message_field, convention);
    }
    if (type == "orderbook_delta") {
        return parse_delta(root, *message_field, convention);
    }
    if (type == "trade") {
        return parse_trade(root, *message_field);
    }
    if (type == "subscribed") {
        return parse_subscribed(root, *message_field);
    }
    if (type == "error") {
        return parse_error(root, *message_field);
    }

    // Not a failure: the venue adds message types, and a recorder that stopped
    // on an unrecognized one would stop for no reason.
    return MarketEvent{UnhandledMessage{type}};
}

}  // namespace eventbook
