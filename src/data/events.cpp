#include "eventbook/data/events.hpp"

namespace eventbook {

std::string_view to_string(BookSide side) {
    switch (side) {
        case BookSide::Bid:
            return "bid";
        case BookSide::Ask:
            return "ask";
    }
    return "unknown";
}

std::string_view to_string(TradeSide side) {
    switch (side) {
        case TradeSide::BuyYes:
            return "buy_yes";
        case TradeSide::SellYes:
            return "sell_yes";
    }
    return "unknown";
}

std::optional<MarketTicker> market_ticker_of(const MarketEvent& event) {
    return std::visit(
        [](const auto& payload) -> std::optional<MarketTicker> {
            using Payload = std::decay_t<decltype(payload)>;
            if constexpr (std::is_same_v<Payload, BookSnapshot> ||
                          std::is_same_v<Payload, BookDelta> ||
                          std::is_same_v<Payload, PublicTrade>) {
                return payload.market_ticker;
            } else if constexpr (std::is_same_v<Payload, StreamError>) {
                return payload.market_ticker;
            } else {
                return std::nullopt;
            }
        },
        event);
}

std::optional<SequenceNumber> sequence_of(const MarketEvent& event) {
    return std::visit(
        [](const auto& payload) -> std::optional<SequenceNumber> {
            using Payload = std::decay_t<decltype(payload)>;
            if constexpr (std::is_same_v<Payload, BookSnapshot> ||
                          std::is_same_v<Payload, BookDelta>) {
                return payload.sequence;
            } else {
                return std::nullopt;
            }
        },
        event);
}

}  // namespace eventbook
