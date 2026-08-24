#pragma once

#include <string>
#include <string_view>

#include "eventbook/common/result.hpp"

namespace eventbook {

/// The only HTTP method this project can issue.
///
/// Version one is read-only with respect to the exchange. Rather than stating
/// that in documentation and trusting future code to honour it, the request
/// type offers no way to express anything else: there is no Post or Delete
/// enumerator to select, so no code path can submit, amend, or cancel an order
/// through this transport. AGENTS.md requires tests proving the default
/// configuration cannot send a write request -- here the guarantee is
/// structural, and adding a write would be a visible, reviewable edit to this
/// enum rather than a one-line call somewhere far away.
enum class HttpMethod {
    Get,
};

/// Everything needed to issue one request. Note the absence of a body field.
struct HttpRequest {
    HttpMethod method{HttpMethod::Get};
    std::string host;    ///< "external-api.kalshi.com"; used for SNI and the Host header
    std::string target;  ///< origin-form path and query: "/trade-api/v2/markets?limit=1"
};

struct HttpResponse {
    int status_code{};
    std::string body;
};

/// Failures that occur below the HTTP status layer.
///
/// A non-2xx status is deliberately NOT one of these. A 404 or a 429 is a
/// *successful* exchange that happened to carry an unwelcome status, and what
/// it means is the caller's business. These are the cases where no status was
/// ever received at all.
enum class HttpError {
    InvalidRequest,      ///< malformed before anything was sent
    ResolveFailed,       ///< DNS
    ConnectFailed,       ///< TCP
    TlsContextFailed,    ///< no trust store, or SNI could not be set
    TlsHandshakeFailed,  ///< including certificate verification failure
    WriteFailed,
    ReadFailed,
    Timeout,
};

[[nodiscard]] std::string_view to_string(HttpError error);

/// The seam that makes every layer above it testable without a network.
///
/// This is the only interface in the API layer, and it exists for exactly the
/// reason AGENTS.md sanctions interfaces: two implementations are genuinely
/// needed. One speaks TLS to Kalshi; one replays recorded payloads, so that
/// pagination, status handling, and eligibility rules can be tested
/// deterministically and offline rather than against a live venue whose
/// contents change by the minute.
class HttpTransport {
public:
    HttpTransport() = default;
    virtual ~HttpTransport() = default;

    HttpTransport(const HttpTransport&) = delete;
    HttpTransport& operator=(const HttpTransport&) = delete;
    HttpTransport(HttpTransport&&) = delete;
    HttpTransport& operator=(HttpTransport&&) = delete;

    [[nodiscard]] virtual Result<HttpResponse, HttpError> send(const HttpRequest& request) = 0;
};

}  // namespace eventbook
