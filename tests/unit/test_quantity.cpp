#include "eventbook/common/quantity.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <limits>
#include <string_view>

using eventbook::apply_delta;
using eventbook::DecimalParseError;
using eventbook::format_quantity;
using eventbook::format_quantity_delta;
using eventbook::parse_quantity;
using eventbook::parse_quantity_delta;
using eventbook::Quantity;
using eventbook::QuantityDelta;
using eventbook::QuantityDeltaError;

// Comparison, arithmetic, and delta application all work at compile time, so a
// broken invariant here fails the build rather than the test run.
static_assert(Quantity{100} == Quantity{100});
static_assert(Quantity{100} < Quantity{101});
static_assert(Quantity{500} - Quantity{200} == QuantityDelta{300});
static_assert(Quantity{200} - Quantity{500} == QuantityDelta{-300});
static_assert(Quantity{500} + Quantity{200} == Quantity{700});
static_assert(-QuantityDelta{300} == QuantityDelta{-300});
static_assert(*apply_delta(Quantity{500}, QuantityDelta{-200}) == Quantity{300});
static_assert(!apply_delta(Quantity{100}, QuantityDelta{-200}).has_value());

TEST_CASE("parse_quantity reads the *_fp wire format exactly") {
    const auto quantity = parse_quantity("54.00");
    REQUIRE(quantity.has_value());
    CHECK(*quantity == Quantity{5400});
    CHECK(quantity->units == 5400);
}

TEST_CASE("parse_quantity represents the 0.01-contract granularity") {
    REQUIRE(parse_quantity("0.01").has_value());
    CHECK(*parse_quantity("0.01") == Quantity{1});
    REQUIRE(parse_quantity("1").has_value());
    CHECK(*parse_quantity("1") == Quantity{Quantity::kUnitsPerContract});
    REQUIRE(parse_quantity("0").has_value());
    CHECK(*parse_quantity("0") == Quantity{});
}

TEST_CASE("parse_quantity refuses a negative resting size") {
    // A size is nonnegative by construction, so the book invariant is enforced
    // where text enters the system rather than checked later and hopefully
    // everywhere.
    const auto quantity = parse_quantity("-54.00");
    REQUIRE_FALSE(quantity.has_value());
    CHECK(quantity.error() == DecimalParseError::NegativeNotAllowed);
}

TEST_CASE("parse_quantity_delta accepts the sign that delta_fp carries") {
    // "-54.00" is a value from the documented orderbook_delta example.
    const auto removal = parse_quantity_delta("-54.00");
    REQUIRE(removal.has_value());
    CHECK(*removal == QuantityDelta{-5400});

    const auto addition = parse_quantity_delta("54.00");
    REQUIRE(addition.has_value());
    CHECK(*addition == QuantityDelta{5400});
}

TEST_CASE("parse_quantity rejects precision finer than 0.01 contracts") {
    const auto quantity = parse_quantity("1.005");
    REQUIRE_FALSE(quantity.has_value());
    CHECK(quantity.error() == DecimalParseError::TooManyFractionalDigits);
}

TEST_CASE("format round-trips the canonical wire form") {
    for (const std::string_view text : {"0.00", "0.01", "1.00", "54.00", "12345.67"}) {
        const auto quantity = parse_quantity(text);
        REQUIRE(quantity.has_value());
        INFO("text=" << text);
        CHECK(format_quantity(*quantity) == text);
    }

    for (const std::string_view text : {"-54.00", "-0.01", "0.00", "54.00"}) {
        const auto delta = parse_quantity_delta(text);
        REQUIRE(delta.has_value());
        INFO("text=" << text);
        CHECK(format_quantity_delta(*delta) == text);
    }
}

TEST_CASE("apply_delta adds and removes size") {
    const auto grown = apply_delta(Quantity{5400}, QuantityDelta{600});
    REQUIRE(grown.has_value());
    CHECK(*grown == Quantity{6000});

    const auto shrunk = apply_delta(Quantity{5400}, QuantityDelta{-400});
    REQUIRE(shrunk.has_value());
    CHECK(*shrunk == Quantity{5000});
}

TEST_CASE("apply_delta permits a level to reach exactly zero") {
    // Emptying a level is normal and expected -- the book removes zero levels.
    // Only going *below* zero signals desynchronization.
    const auto emptied = apply_delta(Quantity{5400}, QuantityDelta{-5400});
    REQUIRE(emptied.has_value());
    CHECK(*emptied == Quantity{});
}

TEST_CASE("apply_delta reports an impossible transition instead of going negative") {
    // Removing more size than the level holds means our state is wrong: a missed
    // message or a stale snapshot. apply_delta only says the transition is
    // impossible; the order book decides to mark itself invalid and re-snapshot.
    const auto result = apply_delta(Quantity{5400}, QuantityDelta{-5401});
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == QuantityDeltaError::WouldGoNegative);
}

TEST_CASE("apply_delta reports overflow instead of wrapping") {
    constexpr auto kMax = std::numeric_limits<std::int64_t>::max();
    const auto result = apply_delta(Quantity{kMax}, QuantityDelta{1});
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == QuantityDeltaError::Overflow);
}

TEST_CASE("apply_delta never yields a negative size") {
    // The property AGENTS.md states as a book invariant, exercised across the
    // boundary where it is most likely to break.
    for (const std::int64_t resting : {0, 1, 100, 5400}) {
        for (const std::int64_t change : {-10000, -5401, -5400, -1, 0, 1, 10000}) {
            const auto result = apply_delta(Quantity{resting}, QuantityDelta{change});
            INFO("resting=" << resting << " change=" << change);
            if (result.has_value()) {
                CHECK(result->units >= 0);
                CHECK(result->units == resting + change);
            } else {
                CHECK(result.error() == QuantityDeltaError::WouldGoNegative);
                CHECK(resting + change < 0);
            }
        }
    }
}
