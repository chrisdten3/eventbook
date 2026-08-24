#include "eventbook/common/decimal.hpp"

#include <fmt/format.h>

#include <cassert>
#include <cstddef>
#include <limits>

namespace eventbook {
namespace {

// Bounded so that 10^fractional_digits always fits comfortably in int64.
// Referenced only from assert(), which compiles out under NDEBUG in the
// release preset, so the attribute is what keeps that build warning-free.
[[maybe_unused]] constexpr int kMaxFractionalDigits = 9;

constexpr std::int64_t power_of_ten(int exponent) {
    std::int64_t result = 1;
    for (int i = 0; i < exponent; ++i) {
        result *= 10;
    }
    return result;
}

constexpr bool is_digit(char character) {
    return character >= '0' && character <= '9';
}

// Append one decimal digit to `value`, refusing to wrap around.
//
// The check happens *before* the multiply because signed integer overflow is
// undefined behaviour: there is no "after" to inspect, and the UBSan dev preset
// would abort rather than let us detect it.
constexpr bool accumulate_digit(std::int64_t& value, int digit) {
    constexpr std::int64_t kMax = std::numeric_limits<std::int64_t>::max();
    if (value > (kMax - digit) / 10) {
        return false;
    }
    value = value * 10 + digit;
    return true;
}

}  // namespace

std::string_view to_string(DecimalParseError error) {
    switch (error) {
        case DecimalParseError::Empty:
            return "empty string";
        case DecimalParseError::MissingDigits:
            return "missing required digits";
        case DecimalParseError::InvalidCharacter:
            return "invalid character";
        case DecimalParseError::NegativeNotAllowed:
            return "negative value not allowed";
        case DecimalParseError::TooManyFractionalDigits:
            return "too many fractional digits";
        case DecimalParseError::Overflow:
            return "value out of range";
    }
    return "unknown decimal parse error";
}

Result<std::int64_t, DecimalParseError> parse_scaled_decimal(std::string_view text,
                                                             int fractional_digits,
                                                             bool allow_negative) {
    assert(fractional_digits >= 0 && fractional_digits <= kMaxFractionalDigits);

    if (text.empty()) {
        return Failure{DecimalParseError::Empty};
    }

    std::size_t index = 0;
    bool negative = false;
    if (text[index] == '-') {
        if (!allow_negative) {
            return Failure{DecimalParseError::NegativeNotAllowed};
        }
        negative = true;
        ++index;
    }

    // Integer part. At least one digit is required, so ".5" and "-" are rejected
    // rather than quietly treated as zero.
    const std::size_t integer_begin = index;
    std::int64_t value = 0;
    while (index < text.size() && is_digit(text[index])) {
        if (!accumulate_digit(value, text[index] - '0')) {
            return Failure{DecimalParseError::Overflow};
        }
        ++index;
    }
    if (index == integer_begin) {
        return Failure{DecimalParseError::MissingDigits};
    }

    // Shift the integer part up by the scale so the fractional digits slot in
    // beneath it. Reusing accumulate_digit keeps the overflow check in one place.
    for (int i = 0; i < fractional_digits; ++i) {
        if (!accumulate_digit(value, 0)) {
            return Failure{DecimalParseError::Overflow};
        }
    }

    if (index < text.size() && text[index] == '.') {
        ++index;
        const std::size_t fraction_begin = index;
        while (index < text.size() && is_digit(text[index])) {
            ++index;
        }

        // Measure the digit run before accumulating it: "0.000...0001" with a
        // hundred digits must report TooManyFractionalDigits, not overflow while
        // being read.
        const std::size_t digit_count = index - fraction_begin;
        if (digit_count == 0) {
            return Failure{DecimalParseError::MissingDigits};
        }
        if (digit_count > static_cast<std::size_t>(fractional_digits)) {
            return Failure{DecimalParseError::TooManyFractionalDigits};
        }

        // digit_count <= fractional_digits <= 9, so this loop cannot overflow.
        std::int64_t fraction = 0;
        for (std::size_t i = fraction_begin; i < index; ++i) {
            fraction = fraction * 10 + (text[i] - '0');
        }
        // "0.5" at scale 4 means 5000 units, not 5: pad the run out to the scale.
        fraction *= power_of_ten(fractional_digits - static_cast<int>(digit_count));

        if (value > std::numeric_limits<std::int64_t>::max() - fraction) {
            return Failure{DecimalParseError::Overflow};
        }
        value += fraction;
    }

    // Anything left over is junk. Being strict here means a schema change shows
    // up as a loud parse failure instead of a silently truncated price.
    if (index != text.size()) {
        return Failure{DecimalParseError::InvalidCharacter};
    }

    return negative ? -value : value;
}

std::string format_scaled_decimal(std::int64_t units, int fractional_digits) {
    assert(fractional_digits >= 0 && fractional_digits <= kMaxFractionalDigits);

    if (fractional_digits == 0) {
        return fmt::format("{}", units);
    }

    // Work in unsigned magnitude so the sign survives a zero integer part (-50
    // at scale 100 must print "-0.50") and so negating the most negative int64
    // does not overflow.
    const bool negative = units < 0;
    const auto magnitude =
        negative ? ~static_cast<std::uint64_t>(units) + 1U : static_cast<std::uint64_t>(units);
    const auto scale = static_cast<std::uint64_t>(power_of_ten(fractional_digits));

    return fmt::format("{}{}.{:0{}}", negative ? "-" : "", magnitude / scale, magnitude % scale,
                       fractional_digits);
}

}  // namespace eventbook
