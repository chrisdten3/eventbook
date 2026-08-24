#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "eventbook/api/market.hpp"
#include "eventbook/api/rest_client.hpp"
#include "eventbook/common/identifiers.hpp"
#include "eventbook/common/result.hpp"

namespace eventbook {

/// Values accepted by the `status` QUERY PARAMETER.
///
/// Deliberately a different type from MarketStatus, which models the `status`
/// FIELD, because the two vocabularies genuinely differ: querying `open`
/// returns markets whose field reads `active`, and querying `closed` returns a
/// mixture of `closed` and `determined`. Sharing one enum would invite writing
/// `status=active`, which matches nothing and silently returns everything.
enum class MarketStatusFilter {
    Unopened,  ///< field: initialized
    Open,      ///< field: active
    Closed,    ///< field: closed or determined
    Settled,   ///< field: finalized
};

[[nodiscard]] std::string_view to_query_value(MarketStatusFilter filter);

/// Server-side filters for GET /markets.
struct MarketQuery {
    std::optional<SeriesTicker> series_ticker;
    std::optional<MarketStatusFilter> status;

    /// Page size. The API accepts 1..1000 and defaults to 100. Larger pages
    /// mean fewer round trips against a read budget of 200 tokens/second at 10
    /// tokens per request -- roughly 20 requests/second on the Basic tier.
    int limit{100};
};

/// Build the query string for one request, percent-encoded.
///
/// Exposed rather than hidden so it can be tested directly. Cursors are
/// base64url in practice and need no escaping, but encoding them anyway costs
/// nothing and removes a class of bug that would otherwise appear only when the
/// venue changed its cursor format.
[[nodiscard]] std::string build_market_query(const MarketQuery& query, std::string_view cursor);

enum class MarketQueryErrorKind {
    Rest,                ///< HTTP or transport failure; see MarketQueryError::rest
    Parse,               ///< body arrived but did not parse; see MarketQueryError::parse
    CursorNotAdvancing,  ///< the venue returned a cursor it had already given us
    PageLimitReached,    ///< more pages than the caller was willing to fetch
};

[[nodiscard]] std::string_view to_string(MarketQueryErrorKind kind);

struct MarketQueryError {
    MarketQueryErrorKind kind{};
    RestError rest{};
    MarketParseError parse{};
};

/// Fetch one page. An empty `cursor` requests the first page.
[[nodiscard]] Result<MarketPage, MarketQueryError> fetch_market_page(KalshiRestClient& client,
                                                                     const MarketQuery& query,
                                                                     std::string_view cursor = {});

/// Follow cursors until the venue stops issuing them.
///
/// Two guards, both of which have to exist because this is an unbounded loop
/// driven by a remote server. `max_pages` caps the work. The repeated-cursor
/// check catches the worse failure: a venue that keeps handing back the same
/// cursor would otherwise spin forever, accumulating duplicate markets and
/// consuming the read budget, with nothing in the response ever looking wrong.
[[nodiscard]] Result<std::vector<Market>, MarketQueryError> fetch_all_markets(
    KalshiRestClient& client, const MarketQuery& query, std::size_t max_pages = 50);

}  // namespace eventbook
