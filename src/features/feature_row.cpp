#include "eventbook/features/feature_row.hpp"

#include <fmt/format.h>

#include <string>

namespace eventbook {
namespace {

/// Empty stays empty. Writing 0 or NaN for a missing value would let a reader
/// mistake "not defined here" for "measured as zero", which is the difference
/// between a balanced book and no book at all.
std::string optional_price_dollars(const std::optional<Price>& price) {
    return price.has_value() ? format_price(*price) : std::string{};
}

std::string optional_units_as_dollars(const std::optional<double>& units) {
    if (!units.has_value()) {
        return {};
    }
    return fmt::format("{:.6f}", *units / static_cast<double>(Price::kUnitsPerDollar));
}

std::string optional_ratio(const std::optional<double>& value) {
    return value.has_value() ? fmt::format("{:.6f}", *value) : std::string{};
}

std::string optional_integer(const std::optional<std::int64_t>& value) {
    return value.has_value() ? std::to_string(*value) : std::string{};
}

}  // namespace

std::string feature_row_header(const std::vector<std::chrono::seconds>& windows) {
    std::string header =
        "market_ticker,sample_time_us,last_exchange_time_us,last_sequence,book_valid,"
        "best_bid,best_ask,midpoint,spread,"
        "bid_depth_1,bid_depth_3,bid_depth_5,ask_depth_1,ask_depth_3,ask_depth_5,"
        "imbalance_1,imbalance_3,imbalance_5,"
        "microprice,microprice_displacement,bid_levels,ask_levels";
    for (const auto window : windows) {
        const auto suffix = std::to_string(window.count()) + "s";
        header += ",trades_" + suffix;
        header += ",trade_volume_" + suffix;
        header += ",signed_trade_volume_" + suffix;
        header += ",trade_flow_imbalance_" + suffix;
        header += ",book_adds_" + suffix;
        header += ",book_removes_" + suffix;
        header += ",realized_vol_" + suffix;
    }
    return header;
}

std::string to_csv(const FeatureRow& row) {
    std::string line = fmt::format(
        "{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{}",
        row.market_ticker.value, epoch_micros(row.sample_time),
        row.last_exchange_time.has_value() ? std::to_string(epoch_micros(*row.last_exchange_time))
                                           : std::string{},
        row.last_sequence.has_value() ? std::to_string(row.last_sequence->value) : std::string{},
        row.book_valid ? 1 : 0, optional_price_dollars(row.best_bid),
        optional_price_dollars(row.best_ask), optional_units_as_dollars(row.midpoint),
        row.spread.has_value() ? format_price(Price{row.spread->units}) : std::string{},
        format_quantity(row.bid_depth_1), format_quantity(row.bid_depth_3),
        format_quantity(row.bid_depth_5), format_quantity(row.ask_depth_1),
        format_quantity(row.ask_depth_3), format_quantity(row.ask_depth_5),
        optional_ratio(row.imbalance_1), optional_ratio(row.imbalance_3),
        optional_ratio(row.imbalance_5), optional_units_as_dollars(row.microprice),
        optional_units_as_dollars(row.microprice_displacement), optional_integer(row.bid_levels),
        optional_integer(row.ask_levels));

    for (const auto& window : row.windows) {
        line += fmt::format(
            ",{},{},{},{},{},{},{}", window.trades, format_quantity(window.trade_volume),
            format_quantity(Quantity{window.signed_trade_volume}),
            optional_ratio(window.trade_flow_imbalance), window.book_adds, window.book_removes,
            optional_units_as_dollars(window.realized_volatility));
    }
    return line;
}

}  // namespace eventbook
