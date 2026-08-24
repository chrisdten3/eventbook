#include "eventbook/common/quantity.hpp"

namespace eventbook {

std::string_view to_string(QuantityDeltaError error) {
    switch (error) {
        case QuantityDeltaError::WouldGoNegative:
            return "delta would drive the level below zero";
        case QuantityDeltaError::Overflow:
            return "resulting size out of range";
    }
    return "unknown quantity delta error";
}

Result<Quantity, DecimalParseError> parse_quantity(std::string_view contracts) {
    const auto units = parse_scaled_decimal(contracts, Quantity::kFractionalDigits,
                                            /*allow_negative=*/false);
    if (!units) {
        return Failure{units.error()};
    }

    // No range check here, unlike parse_price: the shared parser already works
    // in int64 and Quantity stores int64, so nothing is narrowed away.
    return Quantity{*units};
}

Result<QuantityDelta, DecimalParseError> parse_quantity_delta(std::string_view contracts) {
    const auto units = parse_scaled_decimal(contracts, Quantity::kFractionalDigits,
                                            /*allow_negative=*/true);
    if (!units) {
        return Failure{units.error()};
    }
    return QuantityDelta{*units};
}

std::string format_quantity(Quantity quantity) {
    return format_scaled_decimal(quantity.units, Quantity::kFractionalDigits);
}

std::string format_quantity_delta(QuantityDelta delta) {
    return format_scaled_decimal(delta.units, Quantity::kFractionalDigits);
}

}  // namespace eventbook
