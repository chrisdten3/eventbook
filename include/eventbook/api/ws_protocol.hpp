#pragma once

#include <string>
#include <string_view>

#include "eventbook/common/identifiers.hpp"
#include "eventbook/common/result.hpp"
#include "eventbook/data/events.hpp"

namespace eventbook {

/// Which price scale the venue is using for NO-side orderbook entries.
///
/// The orderbook channel reports NO-side levels in "no-leg pricing" by default:
/// a NO bid at $0.30 arrives as `0.3000`. Subscribing with `use_yes_price:
/// true` asks for both sides on the YES scale instead, so the same level
/// arrives as `0.7000`.
///
/// This is a subscription-time decision the messages themselves do not restate,
/// so the normalizer must be told which one is in force. Guessing from the
/// values is impossible: `0.3000` is a legal price under either reading, and
/// getting it wrong silently mirrors the entire ask side of every book.
///
/// AGENTS.md directs subscribing with `use_yes_price: true` explicitly rather
/// than relying on a default that may change. Both are supported here so that a
/// default change cannot break the recorder.
enum class PriceConvention {
    NoLegPricing,   ///< default: NO-side prices are NO prices, reflected here
    YesLegPricing,  ///< use_yes_price: true, prices already on the YES scale
};

enum class WsParseErrorKind {
    MalformedJson,
    NotAnObject,
    MissingField,
    WrongFieldType,
    InvalidPrice,
    InvalidQuantity,
    InvalidSide,
    MalformedLevel,
};

[[nodiscard]] std::string_view to_string(WsParseErrorKind kind);

/// A parse failure, naming the field responsible.
///
/// Same reasoning as MarketParseError: a per-session count of "some message
/// failed" is not actionable, while "delta_fp failed 4,102 times" identifies a
/// schema change immediately.
struct WsParseError {
    WsParseErrorKind kind{};
    std::string field;

    [[nodiscard]] friend bool operator==(const WsParseError&, const WsParseError&) = default;
};

/// Convert a NO-side price to the equivalent level on the YES scale.
///
/// Under YesLegPricing the venue has already done this, so the price passes
/// through untouched.
[[nodiscard]] Price yes_price_from_book_entry(Price published, BookSide side,
                                              PriceConvention convention);

/// Normalize one raw WebSocket text frame into a MarketEvent.
///
/// Takes text rather than a JSON value so nlohmann/json stays out of this
/// header, matching how the REST market parser and the Beast transport are
/// arranged.
[[nodiscard]] Result<MarketEvent, WsParseError> parse_ws_message(std::string_view json_text,
                                                                 PriceConvention convention);

}  // namespace eventbook
