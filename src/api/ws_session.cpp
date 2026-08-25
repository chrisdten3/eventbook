#include "eventbook/api/ws_session.hpp"

#include <boost/asio/connect.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>
#include <nlohmann/json.hpp>

#include <array>
#include <cstdlib>
#include <utility>

#include "eventbook/common/version.hpp"

namespace eventbook {
namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace ssl = boost::asio::ssl;
namespace websocket = boost::beast::websocket;
using tcp = boost::asio::ip::tcp;

constexpr const char* kHttpsPort = "443";

// Duplicated from beast_http_transport.cpp rather than shared, because the two
// will diverge: this one will eventually want pinning or a configured bundle,
// and coupling them now would make that change touch REST as well. See that
// file for why the candidate list exists at all.
bool configure_trust_store(ssl::context& context) {
    boost::system::error_code ec;
    bool loaded = false;

    if (const char* override_path = std::getenv("SSL_CERT_FILE"); override_path != nullptr) {
        context.load_verify_file(override_path, ec);
        loaded = loaded || !ec;
    }
    static constexpr std::array<const char*, 4> kBundles{
        "/etc/ssl/certs/ca-certificates.crt", "/etc/pki/tls/certs/ca-bundle.crt",
        "/etc/ssl/cert.pem", "/usr/local/etc/openssl/cert.pem"};
    for (const char* path : kBundles) {
        context.load_verify_file(path, ec);
        loaded = loaded || !ec;
    }
    context.set_default_verify_paths(ec);
    return loaded || !ec;
}

// See beast_http_transport.cpp: the OpenSSL SNI macro contains a C-style cast
// that GCC attributes to the expansion site.
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wold-style-cast"
#endif
bool set_sni_hostname(::SSL* handle, const char* host) {
    return SSL_set_tlsext_host_name(handle, host) == 1;
}
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

}  // namespace

std::string_view websocket_host(KalshiEnvironment environment) {
    switch (environment) {
        case KalshiEnvironment::Production:
            return "external-api-ws.kalshi.com";
        case KalshiEnvironment::Demo:
            return "external-api-ws.demo.kalshi.co";
    }
    return "external-api-ws.kalshi.com";
}

std::string build_subscribe_command(std::int64_t command_id, const WsSubscription& subscription) {
    nlohmann::json command;
    command["id"] = command_id;
    command["cmd"] = "subscribe";
    command["params"]["channels"] = subscription.channels;
    command["params"]["market_ticker"] = subscription.market_ticker.value;
    command["params"]["use_yes_price"] = subscription.use_yes_price;
    return command.dump();
}

std::chrono::milliseconds reconnect_backoff(unsigned attempt, std::chrono::milliseconds initial,
                                            std::chrono::milliseconds cap) {
    if (initial <= std::chrono::milliseconds::zero()) {
        return std::chrono::milliseconds::zero();
    }
    auto delay = initial;
    // Shift rather than pow, and stop the moment the cap is reached, so a large
    // attempt count cannot overflow the duration on its way to being clamped.
    for (unsigned step = 0; step < attempt; ++step) {
        if (delay >= cap) {
            break;
        }
        delay *= 2;
    }
    return delay > cap ? cap : delay;
}

std::string_view to_string(WsSessionNotice notice) {
    switch (notice) {
        case WsSessionNotice::Connecting:
            return "connecting";
        case WsSessionNotice::Connected:
            return "connected";
        case WsSessionNotice::Subscribed:
            return "subscribed";
        case WsSessionNotice::Disconnected:
            return "disconnected";
        case WsSessionNotice::Reconnecting:
            return "reconnecting";
        case WsSessionNotice::ParseFailure:
            return "parse_failure";
        case WsSessionNotice::StreamErrorReceived:
            return "stream_error";
        case WsSessionNotice::GaveUp:
            return "gave_up";
        case WsSessionNotice::Stopped:
            return "stopped";
    }
    return "unknown";
}

struct WebSocketSession::Impl {
    using Stream = websocket::stream<beast::ssl_stream<beast::tcp_stream>>;

    Impl(WsSessionConfig configuration, RequestSigner request_signer)
        : config(std::move(configuration)),
          signer(std::move(request_signer)),
          ssl_context(ssl::context::tls_client),
          resolver(io_context),
          retry_timer(io_context) {
        ssl_context.set_verify_mode(ssl::verify_peer);
        trust_store_ready = configure_trust_store(ssl_context);
    }

    void notify(WsSessionNotice notice, std::string_view detail = {}) {
        if (notice_handler) {
            notice_handler(notice, detail);
        }
    }

    void fail(std::string_view stage, const beast::error_code& ec) {
        notify(WsSessionNotice::Disconnected, std::string{stage} + ": " + ec.message());
        schedule_reconnect();
    }

    void schedule_reconnect() {
        stream.reset();
        if (stopping) {
            return;
        }
        if (config.max_reconnect_attempts != 0 && attempt >= config.max_reconnect_attempts) {
            notify(WsSessionNotice::GaveUp, "reconnect attempts exhausted");
            io_context.stop();
            return;
        }

        const auto delay = reconnect_backoff(attempt, config.initial_backoff, config.max_backoff);
        ++attempt;
        ++stats.reconnects;
        notify(WsSessionNotice::Reconnecting, std::to_string(delay.count()) + "ms");

        retry_timer.expires_after(delay);
        retry_timer.async_wait([this](const beast::error_code& ec) {
            if (!ec && !stopping) {
                connect();
            }
        });
    }

    void connect() {
        if (!trust_store_ready) {
            notify(WsSessionNotice::GaveUp, "no certificate trust store available");
            io_context.stop();
            return;
        }

        notify(WsSessionNotice::Connecting, std::string{websocket_host(config.environment)});
        stream.emplace(io_context, ssl_context);

        const std::string host{websocket_host(config.environment)};
        resolver.async_resolve(
            host, kHttpsPort,
            [this](const beast::error_code& ec, const tcp::resolver::results_type& endpoints) {
                if (ec) {
                    fail("resolve", ec);
                    return;
                }
                on_resolved(endpoints);
            });
    }

    void on_resolved(const tcp::resolver::results_type& endpoints) {
        auto& lowest = beast::get_lowest_layer(*stream);
        lowest.expires_after(config.handshake_timeout);
        lowest.async_connect(endpoints, [this](const beast::error_code& ec, const tcp::endpoint&) {
            if (ec) {
                fail("connect", ec);
                return;
            }
            on_connected();
        });
    }

    void on_connected() {
        const std::string host{websocket_host(config.environment)};
        if (!set_sni_hostname(stream->next_layer().native_handle(), host.c_str())) {
            notify(WsSessionNotice::Disconnected, "could not set SNI");
            schedule_reconnect();
            return;
        }
        beast::error_code verify_ec;
        stream->next_layer().set_verify_callback(ssl::host_name_verification(host), verify_ec);
        if (verify_ec) {
            fail("verify_setup", verify_ec);
            return;
        }

        stream->next_layer().async_handshake(ssl::stream_base::client,
                                             [this](const beast::error_code& ec) {
                                                 if (ec) {
                                                     fail("tls_handshake", ec);
                                                     return;
                                                 }
                                                 on_tls_ready();
                                             });
    }

    void on_tls_ready() {
        // Disarm the TCP-layer deadline before handing timing to the WebSocket
        // layer. websocket::stream runs its own timeout system, and the
        // expires_after() armed for the handshake stays live underneath it
        // otherwise -- which closed a perfectly healthy connection every
        // handshake_timeout seconds, on a market sending nine messages a
        // second, for reasons that looked exactly like a network fault.
        beast::get_lowest_layer(*stream).expires_never();

        // Beast's own timeouts replace a hand-rolled timer, and crucially they
        // treat control frames as traffic. Kalshi pings every 10 seconds, so a
        // market that trades nothing for minutes still looks alive -- which is
        // the difference between a quiet market and a dead connection.
        websocket::stream_base::timeout timeout{};
        timeout.handshake_timeout = config.handshake_timeout;
        timeout.idle_timeout = config.idle_timeout;
        timeout.keep_alive_pings = true;
        stream->set_option(timeout);

        // Beast answers incoming pings with pongs automatically; the callback is
        // only here so heartbeats are observable rather than invisible.
        stream->control_callback([this](websocket::frame_type kind, beast::string_view payload) {
            if (kind == websocket::frame_type::ping) {
                stats.bytes_received += payload.size();
            }
        });

        // The timestamp must be fresh at handshake time, so the signature is
        // produced here rather than once at construction.
        auto signature = signer.sign_get(kWebSocketPath, local_now());
        if (!signature) {
            notify(WsSessionNotice::GaveUp,
                   std::string{"signing failed: "} + std::string{to_string(signature.error())});
            io_context.stop();
            return;
        }

        stream->set_option(websocket::stream_base::decorator(
            [auth = *signature](websocket::request_type& request) {
                request.set("KALSHI-ACCESS-KEY", auth.key_id);
                request.set("KALSHI-ACCESS-TIMESTAMP", auth.timestamp_millis);
                request.set("KALSHI-ACCESS-SIGNATURE", auth.signature);
                request.set(beast::http::field::user_agent, "eventbook/" + to_string(kVersion));
            }));

        const std::string host{websocket_host(config.environment)};
        stream->async_handshake(host, std::string{kWebSocketPath},
                                [this](const beast::error_code& ec) {
                                    if (ec) {
                                        fail("ws_handshake", ec);
                                        return;
                                    }
                                    on_open();
                                });
    }

    void on_open() {
        ++stats.connections;
        attempt = 0;
        notify(WsSessionNotice::Connected);

        const auto command = build_subscribe_command(++command_id, config.subscription);
        stream->async_write(asio::buffer(command),
                            [this](const beast::error_code& ec, std::size_t) {
                                if (ec) {
                                    fail("subscribe", ec);
                                    return;
                                }
                                read();
                            });
    }

    void read() {
        buffer.clear();
        stream->async_read(buffer, [this](const beast::error_code& ec, std::size_t bytes) {
            if (ec) {
                if (stopping) {
                    return;
                }
                fail("read", ec);
                return;
            }
            on_message(bytes);
        });
    }

    void on_message(std::size_t bytes) {
        ++stats.messages_received;
        stats.bytes_received += bytes;

        const auto text = beast::buffers_to_string(buffer.data());
        auto event = parse_ws_message(text, convention());
        if (!event) {
            // Never silently discarded: counted, surfaced, and the raw text is
            // handed to the notice handler so a journal can keep it.
            ++stats.parse_failures;
            notify(WsSessionNotice::ParseFailure, text);
            read();
            return;
        }

        if (std::holds_alternative<UnhandledMessage>(*event)) {
            ++stats.unhandled_messages;
        } else if (std::holds_alternative<StreamError>(*event)) {
            ++stats.stream_errors;
            notify(WsSessionNotice::StreamErrorReceived, std::get<StreamError>(*event).message);
        } else if (std::holds_alternative<SubscriptionAck>(*event)) {
            notify(WsSessionNotice::Subscribed, std::get<SubscriptionAck>(*event).channel);
        }

        if (event_handler) {
            event_handler(*event);
        }
        read();
    }

    [[nodiscard]] PriceConvention convention() const {
        return config.subscription.use_yes_price ? PriceConvention::YesLegPricing
                                                 : PriceConvention::NoLegPricing;
    }

    // Declared first so it is destroyed last: any handler still queued when the
    // context is torn down merely holds a pointer, and is destroyed rather than
    // invoked.
    asio::io_context io_context;

    WsSessionConfig config;
    RequestSigner signer;
    ssl::context ssl_context;
    tcp::resolver resolver;
    asio::steady_timer retry_timer;
    std::optional<Stream> stream;
    beast::flat_buffer buffer;

    WebSocketSession::EventHandler event_handler;
    WebSocketSession::NoticeHandler notice_handler;
    WsSessionStats stats;

    bool trust_store_ready{false};
    bool stopping{false};
    unsigned attempt{0};
    std::int64_t command_id{0};
};

WebSocketSession::WebSocketSession(WsSessionConfig config, RequestSigner signer)
    : impl_(std::make_unique<Impl>(std::move(config), std::move(signer))) {}

WebSocketSession::~WebSocketSession() = default;

void WebSocketSession::on_event(EventHandler handler) {
    impl_->event_handler = std::move(handler);
}

void WebSocketSession::on_notice(NoticeHandler handler) {
    impl_->notice_handler = std::move(handler);
}

void WebSocketSession::run() {
    impl_->stopping = false;
    impl_->connect();
    impl_->io_context.run();
    impl_->notify(WsSessionNotice::Stopped);
}

void WebSocketSession::reconnect() {
    // Posted so the teardown happens on the io_context's thread even when the
    // request comes from inside an event handler, which is the normal case: the
    // book invalidates while handling a delta and asks for a fresh snapshot.
    asio::post(impl_->io_context, [impl = impl_.get()] {
        if (impl->stopping) {
            return;
        }
        // A requested reconnect is not a failure, so the backoff ladder starts
        // over rather than inheriting a previous failure's delay.
        impl->attempt = 0;
        if (impl->stream) {
            beast::error_code ec;
            impl->stream->close(websocket::close_code::normal, ec);
        }
        impl->schedule_reconnect();
    });
}

void WebSocketSession::stop() {
    // Posted rather than executed directly so that shutdown always happens on
    // the io_context's thread, whatever thread asked for it.
    asio::post(impl_->io_context, [impl = impl_.get()] {
        impl->stopping = true;
        impl->retry_timer.cancel();
        if (impl->stream) {
            beast::error_code ec;
            impl->stream->close(websocket::close_code::normal, ec);
        }
        impl->io_context.stop();
    });
}

const WsSessionStats& WebSocketSession::stats() const {
    return impl_->stats;
}

PriceConvention WebSocketSession::price_convention() const {
    return impl_->convention();
}

}  // namespace eventbook
