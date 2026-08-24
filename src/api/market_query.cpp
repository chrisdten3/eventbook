#include "eventbook/api/market_query.hpp"

#include <array>
#include <utility>

namespace eventbook {
namespace {

constexpr std::string_view kMarketsPath = "/markets";

constexpr bool is_unreserved(char character) {
    return (character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z') ||
           (character >= '0' && character <= '9') || character == '-' || character == '_' ||
           character == '.' || character == '~';
}

std::string percent_encode(std::string_view text) {
    static constexpr std::array<char, 16> kHexDigits{'0', '1', '2', '3', '4', '5', '6', '7',
                                                     '8', '9', 'A', 'B', 'C', 'D', 'E', 'F'};
    std::string encoded;
    encoded.reserve(text.size());
    for (const char character : text) {
        if (is_unreserved(character)) {
            encoded.push_back(character);
            continue;
        }
        const auto byte = static_cast<unsigned char>(character);
        encoded.push_back('%');
        encoded.push_back(kHexDigits[byte >> 4U]);
        encoded.push_back(kHexDigits[byte & 0x0FU]);
    }
    return encoded;
}

void append_parameter(std::string& query, std::string_view key, std::string_view value) {
    if (!query.empty()) {
        query.push_back('&');
    }
    query.append(key);
    query.push_back('=');
    query.append(percent_encode(value));
}

}  // namespace

std::string_view to_query_value(MarketStatusFilter filter) {
    switch (filter) {
        case MarketStatusFilter::Unopened:
            return "unopened";
        case MarketStatusFilter::Open:
            return "open";
        case MarketStatusFilter::Closed:
            return "closed";
        case MarketStatusFilter::Settled:
            return "settled";
    }
    return "open";
}

std::string_view to_string(MarketQueryErrorKind kind) {
    switch (kind) {
        case MarketQueryErrorKind::Rest:
            return "REST failure";
        case MarketQueryErrorKind::Parse:
            return "response did not parse";
        case MarketQueryErrorKind::CursorNotAdvancing:
            return "pagination cursor did not advance";
        case MarketQueryErrorKind::PageLimitReached:
            return "page limit reached before pagination finished";
    }
    return "unknown market query error";
}

std::string build_market_query(const MarketQuery& query, std::string_view cursor) {
    std::string encoded;
    append_parameter(encoded, "limit", std::to_string(query.limit));
    if (query.series_ticker.has_value()) {
        append_parameter(encoded, "series_ticker", query.series_ticker->value);
    }
    if (query.status.has_value()) {
        append_parameter(encoded, "status", to_query_value(*query.status));
    }
    if (!cursor.empty()) {
        append_parameter(encoded, "cursor", cursor);
    }
    return encoded;
}

Result<MarketPage, MarketQueryError> fetch_market_page(KalshiRestClient& client,
                                                       const MarketQuery& query,
                                                       std::string_view cursor) {
    const auto response = client.get(kMarketsPath, build_market_query(query, cursor));
    if (!response) {
        return Failure{MarketQueryError{MarketQueryErrorKind::Rest, response.error(), {}}};
    }

    auto page = parse_market_page(response->body);
    if (!page) {
        return Failure{MarketQueryError{MarketQueryErrorKind::Parse, {}, page.error()}};
    }
    return *page;
}

Result<std::vector<Market>, MarketQueryError> fetch_all_markets(KalshiRestClient& client,
                                                                const MarketQuery& query,
                                                                std::size_t max_pages) {
    std::vector<Market> markets;
    std::string cursor;

    for (std::size_t page_number = 0; page_number < max_pages; ++page_number) {
        auto page = fetch_market_page(client, query, cursor);
        if (!page) {
            return Failure{page.error()};
        }

        markets.insert(markets.end(), page->markets.begin(), page->markets.end());

        if (page->cursor.empty()) {
            return markets;
        }
        // A venue that returns the cursor it was just given would otherwise
        // spin here forever, duplicating markets and burning the read budget,
        // with no individual response ever looking malformed.
        if (page->cursor == cursor) {
            return Failure{MarketQueryError{MarketQueryErrorKind::CursorNotAdvancing, {}, {}}};
        }
        cursor = page->cursor;
    }

    // Falling out of the loop means the venue still had more to give. Returning
    // a truncated list silently would understate the universe and quietly bias
    // anything computed from it, so it is an error instead.
    return Failure{MarketQueryError{MarketQueryErrorKind::PageLimitReached, {}, {}}};
}

}  // namespace eventbook
