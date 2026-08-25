#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "eventbook/common/decimal.hpp"

namespace eventbook {

/// A price on the YES scale, stored as an exact count of $0.0001 units.
///
/// Kalshi sends prices as decimal strings in `*_dollars` fields with up to four
/// decimal places, so 10,000 units == $1.00 represents every legal wire value
/// exactly. Nothing in this project may hold a canonical price as a `double`:
/// binary floating point cannot represent $0.01, which would break exact
/// equality -- and Price is used as an order-book map key.
///
/// Tick size is deliberately *not* part of this type. A market's legal grid
/// comes from its own `price_ranges` bands, whose steps range from $0.01 down
/// to $0.0001, so "one tick" is a per-market fact, not a constant.
///
/// This is a plain aggregate: `Price{1200}` works and the type is trivially
/// copyable and comparable. But it is a distinct type from `std::int32_t`, so
/// passing a quantity where a price belongs is a compile error rather than a
/// silently corrupted book.
struct Price {
    std::int32_t units{};

    static constexpr std::int32_t kUnitsPerDollar = 10'000;
    static constexpr int kFractionalDigits = 4;

    /// A defaulted <=> also implicitly defaults ==, giving all six comparisons.
    [[nodiscard]] friend constexpr auto operator<=>(const Price&, const Price&) = default;
};

/// A signed displacement between two prices, in the same $0.0001 units.
///
/// Distinct from Price because the difference of two prices is not itself a
/// market price: it can be negative, and summing two displacements is meaningful
/// where summing two prices is not. Spread, microprice displacement, and
/// implementation shortfall are all PriceDelta, which means their sign
/// conventions are checked by the compiler instead of asserted in a comment.
struct PriceDelta {
    std::int32_t units{};

    [[nodiscard]] friend constexpr auto operator<=>(const PriceDelta&, const PriceDelta&) = default;
};

[[nodiscard]] constexpr PriceDelta operator-(Price left, Price right) {
    return PriceDelta{left.units - right.units};
}

[[nodiscard]] constexpr Price operator+(Price price, PriceDelta delta) {
    return Price{price.units + delta.units};
}

[[nodiscard]] constexpr Price operator-(Price price, PriceDelta delta) {
    return Price{price.units - delta.units};
}

[[nodiscard]] constexpr PriceDelta operator+(PriceDelta left, PriceDelta right) {
    return PriceDelta{left.units + right.units};
}

[[nodiscard]] constexpr PriceDelta operator-(PriceDelta delta) {
    return PriceDelta{-delta.units};
}

/// Settlement bounds of a binary contract: the losing side pays $0, the winning
/// side receives $1, so no YES price outside [$0.0000, $1.0000] is meaningful.
inline constexpr Price kMinYesPrice{0};
inline constexpr Price kMaxYesPrice{Price::kUnitsPerDollar};

/// Whether `price` lies within those settlement bounds.
///
/// A bounds check only. It says nothing about whether the price sits on a given
/// market's legal tick grid, which requires that market's `price_ranges`.
[[nodiscard]] constexpr bool is_valid_yes_price(Price price) {
    return price >= kMinYesPrice && price <= kMaxYesPrice;
}

/// Convert a NO-side price to the equivalent YES-side price.
///
/// A binary contract pays $1.00 to exactly one side, so "no at $0.30" names the
/// same book level as "yes at $0.70". The orderbook channel reports no-side
/// levels in no-leg pricing unless the subscription sets `use_yes_price: true`;
/// normalizing every level onto the YES scale is what lets one book type hold
/// both sides of the market.
///
/// Precondition: is_valid_yes_price(no_price).
[[nodiscard]] constexpr Price yes_price_from_no(Price no_price) {
    return Price{Price::kUnitsPerDollar - no_price.units};
}

/// Parse a `*_dollars` wire field, e.g. "0.1200", into a Price.
///
/// Lexical only: "3.5000" parses successfully even though it is not a legal YES
/// price. Callers that need the domain guarantee must also check
/// is_valid_yes_price(), which keeps "is this a number" and "is this a price"
/// as separate, separately testable questions.
[[nodiscard]] Result<Price, DecimalParseError> parse_price(std::string_view dollars);

/// Render a Price in canonical wire form, always with four decimal places.
[[nodiscard]] std::string format_price(Price price);

/// One band of a market's legal price grid.
///
/// The venue publishes these per market, and the tick genuinely varies: across
/// 4,000 live markets exactly two grids exist, `linear_cent` stepping $0.0100
/// and `deci_cent` stepping $0.0010, in a near-even split. This is why
/// AGENTS.md forbids assuming a one-cent tick and why Price stores $0.0001
/// units rather than ticks.
///
/// `step` is a PriceDelta, not a Price: a tick is the distance between two
/// adjacent levels, not a level itself.
///
/// This lives beside Price rather than beside the REST market model because
/// two unrelated subsystems need it -- market eligibility and the order book --
/// and an order book has no business including a REST response type.
struct PriceRange {
    Price start;
    Price end;
    PriceDelta step;

    [[nodiscard]] friend constexpr bool operator==(const PriceRange&, const PriceRange&) = default;
};

/// Whether a price sits on a market's legal tick grid.
///
/// A price outside every band, or inside one but off its step, is not a price
/// the venue can quote. The order book uses this to reject deltas at impossible
/// prices, which is one of the two ways a desynchronized feed reveals itself.
[[nodiscard]] bool is_on_price_grid(Price price, const std::vector<PriceRange>& ranges);

}  // namespace eventbook
