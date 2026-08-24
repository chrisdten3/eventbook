#include "eventbook/common/decimal.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

using eventbook::DecimalParseError;
using eventbook::format_scaled_decimal;
using eventbook::parse_scaled_decimal;

namespace {

// Helpers that fail the test on the wrong arm, so each assertion below stays a
// single readable line instead of three lines of unwrapping.
std::int64_t parsed(std::string_view text, int digits, bool allow_negative = false) {
    const auto result = parse_scaled_decimal(text, digits, allow_negative);
    REQUIRE(result.has_value());
    return *result;
}

DecimalParseError rejected(std::string_view text, int digits, bool allow_negative = false) {
    const auto result = parse_scaled_decimal(text, digits, allow_negative);
    REQUIRE_FALSE(result.has_value());
    return result.error();
}

}  // namespace

TEST_CASE("parse_scaled_decimal scales by the requested number of digits") {
    CHECK(parsed("0.1200", 4) == 1200);
    CHECK(parsed("0.0001", 4) == 1);
    CHECK(parsed("0.9999", 4) == 9999);
    CHECK(parsed("1.0000", 4) == 10000);
    CHECK(parsed("0", 4) == 0);
    CHECK(parsed("7", 2) == 700);
}

TEST_CASE("parse_scaled_decimal pads a short fractional run out to the scale") {
    // "0.5" means five tenths, i.e. 5000 units at scale 4 -- not 5 units.
    CHECK(parsed("0.5", 4) == 5000);
    CHECK(parsed("0.05", 4) == 500);
    CHECK(parsed("0.5", 2) == 50);
}

TEST_CASE("parse_scaled_decimal refuses to round away precision") {
    // Silently dropping the fifth digit would corrupt a value we later compare
    // for exact equality, so extra precision is an error, not a rounding job.
    CHECK(rejected("0.12345", 4) == DecimalParseError::TooManyFractionalDigits);
    CHECK(rejected("0.001", 2) == DecimalParseError::TooManyFractionalDigits);

    // Measured before accumulation, so a very long run reports precision rather
    // than overflowing while being read.
    CHECK(rejected("0." + std::string(100, '9'), 4) == DecimalParseError::TooManyFractionalDigits);
}

TEST_CASE("parse_scaled_decimal rejects malformed input") {
    CHECK(rejected("", 4) == DecimalParseError::Empty);
    CHECK(rejected(".", 4) == DecimalParseError::MissingDigits);
    CHECK(rejected(".5", 4) == DecimalParseError::MissingDigits);
    CHECK(rejected("1.", 4) == DecimalParseError::MissingDigits);
    CHECK(rejected("abc", 4) == DecimalParseError::MissingDigits);
    CHECK(rejected("+1", 4) == DecimalParseError::MissingDigits);
    CHECK(rejected(" 1", 4) == DecimalParseError::MissingDigits);
    CHECK(rejected("-", 4, true) == DecimalParseError::MissingDigits);

    CHECK(rejected("1 ", 4) == DecimalParseError::InvalidCharacter);
    CHECK(rejected("1a", 4) == DecimalParseError::InvalidCharacter);
    CHECK(rejected("1.2.3", 4) == DecimalParseError::InvalidCharacter);
    CHECK(rejected("1e-2", 4) == DecimalParseError::InvalidCharacter);
}

TEST_CASE("parse_scaled_decimal gates the sign on the caller's domain") {
    CHECK(rejected("-0.5", 4) == DecimalParseError::NegativeNotAllowed);
    CHECK(parsed("-0.5", 4, true) == -5000);
    CHECK(parsed("-0.05", 2, true) == -5);
    CHECK(parsed("-0", 4, true) == 0);
}

TEST_CASE("parse_scaled_decimal reports overflow instead of wrapping") {
    CHECK(rejected("99999999999999999999", 4) == DecimalParseError::Overflow);
    CHECK(rejected("9223372036854775807", 4) == DecimalParseError::Overflow);
    CHECK(parsed("922337203685477", 4) == 9223372036854770000);
}

TEST_CASE("format_scaled_decimal emits the canonical wire form") {
    CHECK(format_scaled_decimal(1200, 4) == "0.1200");
    CHECK(format_scaled_decimal(1, 4) == "0.0001");
    CHECK(format_scaled_decimal(10000, 4) == "1.0000");
    CHECK(format_scaled_decimal(0, 4) == "0.0000");
    CHECK(format_scaled_decimal(700, 2) == "7.00");
    CHECK(format_scaled_decimal(123, 0) == "123");
}

TEST_CASE("format_scaled_decimal keeps the sign when the integer part is zero") {
    CHECK(format_scaled_decimal(-50, 2) == "-0.50");
    CHECK(format_scaled_decimal(-5, 2) == "-0.05");
    CHECK(format_scaled_decimal(-150, 2) == "-1.50");
}

TEST_CASE("format_scaled_decimal survives the most negative int64") {
    // Negating INT64_MIN would overflow, so the implementation takes the
    // magnitude in unsigned arithmetic. This asserts it does not trap or wrap.
    const auto text = format_scaled_decimal(std::numeric_limits<std::int64_t>::min(), 2);
    CHECK(text == "-92233720368547758.08");
}

TEST_CASE("format and parse round-trip through each other") {
    for (const std::int64_t units : {std::int64_t{0}, std::int64_t{1}, std::int64_t{-1},
                                     std::int64_t{9999}, std::int64_t{10000}, std::int64_t{-73}}) {
        const auto text = format_scaled_decimal(units, 4);
        INFO("units=" << units << " text=" << text);
        CHECK(parsed(text, 4, true) == units);
    }
}
