#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "eventbook/api/rest_client.hpp"
#include "eventbook/api/signing.hpp"
#include "eventbook/api/ws_protocol.hpp"
#include "eventbook/common/identifiers.hpp"
#include "eventbook/data/events.hpp"

namespace eventbook {

/// Path of the WebSocket endpoint. Also the exact path that gets signed.
inline constexpr std::string_view kWebSocketPath = "/trade-api/ws/v2";

/// Host for the WebSocket, which is a DIFFERENT subdomain from REST:
/// external-api-ws.kalshi.com against external-api.kalshi.com. Production is
/// .com and demo is .co, as with REST.
[[nodiscard]] std::string_view websocket_host(KalshiEnvironment environment);

/// What to subscribe to once connected.
struct WsSubscription {
    MarketTicker market_ticker;
    std::vector<std::string> channels{"orderbook_delta", "trade"};

    /// AGENTS.md directs subscribing on the YES-price scale explicitly rather
    /// than relying on a default that may change. The session derives the
    /// matching PriceConvention from this flag, so the two cannot disagree --
    /// and disagreeing would silently mirror every ask.
    bool use_yes_price{true};
};

/// The subscribe command, as a JSON string ready to send.
///
/// Pure and exposed so it can be tested without a socket, which is most of what
/// can be verified about this layer offline.
[[nodiscard]] std::string build_subscribe_command(std::int64_t command_id,
                                                  const WsSubscription& subscription);

/// Exponential backoff, doubling per attempt and capped.
///
/// Attempt 0 is the first retry. Capped because an unbounded doubling reaches
/// hours, and a recorder that gives up for hours after a transient blip has
/// silently ended the session it was meant to record.
[[nodiscard]] std::chrono::milliseconds reconnect_backoff(unsigned attempt,
                                                          std::chrono::milliseconds initial,
                                                          std::chrono::milliseconds cap);

struct WsSessionConfig {
    KalshiEnvironment environment{KalshiEnvironment::Production};
    WsSubscription subscription;

    std::chrono::seconds handshake_timeout{20};

    /// How long without ANY traffic before the connection is considered dead.
    ///
    /// Kalshi sends a Ping every 10 seconds, and control frames count as
    /// traffic, so this measures liveness of the connection rather than
    /// activity in the market. That distinction is essential: a quiet market
    /// can legitimately produce no order-book messages for minutes, and
    /// treating silence as failure would reconnect in a loop and log gaps that
    /// never happened.
    std::chrono::seconds idle_timeout{35};

    std::chrono::milliseconds initial_backoff{500};
    std::chrono::milliseconds max_backoff{30'000};

    /// 0 means retry indefinitely.
    unsigned max_reconnect_attempts{0};
};

/// Lifecycle and data-quality notifications.
///
/// Separate from MarketEvent because these are facts about our connection
/// rather than about the market. AGENTS.md requires per-session counts of
/// connects, reconnects, parse failures, and time spent invalid; these are what
/// feed them.
enum class WsSessionNotice {
    Connecting,
    Connected,
    Subscribed,
    Disconnected,
    Reconnecting,
    ParseFailure,
    StreamErrorReceived,
    GaveUp,
    Stopped,
};

[[nodiscard]] std::string_view to_string(WsSessionNotice notice);

/// Counters AGENTS.md requires a session to report.
struct WsSessionStats {
    std::uint64_t connections{};
    std::uint64_t reconnects{};
    std::uint64_t messages_received{};
    std::uint64_t bytes_received{};
    std::uint64_t parse_failures{};
    std::uint64_t unhandled_messages{};
    std::uint64_t stream_errors{};
};

/// An authenticated, read-only WebSocket session for one market.
///
/// Asynchronous underneath, single-threaded on top. run() drives one io_context
/// on the calling thread, so every handler -- and therefore every mutation of
/// whatever consumes these events -- happens on one ordered path. AGENTS.md puts
/// determinism ahead of concurrency, and the point of asynchrony here is
/// juggling reads, timers, and control frames on one thread, not using several.
///
/// Ownership across callbacks is the hazard this design avoids by construction.
/// The implementation owns its io_context, and run() blocks until that context
/// stops, so the session cannot be destroyed while a handler is pending: the
/// caller's thread is inside run() for the whole lifetime of every callback.
/// That is why handlers can capture a plain pointer rather than needing
/// shared_from_this.
///
/// Read-only: the only frames sent are subscribe commands and protocol-level
/// pongs. There is no code path that can place, amend, or cancel an order.
class WebSocketSession {
public:
    using EventHandler = std::function<void(const MarketEvent&)>;
    using NoticeHandler = std::function<void(WsSessionNotice, std::string_view)>;

    /// Called for every frame, with the bytes exactly as they arrived and the
    /// normalization result -- nullptr when the payload did not parse.
    ///
    /// Invoked BEFORE the event handler, so a journal records what the venue
    /// sent before anything interprets it. AGENTS.md requires the raw payload
    /// to be preserved before transformations precisely so that a parser bug is
    /// fixable by replaying the journal; a payload that failed to parse is the
    /// one most worth having kept, which is why this fires either way.
    using RawHandler = std::function<void(std::string_view payload, LocalTimestamp received_at,
                                          const MarketEvent* event)>;

    WebSocketSession(WsSessionConfig config, RequestSigner signer);
    ~WebSocketSession();

    WebSocketSession(const WebSocketSession&) = delete;
    WebSocketSession& operator=(const WebSocketSession&) = delete;
    WebSocketSession(WebSocketSession&&) = delete;
    WebSocketSession& operator=(WebSocketSession&&) = delete;

    void on_event(EventHandler handler);
    void on_raw(RawHandler handler);
    void on_notice(NoticeHandler handler);

    /// Run until stop() is called, the retry budget is exhausted, or the
    /// process is interrupted. Blocks the calling thread.
    void run();

    /// Drop the connection and establish a new one, which re-subscribes and
    /// yields a fresh snapshot.
    ///
    /// This is the recovery path for a book that has gone invalid. A snapshot
    /// is the only way back -- the book refuses deltas until one arrives -- and
    /// without an explicit request the only thing that would eventually deliver
    /// one is an idle timeout, which is recovery by accident rather than by
    /// design. Safe to call from inside an event handler.
    void reconnect();

    /// Ask the session to shut down. Safe to call from a signal handler context
    /// or from inside an event handler.
    void stop();

    [[nodiscard]] const WsSessionStats& stats() const;

    /// How many connections this session has opened. Increments on every
    /// reconnect, so a journal can record which records share an uninterrupted
    /// stream -- and therefore which sequence numbers are comparable.
    [[nodiscard]] std::uint64_t connection_id() const;

    /// The convention the session subscribed with, for feeding the normalizer.
    [[nodiscard]] PriceConvention price_convention() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace eventbook
