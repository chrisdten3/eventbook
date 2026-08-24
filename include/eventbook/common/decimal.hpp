#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "eventbook/common/result.hpp"

namespace eventbook {

/// Why a decimal string could not be converted to a fixed-point integer.
enum class DecimalParseError {
    Empty,                    ///< no characters at all
    MissingDigits,            ///< ".5", "1.", "-", "abc" -- a required digit run is absent
    InvalidCharacter,         ///< trailing or interior junk: "1 ", "1e-2", "1.2.3"
    NegativeNotAllowed,       ///< a leading '-' on a field whose domain is nonnegative
    TooManyFractionalDigits,  ///< more precision than the field's scale can represent
    Overflow,                 ///< the scaled value does not fit in std::int64_t
};

[[nodiscard]] std::string_view to_string(DecimalParseError error);

/// Convert a decimal string into an integer scaled by 10^fractional_digits.
///
/// This is the only place in the project that turns wire text into money-like
/// integers, so it is deliberately strict: no surrounding whitespace, no leading
/// '+', no exponent notation, and above all no silent rounding. A value carrying
/// more precision than the field's scale is an error rather than something to
/// round away, because rounding here would quietly corrupt values that the order
/// book later compares for exact equality and uses as map keys.
///
/// Parsing is purely *lexical*. Whether the result is a legal price or quantity
/// for a particular market is a separate, domain-level question -- see
/// is_valid_yes_price() and, later, the per-market `price_ranges` grid.
///
/// Precondition: 0 <= fractional_digits <= 9.
[[nodiscard]] Result<std::int64_t, DecimalParseError> parse_scaled_decimal(std::string_view text,
                                                                           int fractional_digits,
                                                                           bool allow_negative);

/// Inverse of parse_scaled_decimal.
///
/// Always emits exactly `fractional_digits` digits after the point, matching the
/// canonical wire representation, so format(parse(text)) == text for any text
/// already in canonical form.
[[nodiscard]] std::string format_scaled_decimal(std::int64_t units, int fractional_digits);

}  // namespace eventbook
