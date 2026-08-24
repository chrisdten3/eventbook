#pragma once

#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

#include "eventbook/common/decimal.hpp"

namespace eventbook {

/// A resting size, stored as an exact count of 0.01-contract units.
///
/// Kalshi sends sizes in `*_fp` fields with two decimal places, so 100 units
/// == 1 contract represents every legal wire value exactly.
///
/// The width differs from Price on purpose. A Price is hard-bounded at 10,000
/// units by settlement and is never accumulated -- weighted averages such as
/// microprice divide back into the same range. A Quantity has no upper bound
/// and is summed constantly: L5 depth, rolling trade volume, the queue-ahead
/// counter in the execution simulator. An int32 holds a single level comfortably
/// (21.4M contracts) but could wrap inside an accumulator, and signed overflow
/// is undefined behaviour that only the dev preset's UBSan would catch. Four
/// extra bytes per level is not a cost worth that risk.
struct Quantity {
    std::int64_t units{};

    static constexpr std::int64_t kUnitsPerContract = 100;
    static constexpr int kFractionalDigits = 2;

    [[nodiscard]] friend constexpr auto operator<=>(const Quantity&, const Quantity&) = default;
};

/// A signed change in size, in the same 0.01-contract units.
///
/// This is exactly what the `delta_fp` field of an orderbook_delta carries; the
/// documented example includes "-54.00". Keeping it distinct from Quantity is
/// what makes "a resting size is never negative" expressible at all.
struct QuantityDelta {
    std::int64_t units{};

    [[nodiscard]] friend constexpr auto operator<=>(const QuantityDelta&,
                                                    const QuantityDelta&) = default;
};

/// The difference of two sizes is a signed change, not a size.
[[nodiscard]] constexpr QuantityDelta operator-(Quantity left, Quantity right) {
    return QuantityDelta{left.units - right.units};
}

/// Summing sizes across levels: depth aggregation, never a book mutation.
[[nodiscard]] constexpr Quantity operator+(Quantity left, Quantity right) {
    return Quantity{left.units + right.units};
}

[[nodiscard]] constexpr QuantityDelta operator+(QuantityDelta left, QuantityDelta right) {
    return QuantityDelta{left.units + right.units};
}

[[nodiscard]] constexpr QuantityDelta operator-(QuantityDelta delta) {
    return QuantityDelta{-delta.units};
}

/// Why a delta could not be applied to a resting size.
enum class QuantityDeltaError {
    WouldGoNegative,  ///< the level does not hold enough size to remove
    Overflow,         ///< the resulting size does not fit in std::int64_t
};

[[nodiscard]] std::string_view to_string(QuantityDeltaError error);

/// Apply a book delta to a resting size.
///
/// Note what is deliberately *absent*: there is no `operator+(Quantity,
/// QuantityDelta)`. The only way to combine the two is through this function,
/// whose Result the caller cannot quietly ignore. Providing the operator as
/// well would leave an unchecked path around the invariant, which is the same
/// as not having the invariant.
///
/// A delta that would drive a level below zero is not a bad *number*. It is
/// evidence that our book state is wrong: a missed message, a stale snapshot, a
/// sequence gap we failed to notice. This function reports only that the
/// transition is impossible. Deciding what to do about it -- mark the book
/// invalid, increment a counter, request a fresh snapshot -- is the order book's
/// policy and belongs there, not in arithmetic.
[[nodiscard]] constexpr Result<Quantity, QuantityDeltaError> apply_delta(Quantity quantity,
                                                                         QuantityDelta delta) {
    if (delta.units < 0) {
        // quantity.units >= 0 and delta.units < 0, so the sum lies strictly
        // between the two operands and cannot overflow. Safe to compute first.
        if (quantity.units + delta.units < 0) {
            return Failure{QuantityDeltaError::WouldGoNegative};
        }
    } else if (quantity.units > std::numeric_limits<std::int64_t>::max() - delta.units) {
        return Failure{QuantityDeltaError::Overflow};
    }
    return Quantity{quantity.units + delta.units};
}

/// Parse an `*_fp` size field, e.g. "54.00", into a Quantity.
///
/// Rejects a leading '-': a resting size is nonnegative by construction, so the
/// invariant is enforced at the point text enters the system rather than being
/// checked later and hopefully everywhere.
[[nodiscard]] Result<Quantity, DecimalParseError> parse_quantity(std::string_view contracts);

/// Parse a `delta_fp` field, which is signed -- "-54.00" is a documented value.
[[nodiscard]] Result<QuantityDelta, DecimalParseError> parse_quantity_delta(
    std::string_view contracts);

/// Render in canonical wire form, always with two decimal places.
[[nodiscard]] std::string format_quantity(Quantity quantity);

[[nodiscard]] std::string format_quantity_delta(QuantityDelta delta);

}  // namespace eventbook
