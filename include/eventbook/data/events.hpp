#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "eventbook/common/identifiers.hpp"
#include "eventbook/common/price.hpp"
#include "eventbook/common/quantity.hpp"
#include "eventbook/common/time.hpp"

namespace eventbook {

/// Which side of the book a level sits on, ALWAYS on the YES price scale.
///
/// Kalshi does not publish a bid/ask pair. It publishes YES bids and NO bids,
/// and a NO bid is economically an offer to sell YES: "buy NO at $0.30" and
/// "sell YES at $0.70" are the same order. Everything downstream of this file
/// works in one currency -- YES dollars -- so the conversion happens once, at
/// the boundary, rather than being rediscovered by the book, the feature
/// engine, and the simulator independently.
enum class BookSide {
    Bid,  ///< buying YES
    Ask,  ///< selling YES, i.e. a NO bid reflected onto the YES scale
};

[[nodiscard]] std::string_view to_string(BookSide side);

/// Direction of a public trade, expressed on the YES scale.
///
/// Derived from the venue's taker fields, where `bid` is equivalent to outcome
/// `yes` and `ask` to `no`. A taker who bought NO has, in YES terms, sold --
/// so naming this on the YES scale means the signed-trade-flow feature in M4
/// never has to re-derive the convention, and cannot get the sign backwards.
enum class TradeSide {
    BuyYes,
    SellYes,
};

[[nodiscard]] std::string_view to_string(TradeSide side);

struct BookLevel {
    Price price;
    Quantity quantity;

    [[nodiscard]] friend constexpr bool operator==(const BookLevel&, const BookLevel&) = default;
};

/// A complete replacement for a market's book state.
///
/// Bids are ordered best (highest) first and asks best (lowest) first, so the
/// top of book is always `front()`. The venue does not guarantee that ordering
/// -- reflecting NO levels onto the YES scale reverses them -- so it is imposed
/// here.
struct BookSnapshot {
    MarketTicker market_ticker;
    SubscriptionId subscription;
    SequenceNumber sequence;
    std::vector<BookLevel> bids;
    std::vector<BookLevel> asks;
};

/// An incremental change to one price level.
struct BookDelta {
    MarketTicker market_ticker;
    SubscriptionId subscription;
    SequenceNumber sequence;
    BookSide side;
    Price price;
    QuantityDelta delta;
    std::optional<ExchangeTimestamp> exchange_time;
};

struct PublicTrade {
    MarketTicker market_ticker;
    SubscriptionId subscription;
    std::string trade_id;
    Price yes_price;
    Quantity quantity;
    TradeSide taker_side;
    bool is_block_trade{};
    std::optional<ExchangeTimestamp> exchange_time;
};

/// The venue confirming a subscription and assigning its `sid`.
struct SubscriptionAck {
    std::int64_t command_id{};
    std::string channel;
    SubscriptionId subscription;
};

/// An error the venue reported for a command we sent.
struct StreamError {
    std::int64_t command_id{};
    int code{};
    std::string message;
    std::optional<MarketTicker> market_ticker;
};

/// A well-formed message of a type this build does not handle.
///
/// Deliberately an event rather than a parse failure. The venue adds channels
/// and message types over time, and a recorder that treats an unrecognized type
/// as an error would stop recording for no reason. AGENTS.md requires that
/// nothing is silently discarded, so this carries the type forward to be
/// counted while the raw payload is journalled intact.
struct UnhandledMessage {
    std::string type;
};

/// Everything the WebSocket can deliver, as one tagged union.
///
/// A variant rather than an inheritance hierarchy: these types share no
/// behaviour, only the fact that exactly one of them arrives at a time, and
/// std::visit makes forgetting a case a compile error.
using MarketEvent = std::variant<BookSnapshot, BookDelta, PublicTrade, SubscriptionAck, StreamError,
                                 UnhandledMessage>;

/// The ticker an event concerns, when it concerns one.
[[nodiscard]] std::optional<MarketTicker> market_ticker_of(const MarketEvent& event);

/// The venue timestamp an event carries, when it carries one.
///
/// Snapshots and acknowledgements have none; deltas and trades do. Recording it
/// on a journal record is what lets a derived feature row be traced back to the
/// venue's own clock, not merely to ours.
[[nodiscard]] std::optional<ExchangeTimestamp> exchange_time_of(const MarketEvent& event);

/// The sequence number an event carries, when it carries one.
///
/// Only snapshots and deltas participate in the per-subscription sequence;
/// acknowledgements and errors do not, so gap detection must ignore them
/// rather than treating a missing seq as a gap.
[[nodiscard]] std::optional<SequenceNumber> sequence_of(const MarketEvent& event);

}  // namespace eventbook
