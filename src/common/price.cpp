#include "eventbook/common/price.hpp"

#include <limits>

namespace eventbook {

Result<Price, DecimalParseError> parse_price(std::string_view dollars) {
    const auto units = parse_scaled_decimal(dollars, Price::kFractionalDigits,
                                            /*allow_negative=*/false);
    if (!units) {
        return Failure{units.error()};
    }

    // parse_scaled_decimal works in int64 so a single implementation can serve
    // every scale in the project. Narrowing to the int32 that Price stores is a
    // separate step and needs its own range check -- a static_cast here would
    // turn an out-of-range price into a plausible-looking one.
    constexpr auto kMin = static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::min());
    constexpr auto kMax = static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::max());
    if (*units < kMin || *units > kMax) {
        return Failure{DecimalParseError::Overflow};
    }

    return Price{static_cast<std::int32_t>(*units)};
}

std::string format_price(Price price) {
    return format_scaled_decimal(price.units, Price::kFractionalDigits);
}

}  // namespace eventbook
