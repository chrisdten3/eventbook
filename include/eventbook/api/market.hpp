#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "eventbook/common/identifiers.hpp"
#include "eventbook/common/price.hpp"
#include "eventbook/common/quantity.hpp"
#include "eventbook/common/result.hpp"
#include "eventbook/common/time.hpp"

namespace eventbook {

/// Lifecycle state as reported in a market's `status` FIELD.
///
/// Beware: the field vocabulary and the `status` QUERY PARAMETER vocabulary are
/// different, which is a genuine trap rather than a pedantic note. Observed
/// against the live API:
///
///     query status=unopened -> field "initialized"
///     query status=open     -> field "active"
///     query status=closed   -> field "closed" or "determined"
///     query status=settled  -> field "finalized"
///
/// Four query values, five field values. MarketStatusFilter in market_query.hpp
/// covers the query side, so the two can never be confused for one another.
enum class MarketStatus {
    Initialized,
    Active,
    Closed,
    Determined,
    Finalized,
    Unknown,  ///< a value this build does not recognize
};

[[nodiscard]] MarketStatus market_status_from_string(std::string_view text);
[[nodiscard]] std::string_view to_string(MarketStatus status);

/// Metadata for one market, as returned by the REST market endpoints.
///
/// The quote fields are a SNAPSHOT taken when the request was served, not a
/// live order book. They are useful for scouting a research universe -- ranking
/// candidate series by spread, depth, and activity -- and must never be treated
/// as book state. The real book arrives over the WebSocket in M2.
///
/// Unknown JSON fields are ignored rather than rejected. Kalshi adds fields
/// over time, and a collector that refuses to parse a market because a new
/// field appeared would take the recorder down for no reason. Fields we DO
/// depend on are required, and their absence is an explicit error.
struct Market {
    MarketTicker ticker;
    EventTicker event_ticker;
    std::string market_type;  ///< "binary" for every standard contract
    MarketStatus status{MarketStatus::Unknown};

    std::string price_level_structure;  ///< "linear_cent", "deci_cent", ...
    std::vector<PriceRange> price_ranges;

    ExchangeTimestamp open_time;
    ExchangeTimestamp close_time;
    std::optional<ExchangeTimestamp> expected_expiration_time;

    /// Present only on multivariate ("combo") markets, which resolve against a
    /// basket of other markets rather than a single proposition. Observed live:
    /// this field, `mve_selected_legs`, and `is_provisional` appear together on
    /// combo markets and are entirely absent from plain binary ones.
    std::optional<std::string> mve_collection_ticker;

    Price yes_bid;
    Price yes_ask;
    Quantity yes_bid_size;
    Quantity yes_ask_size;
    Price last_price;

    Quantity volume;
    Quantity volume_24h;
    Quantity open_interest;
    bool can_close_early{};
};

/// Whether this market resolves against a basket rather than one proposition.
///
/// AGENTS.md excludes multivariate markets from the version-one universe. The
/// signal is the presence of a non-empty `mve_collection_ticker`, not anything
/// parsed out of the ticker text.
[[nodiscard]] bool is_multivariate(const Market& market);

enum class MarketParseErrorKind {
    MalformedJson,
    NotAnObject,
    MissingField,
    WrongFieldType,
    InvalidPrice,
    InvalidQuantity,
    InvalidTimestamp,
    InvalidPriceRange,
};

[[nodiscard]] std::string_view to_string(MarketParseErrorKind kind);

/// A parse failure, naming the field that caused it.
///
/// The field name is not decoration. AGENTS.md requires per-session counts of
/// parse failures, and "some market failed to parse" is not actionable while
/// "close_time failed on 4,102 markets" points straight at a schema change.
struct MarketParseError {
    MarketParseErrorKind kind{};
    std::string field;

    [[nodiscard]] friend bool operator==(const MarketParseError&,
                                         const MarketParseError&) = default;
};

/// One page of results, as returned by GET /markets.
struct MarketPage {
    std::vector<Market> markets;
    std::string cursor;  ///< empty when there are no further pages
};

/// Parse a single market object.
///
/// Takes text rather than a JSON value so that nlohmann/json stays out of this
/// header, for the same reason Boost stays out of the transport header: callers
/// should not inherit a large dependency just to name a type.
[[nodiscard]] Result<Market, MarketParseError> parse_market(std::string_view json_text);

/// Parse a `{"markets": [...], "cursor": "..."}` response body.
[[nodiscard]] Result<MarketPage, MarketParseError> parse_market_page(std::string_view json_text);

/// Parse a `{"market": {...}}` body, as returned by GET /markets/{ticker}.
///
/// A different shape from the list endpoint: the single-market response wraps
/// the object rather than returning it bare, so handing this body to
/// parse_market would fail on every field at once.
[[nodiscard]] Result<Market, MarketParseError> parse_market_response(std::string_view json_text);

}  // namespace eventbook
