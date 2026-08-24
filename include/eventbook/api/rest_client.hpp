#pragma once

#include <string>
#include <string_view>

#include "eventbook/api/http.hpp"
#include "eventbook/common/result.hpp"

namespace eventbook {

/// Which Kalshi deployment to talk to.
///
/// AGENTS.md requires demo and production to be distinguished by explicit
/// configuration rather than by a string somebody might mistype. That is not
/// paranoia here: the two hosts differ in top-level domain as well as
/// subdomain -- production is `.com`, demo is `.co` -- which is precisely the
/// near-miss a free-form string invites.
enum class KalshiEnvironment {
    Production,
    Demo,
};

/// Host for the given environment, with no scheme and no path.
[[nodiscard]] std::string_view host_for(KalshiEnvironment environment);

/// Path prefix shared by every v2 endpoint, and part of the string that
/// authenticated requests must sign once M2 needs them.
inline constexpr std::string_view kApiBasePath = "/trade-api/v2";

enum class RestErrorKind {
    Transport,         ///< never reached the HTTP layer; see RestError::transport_error
    RateLimited,       ///< 429
    NotFound,          ///< 404
    ClientError,       ///< other 4xx
    ServerError,       ///< 5xx
    UnexpectedStatus,  ///< 1xx, 3xx, or anything not covered above
};

[[nodiscard]] std::string_view to_string(RestErrorKind kind);

/// A failed request, carrying enough detail to drive a data-quality counter.
///
/// AGENTS.md requires reporting parse failures, reconnects, and similar counts
/// per session. Collapsing every failure into a single "error" would make those
/// counts useless, so the kind and the originating status are both preserved.
struct RestError {
    RestErrorKind kind{};
    HttpError transport_error{};  ///< meaningful only when kind == Transport
    int status_code{};            ///< meaningful for every other kind

    [[nodiscard]] friend constexpr bool operator==(const RestError&, const RestError&) = default;
};

/// Read-only client for the Kalshi REST API.
///
/// Synchronous, deliberately. Market discovery happens once at startup and sits
/// on no hot path, so a blocking call is simpler to reason about and to test
/// than a callback or a coroutine, and AGENTS.md puts determinism ahead of
/// concurrency. M2's WebSocket is where asynchrony actually earns its keep.
///
/// Holds a reference rather than ownership: the transport must outlive the
/// client. That lets the real transport -- which owns an io_context and a
/// loaded TLS trust store, both expensive to build -- be constructed once and
/// shared, and lets a test substitute a fake with no allocation ceremony.
class KalshiRestClient {
public:
    explicit KalshiRestClient(HttpTransport& transport,
                              KalshiEnvironment environment = KalshiEnvironment::Production);

    /// Issue a GET against `path`, which is relative to kApiBasePath and must
    /// begin with '/'. When `query` is non-empty it is appended after '?'
    /// verbatim -- the caller is responsible for encoding it. That is adequate
    /// while the only parameters are numeric limits and the API's own base64url
    /// cursors; a real encoder belongs with the pagination work in the next
    /// slice, where parameters are actually assembled from values.
    ///
    /// Any 2xx yields the response. Every other status becomes a RestError, so
    /// a caller cannot accidentally hand an error page to a JSON parser and
    /// treat the result as market data.
    [[nodiscard]] Result<HttpResponse, RestError> get(std::string_view path,
                                                      std::string_view query = {});

    [[nodiscard]] KalshiEnvironment environment() const {
        return environment_;
    }

private:
    HttpTransport& transport_;
    KalshiEnvironment environment_;
};

}  // namespace eventbook
