#pragma once

#include <chrono>
#include <memory>

#include "eventbook/api/http.hpp"

namespace eventbook {

/// HttpTransport backed by Boost.Beast over an OpenSSL TLS stream.
///
/// Boost and OpenSSL headers are deliberately absent from this file. They are
/// enormous, and every translation unit that merely wants to call a REST
/// endpoint would otherwise pay their compile cost. The implementation lives
/// behind a pointer -- the pimpl idiom -- so this header stays cheap and the
/// TLS details can change without recompiling everything downstream. That is
/// what AGENTS.md means by "keep public headers minimal".
///
/// One connection per request, with no pooling. That is wasteful and entirely
/// adequate: market discovery issues a handful of requests at startup. Adding
/// connection reuse before a benchmark says it matters would be optimizing
/// ahead of measurement.
class BeastHttpTransport final : public HttpTransport {
public:
    /// The timeout covers the whole exchange -- connect, handshake, write, and
    /// read -- rather than resetting for each step, so a server that trickles
    /// bytes cannot hold the caller indefinitely.
    explicit BeastHttpTransport(std::chrono::seconds timeout = std::chrono::seconds{15});
    ~BeastHttpTransport() override;

    [[nodiscard]] Result<HttpResponse, HttpError> send(const HttpRequest& request) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace eventbook
