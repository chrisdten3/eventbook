#include "eventbook/api/market.hpp"

#include <nlohmann/json.hpp>

namespace eventbook {
namespace {

using nlohmann::json;

/// Reads typed fields out of a JSON object, remembering the first failure.
///
/// The alternative is a check-and-return after every one of fifteen fields,
/// which buries the shape of a Market under error plumbing. This records the
/// first error and returns a harmless default for the rest, so the caller reads
/// every field and then asks once whether anything went wrong. Reporting the
/// FIRST error rather than the last matters: it names the field that actually
/// broke, not whatever happened to be read afterwards.
class FieldReader {
public:
    explicit FieldReader(const json& object) : object_(object) {}

    [[nodiscard]] std::string string_field(const char* field) {
        const json* value = find(field, &json::is_string, MarketParseErrorKind::WrongFieldType);
        return value != nullptr ? value->get<std::string>() : std::string{};
    }

    [[nodiscard]] bool bool_field(const char* field) {
        const json* value = find(field, &json::is_boolean, MarketParseErrorKind::WrongFieldType);
        return value != nullptr && value->get<bool>();
    }

    [[nodiscard]] Price price_field(const char* field) {
        const json* value = find(field, &json::is_string, MarketParseErrorKind::WrongFieldType);
        if (value == nullptr) {
            return Price{};
        }
        const auto price = parse_price(value->get_ref<const std::string&>());
        if (!price) {
            record(MarketParseErrorKind::InvalidPrice, field);
            return Price{};
        }
        return *price;
    }

    [[nodiscard]] Quantity quantity_field(const char* field) {
        const json* value = find(field, &json::is_string, MarketParseErrorKind::WrongFieldType);
        if (value == nullptr) {
            return Quantity{};
        }
        const auto quantity = parse_quantity(value->get_ref<const std::string&>());
        if (!quantity) {
            record(MarketParseErrorKind::InvalidQuantity, field);
            return Quantity{};
        }
        return *quantity;
    }

    [[nodiscard]] ExchangeTimestamp timestamp_field(const char* field) {
        const json* value = find(field, &json::is_string, MarketParseErrorKind::WrongFieldType);
        if (value == nullptr) {
            return ExchangeTimestamp{};
        }
        const auto timestamp = parse_rfc3339(value->get_ref<const std::string&>());
        if (!timestamp) {
            record(MarketParseErrorKind::InvalidTimestamp, field);
            return ExchangeTimestamp{};
        }
        return *timestamp;
    }

    /// Absent or null yields nullopt without recording an error. Present but
    /// unparseable is still an error -- silence is fine, nonsense is not.
    [[nodiscard]] std::optional<ExchangeTimestamp> optional_timestamp_field(const char* field) {
        if (!object_.contains(field) || object_.at(field).is_null()) {
            return std::nullopt;
        }
        return timestamp_field(field);
    }

    [[nodiscard]] std::optional<std::string> optional_string_field(const char* field) {
        if (!object_.contains(field) || object_.at(field).is_null()) {
            return std::nullopt;
        }
        return string_field(field);
    }

    [[nodiscard]] std::vector<PriceRange> price_ranges_field(const char* field) {
        const json* value = find(field, &json::is_array, MarketParseErrorKind::WrongFieldType);
        if (value == nullptr) {
            return {};
        }

        std::vector<PriceRange> ranges;
        ranges.reserve(value->size());
        for (const auto& element : *value) {
            if (!element.is_object()) {
                record(MarketParseErrorKind::InvalidPriceRange, field);
                return {};
            }
            FieldReader band{element};
            const Price start = band.price_field("start");
            const Price end = band.price_field("end");
            const Price step = band.price_field("step");
            if (!band.ok()) {
                record(MarketParseErrorKind::InvalidPriceRange, field);
                return {};
            }
            // step arrives as an absolute price string but denotes a distance
            // between adjacent levels, so it becomes a PriceDelta here.
            ranges.push_back(PriceRange{start, end, PriceDelta{step.units}});
        }
        return ranges;
    }

    [[nodiscard]] bool ok() const {
        return !error_.has_value();
    }

    [[nodiscard]] const MarketParseError& error() const {
        return *error_;
    }

private:
    const json* find(const char* field, bool (json::*predicate)() const,
                     MarketParseErrorKind mismatch) {
        const auto position = object_.find(field);
        if (position == object_.end()) {
            record(MarketParseErrorKind::MissingField, field);
            return nullptr;
        }
        if (!(*position.*predicate)()) {
            record(mismatch, field);
            return nullptr;
        }
        return &(*position);
    }

    void record(MarketParseErrorKind kind, const char* field) {
        if (!error_.has_value()) {
            error_ = MarketParseError{kind, field};
        }
    }

    const json& object_;
    std::optional<MarketParseError> error_;
};

Result<Market, MarketParseError> read_market(const json& object) {
    if (!object.is_object()) {
        return Failure{MarketParseError{MarketParseErrorKind::NotAnObject, ""}};
    }

    FieldReader reader{object};

    Market market;
    market.ticker = MarketTicker{reader.string_field("ticker")};
    market.event_ticker = EventTicker{reader.string_field("event_ticker")};
    market.market_type = reader.string_field("market_type");
    market.status = market_status_from_string(reader.string_field("status"));
    market.price_level_structure = reader.string_field("price_level_structure");
    market.price_ranges = reader.price_ranges_field("price_ranges");
    market.open_time = reader.timestamp_field("open_time");
    market.close_time = reader.timestamp_field("close_time");
    market.expected_expiration_time = reader.optional_timestamp_field("expected_expiration_time");
    market.mve_collection_ticker = reader.optional_string_field("mve_collection_ticker");
    market.yes_bid = reader.price_field("yes_bid_dollars");
    market.yes_ask = reader.price_field("yes_ask_dollars");
    market.yes_bid_size = reader.quantity_field("yes_bid_size_fp");
    market.yes_ask_size = reader.quantity_field("yes_ask_size_fp");
    market.last_price = reader.price_field("last_price_dollars");
    market.volume = reader.quantity_field("volume_fp");
    market.volume_24h = reader.quantity_field("volume_24h_fp");
    market.open_interest = reader.quantity_field("open_interest_fp");
    market.can_close_early = reader.bool_field("can_close_early");

    if (!reader.ok()) {
        return Failure{reader.error()};
    }
    return market;
}

}  // namespace

MarketStatus market_status_from_string(std::string_view text) {
    if (text == "initialized") {
        return MarketStatus::Initialized;
    }
    if (text == "active") {
        return MarketStatus::Active;
    }
    if (text == "closed") {
        return MarketStatus::Closed;
    }
    if (text == "determined") {
        return MarketStatus::Determined;
    }
    if (text == "finalized") {
        return MarketStatus::Finalized;
    }
    // An unrecognized status is deliberately not a parse failure. The venue may
    // introduce states, and a recorder should keep recording. The eligibility
    // filter rejects Unknown explicitly, so nothing unrecognized reaches the
    // research universe by accident.
    return MarketStatus::Unknown;
}

std::string_view to_string(MarketStatus status) {
    switch (status) {
        case MarketStatus::Initialized:
            return "initialized";
        case MarketStatus::Active:
            return "active";
        case MarketStatus::Closed:
            return "closed";
        case MarketStatus::Determined:
            return "determined";
        case MarketStatus::Finalized:
            return "finalized";
        case MarketStatus::Unknown:
            return "unknown";
    }
    return "unknown";
}

std::string_view to_string(MarketParseErrorKind kind) {
    switch (kind) {
        case MarketParseErrorKind::MalformedJson:
            return "malformed JSON";
        case MarketParseErrorKind::NotAnObject:
            return "expected a JSON object";
        case MarketParseErrorKind::MissingField:
            return "required field missing";
        case MarketParseErrorKind::WrongFieldType:
            return "field has the wrong JSON type";
        case MarketParseErrorKind::InvalidPrice:
            return "field is not a valid price";
        case MarketParseErrorKind::InvalidQuantity:
            return "field is not a valid quantity";
        case MarketParseErrorKind::InvalidTimestamp:
            return "field is not a valid RFC 3339 timestamp";
        case MarketParseErrorKind::InvalidPriceRange:
            return "price_ranges band is malformed";
    }
    return "unknown market parse error";
}

bool is_multivariate(const Market& market) {
    return market.mve_collection_ticker.has_value() && !market.mve_collection_ticker->empty();
}

Result<Market, MarketParseError> parse_market(std::string_view json_text) {
    // Non-throwing parse: a malformed payload is an ordinary operating
    // condition for a recorder, not an exceptional one.
    const auto document = json::parse(json_text, nullptr, /*allow_exceptions=*/false);
    if (document.is_discarded()) {
        return Failure{MarketParseError{MarketParseErrorKind::MalformedJson, ""}};
    }
    return read_market(document);
}

Result<Market, MarketParseError> parse_market_response(std::string_view json_text) {
    const auto document = json::parse(json_text, nullptr, /*allow_exceptions=*/false);
    if (document.is_discarded()) {
        return Failure{MarketParseError{MarketParseErrorKind::MalformedJson, ""}};
    }
    if (!document.is_object()) {
        return Failure{MarketParseError{MarketParseErrorKind::NotAnObject, ""}};
    }
    const auto market = document.find("market");
    if (market == document.end()) {
        return Failure{MarketParseError{MarketParseErrorKind::MissingField, "market"}};
    }
    return read_market(*market);
}

Result<MarketPage, MarketParseError> parse_market_page(std::string_view json_text) {
    const auto document = json::parse(json_text, nullptr, /*allow_exceptions=*/false);
    if (document.is_discarded()) {
        return Failure{MarketParseError{MarketParseErrorKind::MalformedJson, ""}};
    }
    if (!document.is_object()) {
        return Failure{MarketParseError{MarketParseErrorKind::NotAnObject, ""}};
    }

    const auto markets = document.find("markets");
    if (markets == document.end()) {
        return Failure{MarketParseError{MarketParseErrorKind::MissingField, "markets"}};
    }
    if (!markets->is_array()) {
        return Failure{MarketParseError{MarketParseErrorKind::WrongFieldType, "markets"}};
    }

    MarketPage page;
    page.markets.reserve(markets->size());
    for (const auto& element : *markets) {
        auto market = read_market(element);
        if (!market) {
            return Failure{market.error()};
        }
        page.markets.push_back(*market);
    }

    // A missing cursor means the same thing as an empty one: no further pages.
    if (const auto cursor = document.find("cursor");
        cursor != document.end() && cursor->is_string()) {
        page.cursor = cursor->get<std::string>();
    }
    return page;
}

}  // namespace eventbook
