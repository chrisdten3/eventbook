#include "eventbook/common/price.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string_view>

using eventbook::DecimalParseError;
using eventbook::format_price;
using eventbook::is_valid_yes_price;
using eventbook::kMaxYesPrice;
using eventbook::kMinYesPrice;
using eventbook::parse_price;
using eventbook::Price;
using eventbook::PriceDelta;
using eventbook::yes_price_from_no;

// Comparison and arithmetic are usable at compile time. These fail the build
// rather than the test run, which is the point of making them constexpr.
static_assert(Price{1200} == Price{1200});
static_assert(Price{1200} < Price{1201});
static_assert(Price{7000} - Price{5000} == PriceDelta{2000});
static_assert(Price{5000} - Price{7000} == PriceDelta{-2000});
static_assert(Price{5000} + PriceDelta{2000} == Price{7000});
static_assert(-PriceDelta{2000} == PriceDelta{-2000});
static_assert(is_valid_yes_price(kMinYesPrice) && is_valid_yes_price(kMaxYesPrice));
static_assert(yes_price_from_no(Price{3000}) == Price{7000});

TEST_CASE("parse_price reads the *_dollars wire format exactly") {
    const auto price = parse_price("0.1200");
    REQUIRE(price.has_value());
    CHECK(*price == Price{1200});
    CHECK(price->units == 1200);
}

TEST_CASE("parse_price handles the endpoints of the settlement range") {
    REQUIRE(parse_price("0.0000").has_value());
    CHECK(*parse_price("0.0000") == kMinYesPrice);
    REQUIRE(parse_price("1.0000").has_value());
    CHECK(*parse_price("1.0000") == kMaxYesPrice);

    // Sub-cent prices are why the scale is 10,000 and not 100.
    REQUIRE(parse_price("0.0001").has_value());
    CHECK(*parse_price("0.0001") == Price{1});
}

TEST_CASE("parse_price rejects a negative price outright") {
    const auto price = parse_price("-0.0100");
    REQUIRE_FALSE(price.has_value());
    CHECK(price.error() == DecimalParseError::NegativeNotAllowed);
}

TEST_CASE("parse_price is lexical; settlement bounds are a separate question") {
    // $1.50 is a well-formed decimal and a nonsensical binary-contract price.
    // Keeping the two checks apart means the parser stays reusable and the
    // domain rule stays visible at the call site that cares about it.
    const auto price = parse_price("1.5000");
    REQUIRE(price.has_value());
    CHECK(*price == Price{15000});
    CHECK_FALSE(is_valid_yes_price(*price));
}

TEST_CASE("parse_price reports overflow when narrowing to int32") {
    // 999999 dollars is 9,999,990,000 units: fine in the int64 the shared parser
    // uses, far outside the int32 that Price stores.
    const auto price = parse_price("999999");
    REQUIRE_FALSE(price.has_value());
    CHECK(price.error() == DecimalParseError::Overflow);
}

TEST_CASE("format_price round-trips the canonical form") {
    for (const std::string_view text : {"0.0000", "0.0001", "0.1200", "0.9999", "1.0000"}) {
        const auto price = parse_price(text);
        REQUIRE(price.has_value());
        INFO("text=" << text);
        CHECK(format_price(*price) == text);
    }
}

TEST_CASE("format_price pads a short input to four decimal places") {
    const auto price = parse_price("0.5");
    REQUIRE(price.has_value());
    CHECK(format_price(*price) == "0.5000");
}

TEST_CASE("yes_price_from_no reflects a level across the $1.00 payout") {
    CHECK(yes_price_from_no(Price{3000}) == Price{7000});
    CHECK(yes_price_from_no(Price{9900}) == Price{100});
    CHECK(yes_price_from_no(kMinYesPrice) == kMaxYesPrice);
    CHECK(yes_price_from_no(kMaxYesPrice) == kMinYesPrice);
}

TEST_CASE("yes_price_from_no is its own inverse") {
    for (const std::int32_t units : {0, 1, 2500, 5000, 7331, 10000}) {
        const Price no_price{units};
        INFO("units=" << units);
        CHECK(yes_price_from_no(yes_price_from_no(no_price)) == no_price);
    }
}

TEST_CASE("a price displacement carries a sign") {
    const Price arrival{5000};
    const Price executed{5200};

    // A buy that paid more than arrival has a positive shortfall. Encoding this
    // as PriceDelta rather than Price is what stops the sign convention from
    // silently inverting in the execution simulator later.
    const PriceDelta shortfall = executed - arrival;
    CHECK(shortfall == PriceDelta{200});
    CHECK(shortfall > PriceDelta{0});
    CHECK(arrival - executed == -shortfall);
}
