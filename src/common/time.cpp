#include "eventbook/common/time.hpp"

#include <fmt/format.h>

#include <cstddef>

namespace eventbook {
namespace {

// Shortest legal form: "YYYY-MM-DDTHH:MM:SSZ".
constexpr std::size_t kMinimumLength = 20;

// Fractional digits kept; the rest are discarded (see parse_rfc3339's contract).
constexpr std::size_t kKeptFractionalDigits = 6;

constexpr bool is_digit(char character) {
    return character >= '0' && character <= '9';
}

// Read exactly `count` digits, advancing `index` only on success. Fixed-width
// reads are what make the format check strict: "2026-8-24" fails here rather
// than being silently accepted as August.
constexpr bool read_fixed_digits(std::string_view text, std::size_t& index, std::size_t count,
                                 int& out) {
    if (index + count > text.size()) {
        return false;
    }
    int value = 0;
    for (std::size_t offset = 0; offset < count; ++offset) {
        const char character = text[index + offset];
        if (!is_digit(character)) {
            return false;
        }
        value = value * 10 + (character - '0');
    }
    index += count;
    out = value;
    return true;
}

constexpr bool consume(std::string_view text, std::size_t& index, char expected) {
    if (index >= text.size() || text[index] != expected) {
        return false;
    }
    ++index;
    return true;
}

}  // namespace

std::string_view to_string(TimestampParseError error) {
    switch (error) {
        case TimestampParseError::Empty:
            return "empty string";
        case TimestampParseError::MalformedFormat:
            return "malformed RFC 3339 timestamp";
        case TimestampParseError::InvalidDate:
            return "impossible calendar date";
        case TimestampParseError::InvalidTime:
            return "impossible time of day";
        case TimestampParseError::InvalidOffset:
            return "UTC offset out of range";
    }
    return "unknown timestamp parse error";
}

LocalTimestamp local_now() {
    // system_clock's native resolution differs by platform -- microseconds on
    // macOS, nanoseconds on Linux -- so the cast is what makes the recorded
    // value identical in shape regardless of where the collector runs.
    return LocalTimestamp{
        std::chrono::time_point_cast<TimeResolution>(std::chrono::system_clock::now())};
}

Result<ExchangeTimestamp, TimestampParseError> parse_rfc3339(std::string_view text) {
    if (text.empty()) {
        return Failure{TimestampParseError::Empty};
    }
    if (text.size() < kMinimumLength) {
        return Failure{TimestampParseError::MalformedFormat};
    }

    std::size_t index = 0;
    int year_value = 0;
    int month_value = 0;
    int day_value = 0;
    int hour_value = 0;
    int minute_value = 0;
    int second_value = 0;

    const bool shape_ok =
        read_fixed_digits(text, index, 4, year_value) && consume(text, index, '-') &&
        read_fixed_digits(text, index, 2, month_value) && consume(text, index, '-') &&
        read_fixed_digits(text, index, 2, day_value) &&
        (consume(text, index, 'T') || consume(text, index, 't')) &&
        read_fixed_digits(text, index, 2, hour_value) && consume(text, index, ':') &&
        read_fixed_digits(text, index, 2, minute_value) && consume(text, index, ':') &&
        read_fixed_digits(text, index, 2, second_value);
    if (!shape_ok) {
        return Failure{TimestampParseError::MalformedFormat};
    }

    // Optional fractional seconds. Every digit present must be a digit, but only
    // the leading six contribute; a shorter run is padded, a longer one truncated.
    std::int64_t fractional_micros = 0;
    if (index < text.size() && text[index] == '.') {
        ++index;
        const std::size_t fraction_begin = index;
        while (index < text.size() && is_digit(text[index])) {
            ++index;
        }
        if (index == fraction_begin) {
            return Failure{TimestampParseError::MalformedFormat};
        }
        for (std::size_t offset = 0; offset < kKeptFractionalDigits; ++offset) {
            const std::size_t at = fraction_begin + offset;
            const int digit = at < index ? text[at] - '0' : 0;
            fractional_micros = fractional_micros * 10 + digit;
        }
    }

    // Zone designator. RFC 3339 permits a numeric offset, so accept one and
    // normalize to UTC rather than assuming every field will always arrive as Z.
    if (index >= text.size()) {
        return Failure{TimestampParseError::MalformedFormat};
    }
    std::chrono::minutes offset{0};
    const char zone = text[index];
    if (zone == 'Z' || zone == 'z') {
        ++index;
    } else if (zone == '+' || zone == '-') {
        ++index;
        int offset_hours = 0;
        int offset_minutes = 0;
        if (!read_fixed_digits(text, index, 2, offset_hours) || !consume(text, index, ':') ||
            !read_fixed_digits(text, index, 2, offset_minutes)) {
            return Failure{TimestampParseError::MalformedFormat};
        }
        if (offset_hours > 23 || offset_minutes > 59) {
            return Failure{TimestampParseError::InvalidOffset};
        }
        offset = std::chrono::hours{offset_hours} + std::chrono::minutes{offset_minutes};
        if (zone == '-') {
            offset = -offset;
        }
    } else {
        return Failure{TimestampParseError::MalformedFormat};
    }

    if (index != text.size()) {
        return Failure{TimestampParseError::MalformedFormat};
    }

    const std::chrono::year_month_day date{std::chrono::year{year_value},
                                           std::chrono::month{static_cast<unsigned>(month_value)},
                                           std::chrono::day{static_cast<unsigned>(day_value)}};
    if (!date.ok()) {
        return Failure{TimestampParseError::InvalidDate};
    }
    // 60 is a legal RFC 3339 leap second but has no representation in sys_time,
    // so it is rejected rather than silently folded into the next minute.
    if (hour_value > 23 || minute_value > 59 || second_value > 59) {
        return Failure{TimestampParseError::InvalidTime};
    }

    const auto midnight_utc =
        std::chrono::duration_cast<TimeResolution>(std::chrono::sys_days{date}.time_since_epoch());
    const auto since_epoch =
        midnight_utc + std::chrono::hours{hour_value} + std::chrono::minutes{minute_value} +
        std::chrono::seconds{second_value} + TimeResolution{fractional_micros} -
        std::chrono::duration_cast<TimeResolution>(offset);

    return ExchangeTimestamp{WallTime{since_epoch}};
}

std::string format_rfc3339(ExchangeTimestamp timestamp) {
    // floor rather than duration_cast so that instants before 1970 land on the
    // preceding midnight instead of rounding toward zero into the next day.
    const auto days = std::chrono::floor<std::chrono::days>(timestamp.value);
    const std::chrono::year_month_day date{days};
    const auto time_of_day = timestamp.value - days;

    const auto hours = std::chrono::duration_cast<std::chrono::hours>(time_of_day);
    const auto minutes = std::chrono::duration_cast<std::chrono::minutes>(time_of_day - hours);
    const auto seconds =
        std::chrono::duration_cast<std::chrono::seconds>(time_of_day - hours - minutes);
    const auto micros = time_of_day - hours - minutes - seconds;

    return fmt::format("{:04}-{:02}-{:02}T{:02}:{:02}:{:02}.{:06}Z", static_cast<int>(date.year()),
                       static_cast<unsigned>(date.month()), static_cast<unsigned>(date.day()),
                       hours.count(), minutes.count(), seconds.count(), micros.count());
}

}  // namespace eventbook
