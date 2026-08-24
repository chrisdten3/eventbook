#include "eventbook/common/time.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>

using eventbook::epoch_micros;
using eventbook::exchange_time_from_epoch_millis;
using eventbook::ExchangeTimestamp;
using eventbook::format_rfc3339;
using eventbook::local_now;
using eventbook::local_time_from_epoch_micros;
using eventbook::LocalTimestamp;
using eventbook::observed_timestamp_difference;
using eventbook::parse_rfc3339;
using eventbook::TimestampParseError;

namespace {

std::int64_t parsed_micros(std::string_view text) {
    const auto timestamp = parse_rfc3339(text);
    REQUIRE(timestamp.has_value());
    return epoch_micros(*timestamp);
}

TimestampParseError rejected(std::string_view text) {
    const auto timestamp = parse_rfc3339(text);
    REQUIRE_FALSE(timestamp.has_value());
    return timestamp.error();
}

}  // namespace

TEST_CASE("parse_rfc3339 anchors on the Unix epoch") {
    CHECK(parsed_micros("1970-01-01T00:00:00Z") == 0);
    CHECK(parsed_micros("1970-01-01T00:00:01Z") == 1'000'000);
}

TEST_CASE("parse_rfc3339 converts a known date correctly") {
    // 2026-08-24T00:00:00Z is 1,787,529,600 seconds after the epoch.
    CHECK(parsed_micros("2026-08-24T00:00:00Z") == 1'787'529'600'000'000);
    CHECK(parsed_micros("2026-08-24T22:19:49Z") == 1'787'609'989'000'000);
}

TEST_CASE("parse_rfc3339 handles instants before the epoch") {
    CHECK(parsed_micros("1969-12-31T23:59:59Z") == -1'000'000);
    CHECK(parsed_micros("1969-12-31T00:00:00Z") == -86'400'000'000);
}

TEST_CASE("parse_rfc3339 pads a short fractional run and truncates a long one") {
    CHECK(parsed_micros("1970-01-01T00:00:00.5Z") == 500'000);
    CHECK(parsed_micros("1970-01-01T00:00:00.123456Z") == 123'456);

    // Go's time.RFC3339Nano emits up to nine digits. Sub-microsecond precision
    // is below the venue's clock accuracy, so it is dropped rather than
    // rejected -- unlike a sub-cent price, which is money and must not round.
    CHECK(parsed_micros("1970-01-01T00:00:00.123456789Z") == 123'456);
    CHECK(parsed_micros("1970-01-01T00:00:00.000000999Z") == 0);
}

TEST_CASE("parse_rfc3339 normalizes a numeric UTC offset") {
    // Same instant, three spellings.
    const auto utc = parsed_micros("2026-08-24T12:00:00Z");
    CHECK(parsed_micros("2026-08-24T17:00:00+05:00") == utc);
    CHECK(parsed_micros("2026-08-24T07:00:00-05:00") == utc);
    CHECK(parsed_micros("2026-08-24T12:30:00+00:30") == utc);
}

TEST_CASE("parse_rfc3339 accepts either case for the separators") {
    const auto upper = parsed_micros("2026-08-24T12:00:00Z");
    CHECK(parsed_micros("2026-08-24t12:00:00z") == upper);
}

TEST_CASE("parse_rfc3339 rejects a malformed shape") {
    CHECK(rejected("") == TimestampParseError::Empty);
    CHECK(rejected("2026-08-24") == TimestampParseError::MalformedFormat);

    // Fixed-width digit reads are what make this strict: an unpadded month is
    // an error rather than something quietly reinterpreted.
    CHECK(rejected("2026-8-24T00:00:00Z") == TimestampParseError::MalformedFormat);
    CHECK(rejected("2026-08-24 00:00:00Z") == TimestampParseError::MalformedFormat);
    CHECK(rejected("2026-08-24T00:00:00") == TimestampParseError::MalformedFormat);
    CHECK(rejected("2026-08-24T00:00:00Z ") == TimestampParseError::MalformedFormat);
    CHECK(rejected("2026-08-24T00:00:00.Z") == TimestampParseError::MalformedFormat);
    CHECK(rejected("2026-08-24T00:00:00+0500") == TimestampParseError::MalformedFormat);
}

TEST_CASE("parse_rfc3339 rejects impossible calendar dates") {
    CHECK(rejected("2026-02-30T00:00:00Z") == TimestampParseError::InvalidDate);
    CHECK(rejected("2026-13-01T00:00:00Z") == TimestampParseError::InvalidDate);
    CHECK(rejected("2026-00-01T00:00:00Z") == TimestampParseError::InvalidDate);

    // 2026 is not a leap year; 2028 is. The calendar arithmetic is delegated to
    // std::chrono precisely so this distinction is not ours to get wrong.
    CHECK(rejected("2026-02-29T00:00:00Z") == TimestampParseError::InvalidDate);
    CHECK(parse_rfc3339("2028-02-29T00:00:00Z").has_value());
}

TEST_CASE("parse_rfc3339 rejects impossible times, including leap seconds") {
    CHECK(rejected("2026-08-24T24:00:00Z") == TimestampParseError::InvalidTime);
    CHECK(rejected("2026-08-24T00:60:00Z") == TimestampParseError::InvalidTime);

    // A leap second is legal RFC 3339 and has no sys_time representation, so it
    // is refused rather than silently folded into the following minute.
    CHECK(rejected("2026-12-31T23:59:60Z") == TimestampParseError::InvalidTime);
}

TEST_CASE("parse_rfc3339 rejects an out-of-range offset") {
    CHECK(rejected("2026-08-24T00:00:00+24:00") == TimestampParseError::InvalidOffset);
    CHECK(rejected("2026-08-24T00:00:00+00:60") == TimestampParseError::InvalidOffset);
}

TEST_CASE("format_rfc3339 emits one canonical form") {
    const auto timestamp = parse_rfc3339("2026-08-24T22:19:49Z");
    REQUIRE(timestamp.has_value());
    CHECK(format_rfc3339(*timestamp) == "2026-08-24T22:19:49.000000Z");
}

TEST_CASE("format_rfc3339 round-trips through the parser") {
    for (const std::string_view text :
         {"1970-01-01T00:00:00.000000Z", "2026-08-24T22:19:49.123456Z",
          "2028-02-29T12:34:56.000001Z"}) {
        const auto timestamp = parse_rfc3339(text);
        REQUIRE(timestamp.has_value());
        INFO("text=" << text);
        CHECK(format_rfc3339(*timestamp) == text);
    }
}

TEST_CASE("format_rfc3339 floors to the preceding midnight before the epoch") {
    // duration_cast would round toward zero here and report the wrong day.
    const auto timestamp = parse_rfc3339("1969-12-31T23:59:59Z");
    REQUIRE(timestamp.has_value());
    CHECK(format_rfc3339(*timestamp) == "1969-12-31T23:59:59.000000Z");
}

TEST_CASE("exchange_time_from_epoch_millis reads the ts_ms wire field") {
    const auto timestamp = exchange_time_from_epoch_millis(1'787'529'600'123);
    CHECK(epoch_micros(timestamp) == 1'787'529'600'123'000);
    CHECK(format_rfc3339(timestamp) == "2026-08-24T00:00:00.123000Z");
}

TEST_CASE("subtracting two readings of the same clock gives elapsed time") {
    const auto earlier = exchange_time_from_epoch_millis(1'000);
    const auto later = exchange_time_from_epoch_millis(1'250);
    CHECK(later - earlier == std::chrono::milliseconds{250});
    CHECK(earlier - later == std::chrono::milliseconds{-250});
}

TEST_CASE("observed_timestamp_difference is signed and may be negative") {
    const auto exchange = exchange_time_from_epoch_millis(1'000);

    // Received 40ms after the venue's stated send time.
    const auto local_after = local_time_from_epoch_micros(1'040'000);
    CHECK(observed_timestamp_difference(local_after, exchange) == std::chrono::milliseconds{40});

    // A negative result is not an error. Clock offset between two unsynchronized
    // machines can exceed transport delay in either direction, which is exactly
    // why this quantity must not be reported as one-way latency.
    const auto local_before = local_time_from_epoch_micros(985'000);
    CHECK(observed_timestamp_difference(local_before, exchange) == std::chrono::milliseconds{-15});
}

TEST_CASE("local_now reads a plausible wall clock") {
    const auto first = local_now();
    const auto second = local_now();

    // Wall clocks can step backwards under NTP correction, so this asserts only
    // that the value is sane, not that it is monotonic. Anything needing a
    // monotonic guarantee must use steady_clock instead.
    CHECK(epoch_micros(first) > 1'577'836'800'000'000);  // after 2020-01-01
    CHECK(second >= first);
}
