#include "eventbook/api/beast_http_transport.hpp"

#include <boost/asio/connect.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>

#include <array>
#include <cstdlib>
#include <string>

#include "eventbook/common/version.hpp"

namespace eventbook {
namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = boost::beast::http;
namespace ssl = boost::asio::ssl;
using tcp = boost::asio::ip::tcp;

constexpr const char* kHttpsPort = "443";
constexpr int kHttpVersion11 = 11;

/// Point the TLS context at a certificate authority bundle.
///
/// Verification is mandatory and is never disabled anywhere in this project.
/// Without it TLS protects against a passive eavesdropper and not at all
/// against an active one, which for market data means silently accepting
/// whatever an interposed party chooses to serve.
///
/// The awkwardness is that OpenSSL's compiled-in default path frequently does
/// not exist on macOS, where the system bundle lives elsewhere, so a
/// vcpkg-built OpenSSL can end up trusting nothing at all and failing the
/// handshake in a way that looks like a network fault. The candidates below
/// cover the usual Debian, RHEL, Alpine, and macOS locations. SSL_CERT_FILE is
/// honoured first so an unusual environment can be corrected without a code
/// change. Loading several sources is fine -- OpenSSL merges trust anchors.
bool configure_trust_store(ssl::context& context) {
    boost::system::error_code ec;
    bool loaded = false;

    if (const char* override_path = std::getenv("SSL_CERT_FILE"); override_path != nullptr) {
        context.load_verify_file(override_path, ec);
        loaded = loaded || !ec;
    }

    static constexpr std::array<const char*, 4> kBundles{
        "/etc/ssl/certs/ca-certificates.crt",  // Debian, Ubuntu
        "/etc/pki/tls/certs/ca-bundle.crt",    // RHEL, Fedora
        "/etc/ssl/cert.pem",                   // macOS, Alpine
        "/usr/local/etc/openssl/cert.pem",     // Homebrew OpenSSL
    };
    for (const char* path : kBundles) {
        context.load_verify_file(path, ec);
        loaded = loaded || !ec;
    }

    context.set_default_verify_paths(ec);
    return loaded || !ec;
}

/// Beast reports a blown deadline as beast::error::timeout regardless of which
/// step blew it, so the distinction is preserved by the caller's fallback.
HttpError classify(const beast::error_code& ec, HttpError fallback) {
    return ec == beast::error::timeout ? HttpError::Timeout : fallback;
}

}  // namespace

struct BeastHttpTransport::Impl {
    explicit Impl(std::chrono::seconds request_timeout)
        : timeout(request_timeout), ssl_context(ssl::context::tls_client) {
        ssl_context.set_verify_mode(ssl::verify_peer);
        trust_store_ready = configure_trust_store(ssl_context);
    }

    std::chrono::seconds timeout;
    asio::io_context io_context;
    ssl::context ssl_context;
    bool trust_store_ready{false};
};

BeastHttpTransport::BeastHttpTransport(std::chrono::seconds timeout)
    : impl_(std::make_unique<Impl>(timeout)) {}

// Defined here, not in the header, because Impl is incomplete there and
// unique_ptr's deleter needs the complete type at the point of destruction.
BeastHttpTransport::~BeastHttpTransport() = default;

Result<HttpResponse, HttpError> BeastHttpTransport::send(const HttpRequest& request) {
    if (request.host.empty() || request.target.empty() || request.target.front() != '/') {
        return Failure{HttpError::InvalidRequest};
    }
    if (!impl_->trust_store_ready) {
        return Failure{HttpError::TlsContextFailed};
    }

    // Every Beast call below takes an error_code overload rather than throwing.
    // A refused connection or a dropped feed is an ordinary operating condition
    // for this system, and AGENTS.md reserves exceptions for irrecoverable
    // setup failures -- not for Tuesday.
    beast::error_code ec;

    tcp::resolver resolver(impl_->io_context);
    const auto endpoints = resolver.resolve(request.host, kHttpsPort, ec);
    if (ec) {
        return Failure{HttpError::ResolveFailed};
    }

    beast::ssl_stream<beast::tcp_stream> stream(impl_->io_context, impl_->ssl_context);

    // Server Name Indication. Kalshi sits behind shared infrastructure, so
    // without SNI the peer cannot know which certificate to present and
    // verification fails for reasons that look like a network problem.
    if (SSL_set_tlsext_host_name(stream.native_handle(), request.host.c_str()) != 1) {
        return Failure{HttpError::TlsContextFailed};
    }
    // Checks the certificate actually names this host. verify_peer alone only
    // proves the chain is trusted, not that it belongs to who we dialled.
    stream.set_verify_callback(ssl::host_name_verification(request.host), ec);
    if (ec) {
        return Failure{HttpError::TlsContextFailed};
    }

    beast::get_lowest_layer(stream).expires_after(impl_->timeout);

    beast::get_lowest_layer(stream).connect(endpoints, ec);
    if (ec) {
        return Failure{classify(ec, HttpError::ConnectFailed)};
    }

    stream.handshake(ssl::stream_base::client, ec);
    if (ec) {
        return Failure{classify(ec, HttpError::TlsHandshakeFailed)};
    }

    http::request<http::empty_body> outbound{http::verb::get, request.target, kHttpVersion11};
    outbound.set(http::field::host, request.host);
    outbound.set(http::field::user_agent, "eventbook/" + to_string(kVersion));
    outbound.set(http::field::accept, "application/json");

    http::write(stream, outbound, ec);
    if (ec) {
        return Failure{classify(ec, HttpError::WriteFailed)};
    }

    beast::flat_buffer buffer;
    http::response<http::string_body> inbound;
    http::read(stream, buffer, inbound, ec);
    if (ec) {
        return Failure{classify(ec, HttpError::ReadFailed)};
    }

    // A peer that closes without close_notify yields stream_truncated. That is
    // routine for HTTP servers and says nothing about the exchange we just
    // completed, so the response is kept and the shutdown result discarded.
    beast::error_code shutdown_ec;
    stream.shutdown(shutdown_ec);

    return HttpResponse{static_cast<int>(inbound.result_int()), inbound.body()};
}

}  // namespace eventbook
