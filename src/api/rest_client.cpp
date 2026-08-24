#include "eventbook/api/rest_client.hpp"

namespace eventbook {

std::string_view host_for(KalshiEnvironment environment) {
    switch (environment) {
        case KalshiEnvironment::Production:
            return "external-api.kalshi.com";
        case KalshiEnvironment::Demo:
            // Note the .co, not .com. Not a typo.
            return "external-api.demo.kalshi.co";
    }
    return "external-api.kalshi.com";
}

std::string_view to_string(RestErrorKind kind) {
    switch (kind) {
        case RestErrorKind::Transport:
            return "transport failure";
        case RestErrorKind::RateLimited:
            return "rate limited";
        case RestErrorKind::NotFound:
            return "not found";
        case RestErrorKind::ClientError:
            return "client error";
        case RestErrorKind::ServerError:
            return "server error";
        case RestErrorKind::UnexpectedStatus:
            return "unexpected status";
    }
    return "unknown rest error";
}

KalshiRestClient::KalshiRestClient(HttpTransport& transport, KalshiEnvironment environment)
    : transport_(transport), environment_(environment) {}

Result<HttpResponse, RestError> KalshiRestClient::get(std::string_view path,
                                                      std::string_view query) {
    HttpRequest request;
    request.method = HttpMethod::Get;
    request.host = std::string{host_for(environment_)};
    request.target = std::string{kApiBasePath};
    request.target.append(path);
    if (!query.empty()) {
        request.target.push_back('?');
        request.target.append(query);
    }

    const auto response = transport_.send(request);
    if (!response) {
        return Failure{RestError{RestErrorKind::Transport, response.error(), 0}};
    }

    const int status = response->status_code;
    if (status >= 200 && status < 300) {
        // Copies the body. Market payloads are small and this happens once per
        // page at startup; if a benchmark ever says otherwise, the fix is a
        // move accessor on Result, not a redesign here.
        return *response;
    }

    RestErrorKind kind = RestErrorKind::UnexpectedStatus;
    if (status == 429) {
        kind = RestErrorKind::RateLimited;
    } else if (status == 404) {
        kind = RestErrorKind::NotFound;
    } else if (status >= 400 && status < 500) {
        kind = RestErrorKind::ClientError;
    } else if (status >= 500 && status < 600) {
        kind = RestErrorKind::ServerError;
    }

    return Failure{RestError{kind, HttpError::InvalidRequest, status}};
}

}  // namespace eventbook
