#include "eventbook/features/feature_engine.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <vector>

using Catch::Matchers::WithinAbs;
using eventbook::BookLevel;
using eventbook::BookSnapshot;
using eventbook::compute_features;
using eventbook::FeatureRow;
using eventbook::FeatureSampler;
using eventbook::local_time_from_epoch_micros;
using eventbook::MarketState;
using eventbook::MarketTicker;
using eventbook::Price;
using eventbook::PriceDelta;
using eventbook::PriceRange;
using eventbook::Quantity;
using eventbook::SequenceNumber;
using eventbook::SubscriptionId;

namespace {

const MarketTicker kTicker{"TEST-1"};
const std::vector<PriceRange> kCentGrid{PriceRange{Price{0}, Price{10000}, PriceDelta{100}}};

MarketState state_with(std::vector<BookLevel> bids, std::vector<BookLevel> asks) {
    MarketState state{kTicker, kCentGrid};
    BookSnapshot snapshot;
    snapshot.market_ticker = kTicker;
    snapshot.subscription = SubscriptionId{1};
    snapshot.sequence = SequenceNumber{1};
    snapshot.bids = std::move(bids);
    snapshot.asks = std::move(asks);
    (void)state.apply(eventbook::MarketEvent{snapshot}, local_time_from_epoch_micros(0));
    return state;
}

FeatureRow features_of(const MarketState& state) {
    return compute_features(state, kTicker, local_time_from_epoch_micros(1'000'000), std::nullopt,
                            std::nullopt);
}

}  // namespace

TEST_CASE("book features are derived from the top of book") {
    const auto state = state_with({{Price{4800}, Quantity{1000}}, {Price{4700}, Quantity{500}}},
                                  {{Price{5200}, Quantity{400}}, {Price{5300}, Quantity{600}}});
    const auto row = features_of(state);

    CHECK(row.book_valid);
    CHECK(row.best_bid == Price{4800});
    CHECK(row.best_ask == Price{5200});
    CHECK(row.spread == PriceDelta{400});
    REQUIRE(row.midpoint.has_value());
    CHECK_THAT(*row.midpoint, WithinAbs(5000.0, 1e-9));
    CHECK(row.bid_levels == 2);
    CHECK(row.ask_levels == 2);
}

TEST_CASE("a midpoint between adjacent ticks lands on a half unit") {
    // Why midpoint is a double and not a Price.
    const auto state = state_with({{Price{4800}, Quantity{10}}}, {{Price{4900}, Quantity{10}}});
    const auto row = features_of(state);
    REQUIRE(row.midpoint.has_value());
    CHECK_THAT(*row.midpoint, WithinAbs(4850.0, 1e-9));
}

TEST_CASE("depth accumulates over the best k levels") {
    const auto state = state_with({{Price{4800}, Quantity{100}},
                                   {Price{4700}, Quantity{200}},
                                   {Price{4600}, Quantity{300}},
                                   {Price{4500}, Quantity{400}}},
                                  {{Price{5200}, Quantity{50}}, {Price{5300}, Quantity{70}}});
    const auto row = features_of(state);

    CHECK(row.bid_depth_1 == Quantity{100});
    CHECK(row.bid_depth_3 == Quantity{600});
    CHECK(row.bid_depth_5 == Quantity{1000});  // only four levels exist
    CHECK(row.ask_depth_1 == Quantity{50});
    CHECK(row.ask_depth_5 == Quantity{120});
}

TEST_CASE("imbalance is signed toward the heavier side") {
    const auto state = state_with({{Price{4800}, Quantity{300}}}, {{Price{5200}, Quantity{100}}});
    const auto row = features_of(state);

    // (300 - 100) / 400 = +0.5, positive because bids dominate.
    REQUIRE(row.imbalance_1.has_value());
    CHECK_THAT(*row.imbalance_1, WithinAbs(0.5, 1e-9));
}

TEST_CASE("imbalance is bounded and symmetric") {
    const auto bid_only = state_with({{Price{4800}, Quantity{300}}}, {});
    CHECK_THAT(*features_of(bid_only).imbalance_1, WithinAbs(1.0, 1e-9));

    const auto ask_only = state_with({}, {{Price{5200}, Quantity{300}}});
    CHECK_THAT(*features_of(ask_only).imbalance_1, WithinAbs(-1.0, 1e-9));

    const auto balanced =
        state_with({{Price{4800}, Quantity{250}}}, {{Price{5200}, Quantity{250}}});
    CHECK_THAT(*features_of(balanced).imbalance_1, WithinAbs(0.0, 1e-9));
}

TEST_CASE("imbalance is missing rather than zero when both sides are empty") {
    // AGENTS.md requires this case be reported missing. A fabricated zero would
    // be indistinguishable from a genuinely balanced book.
    const auto empty = state_with({}, {});
    const auto row = features_of(empty);
    CHECK(row.book_valid);
    CHECK_FALSE(row.imbalance_1.has_value());
    CHECK_FALSE(row.imbalance_5.has_value());
    CHECK_FALSE(row.microprice.has_value());
}

TEST_CASE("microprice leans toward the side with less resting size") {
    // The thin side is the one more likely to be taken out, so the size-weighted
    // midpoint sits closer to it.
    const auto thin_ask =
        state_with({{Price{4800}, Quantity{900}}}, {{Price{5200}, Quantity{100}}});
    const auto row = features_of(thin_ask);

    REQUIRE(row.microprice.has_value());
    REQUIRE(row.midpoint.has_value());
    // (5200*900 + 4800*100) / 1000 = 5160
    CHECK_THAT(*row.microprice, WithinAbs(5160.0, 1e-9));
    CHECK(*row.microprice > *row.midpoint);
    CHECK_THAT(*row.microprice_displacement, WithinAbs(160.0, 1e-9));
}

TEST_CASE("microprice equals the midpoint when both sides are equally deep") {
    const auto state = state_with({{Price{4800}, Quantity{500}}}, {{Price{5200}, Quantity{500}}});
    const auto row = features_of(state);
    CHECK_THAT(*row.microprice, WithinAbs(*row.midpoint, 1e-9));
    CHECK_THAT(*row.microprice_displacement, WithinAbs(0.0, 1e-9));
}

TEST_CASE("an invalid book produces a marked row with no features") {
    // AGENTS.md forbids invalid intervals producing normal rows. The row is
    // still emitted so the hole is visible in the time series.
    MarketState state{kTicker, kCentGrid};  // never snapshotted
    const auto row = features_of(state);

    CHECK_FALSE(row.book_valid);
    CHECK_FALSE(row.best_bid.has_value());
    CHECK_FALSE(row.midpoint.has_value());
    CHECK_FALSE(row.imbalance_1.has_value());
    CHECK_FALSE(row.microprice.has_value());
    CHECK(row.bid_depth_5 == Quantity{});
}

TEST_CASE("the sampler emits one row per interval boundary") {
    const auto state = state_with({{Price{4800}, Quantity{100}}}, {{Price{5200}, Quantity{100}}});
    FeatureSampler sampler{kTicker};

    std::vector<FeatureRow> rows;
    const auto sink = [&](const FeatureRow& row) { rows.push_back(row); };

    sampler.advance(state, local_time_from_epoch_micros(1'000'000), sink);
    sampler.advance(state, local_time_from_epoch_micros(4'500'000), sink);

    // Boundaries at 2s, 3s, 4s -- the first advance establishes the grid.
    REQUIRE(rows.size() == 3);
    CHECK(eventbook::epoch_micros(rows[0].sample_time) == 2'000'000);
    CHECK(eventbook::epoch_micros(rows[1].sample_time) == 3'000'000);
    CHECK(eventbook::epoch_micros(rows[2].sample_time) == 4'000'000);
}

TEST_CASE("boundaries align to the epoch, not to the first event") {
    // Two journals of the same market must land on the same grid so they can be
    // compared or concatenated without resampling.
    const auto state = state_with({{Price{4800}, Quantity{100}}}, {{Price{5200}, Quantity{100}}});

    FeatureSampler early{kTicker};
    FeatureSampler late{kTicker};
    std::vector<FeatureRow> from_early;
    std::vector<FeatureRow> from_late;

    // The first advance only establishes the grid; the second crosses it.
    early.advance(state, local_time_from_epoch_micros(10'100'000),
                  [&](const FeatureRow& r) { from_early.push_back(r); });
    early.advance(state, local_time_from_epoch_micros(11'500'000),
                  [&](const FeatureRow& r) { from_early.push_back(r); });
    late.advance(state, local_time_from_epoch_micros(10'900'000),
                 [&](const FeatureRow& r) { from_late.push_back(r); });
    late.advance(state, local_time_from_epoch_micros(11'500'000),
                 [&](const FeatureRow& r) { from_late.push_back(r); });

    REQUIRE_FALSE(from_early.empty());
    REQUIRE_FALSE(from_late.empty());
    CHECK(eventbook::epoch_micros(from_early.front().sample_time) == 11'000'000);
    CHECK(eventbook::epoch_micros(from_late.front().sample_time) == 11'000'000);
}

TEST_CASE("a quiet market still produces rows") {
    // A gap in a time series and a market that did not move are different
    // facts, and only one of them is true here.
    const auto state = state_with({{Price{4800}, Quantity{100}}}, {{Price{5200}, Quantity{100}}});
    FeatureSampler sampler{kTicker};

    std::size_t count = 0;
    const auto sink = [&](const FeatureRow&) { ++count; };
    sampler.advance(state, local_time_from_epoch_micros(1'000'000), sink);
    count = 0;
    sampler.advance(state, local_time_from_epoch_micros(62'000'000), sink);

    // Boundaries at 2s through 62s inclusive.
    CHECK(count == 61);
}

TEST_CASE("rows over an invalid interval are emitted and counted") {
    MarketState state{kTicker, kCentGrid};  // never snapshotted
    FeatureSampler sampler{kTicker};

    std::vector<FeatureRow> rows;
    sampler.advance(state, local_time_from_epoch_micros(1'000'000),
                    [&](const FeatureRow& r) { rows.push_back(r); });
    sampler.advance(state, local_time_from_epoch_micros(5'000'000),
                    [&](const FeatureRow& r) { rows.push_back(r); });

    REQUIRE(rows.size() == 4);
    for (const auto& row : rows) {
        CHECK_FALSE(row.book_valid);
    }
    CHECK(sampler.invalid_rows() == 4);
    CHECK(sampler.rows_emitted() == 4);
}

TEST_CASE("a row carries the provenance of the event that produced it") {
    // Raw and derived records must be traceable to each other.
    const auto state = state_with({{Price{4800}, Quantity{100}}}, {{Price{5200}, Quantity{100}}});
    FeatureSampler sampler{kTicker};
    sampler.observe(eventbook::exchange_time_from_epoch_micros(999'000), SequenceNumber{4171});

    std::vector<FeatureRow> rows;
    sampler.advance(state, local_time_from_epoch_micros(1'000'000),
                    [&](const FeatureRow& r) { rows.push_back(r); });
    sampler.advance(state, local_time_from_epoch_micros(2'000'000),
                    [&](const FeatureRow& r) { rows.push_back(r); });

    REQUIRE_FALSE(rows.empty());
    REQUIRE(rows.back().last_sequence.has_value());
    CHECK(rows.back().last_sequence->value == 4171);
    REQUIRE(rows.back().last_exchange_time.has_value());
    CHECK(eventbook::epoch_micros(*rows.back().last_exchange_time) == 999'000);
}

TEST_CASE("CSV writes empty fields for missing values, never zero") {
    MarketState state{kTicker, kCentGrid};
    const auto row = features_of(state);
    const auto line = eventbook::to_csv(row);

    // book_valid=0 then a run of empty fields, not a run of zeros.
    CHECK(line.find(",0,,,,") != std::string::npos);
    CHECK(eventbook::feature_row_header({}).find("microprice_displacement") != std::string::npos);
}

// --- rolling windows ---

namespace {

using eventbook::BookDelta;
using eventbook::PublicTrade;
using eventbook::QuantityDelta;
using eventbook::RollingFeatureState;
using eventbook::TradeSide;
using eventbook::WindowFeatures;

eventbook::MarketEvent trade_event(TradeSide side, std::int64_t units) {
    PublicTrade trade;
    trade.market_ticker = kTicker;
    trade.yes_price = Price{5000};
    trade.quantity = Quantity{units};
    trade.taker_side = side;
    return eventbook::MarketEvent{trade};
}

eventbook::MarketEvent delta_event(std::int64_t change) {
    BookDelta delta;
    delta.market_ticker = kTicker;
    delta.side = eventbook::BookSide::Bid;
    delta.price = Price{4800};
    delta.delta = QuantityDelta{change};
    return eventbook::MarketEvent{delta};
}

}  // namespace

TEST_CASE("trade flow is signed by taker aggression") {
    RollingFeatureState rolling{{std::chrono::seconds{60}}};
    rolling.observe(trade_event(TradeSide::BuyYes, 300), local_time_from_epoch_micros(1'000'000));
    rolling.observe(trade_event(TradeSide::SellYes, 100), local_time_from_epoch_micros(2'000'000));

    const auto summary = rolling.summarize(local_time_from_epoch_micros(3'000'000));
    REQUIRE(summary.size() == 1);
    CHECK(summary[0].trades == 2);
    CHECK(summary[0].trade_volume == Quantity{400});
    CHECK(summary[0].signed_trade_volume == 200);
    REQUIRE(summary[0].trade_flow_imbalance.has_value());
    CHECK_THAT(*summary[0].trade_flow_imbalance, WithinAbs(0.5, 1e-9));
}

TEST_CASE("trade flow imbalance is missing when nothing traded") {
    // Most seconds, given roughly 1,500 book updates per trade. A zero would
    // claim balanced flow where there was no flow at all.
    RollingFeatureState rolling{{std::chrono::seconds{60}}};
    const auto summary = rolling.summarize(local_time_from_epoch_micros(3'000'000));
    CHECK(summary[0].trades == 0);
    CHECK_FALSE(summary[0].trade_flow_imbalance.has_value());
}

TEST_CASE("windows are trailing, so old activity drops out") {
    RollingFeatureState rolling{{std::chrono::seconds{10}, std::chrono::seconds{60}}};
    rolling.observe(trade_event(TradeSide::BuyYes, 100), local_time_from_epoch_micros(1'000'000));
    rolling.observe(trade_event(TradeSide::BuyYes, 200), local_time_from_epoch_micros(55'000'000));

    const auto summary = rolling.summarize(local_time_from_epoch_micros(60'000'000));
    REQUIRE(summary.size() == 2);
    CHECK(summary[0].window == std::chrono::seconds{10});
    CHECK(summary[0].trades == 1);  // only the trade at 55s
    CHECK(summary[1].window == std::chrono::seconds{60});
    CHECK(summary[1].trades == 2);  // both
}

TEST_CASE("book churn is split into adds and removes") {
    // Quote churn dominates the stream: we measured roughly 1,500 book updates
    // per trade, so this is where most of the information lives.
    RollingFeatureState rolling{{std::chrono::seconds{60}}};
    rolling.observe(delta_event(500), local_time_from_epoch_micros(1'000'000));
    rolling.observe(delta_event(-200), local_time_from_epoch_micros(2'000'000));
    rolling.observe(delta_event(-300), local_time_from_epoch_micros(3'000'000));
    rolling.observe(delta_event(0), local_time_from_epoch_micros(4'000'000));  // no-op

    const auto summary = rolling.summarize(local_time_from_epoch_micros(5'000'000));
    CHECK(summary[0].book_adds == 1);
    CHECK(summary[0].book_removes == 2);
}

TEST_CASE("realized volatility sums squared midpoint changes") {
    RollingFeatureState rolling{{std::chrono::seconds{60}}};
    // Midpoints 5000, 5100, 5000 -> changes of +100 and -100.
    rolling.observe_sample(local_time_from_epoch_micros(1'000'000), 5000.0);
    rolling.observe_sample(local_time_from_epoch_micros(2'000'000), 5100.0);
    rolling.observe_sample(local_time_from_epoch_micros(3'000'000), 5000.0);

    const auto summary = rolling.summarize(local_time_from_epoch_micros(3'500'000));
    REQUIRE(summary[0].realized_volatility.has_value());
    // sqrt(100^2 + 100^2)
    CHECK_THAT(*summary[0].realized_volatility, WithinAbs(std::sqrt(20000.0), 1e-6));
}

TEST_CASE("a motionless market has zero realized volatility, not missing") {
    RollingFeatureState rolling{{std::chrono::seconds{60}}};
    rolling.observe_sample(local_time_from_epoch_micros(1'000'000), 5000.0);
    rolling.observe_sample(local_time_from_epoch_micros(2'000'000), 5000.0);

    const auto summary = rolling.summarize(local_time_from_epoch_micros(2'500'000));
    REQUIRE(summary[0].realized_volatility.has_value());
    CHECK_THAT(*summary[0].realized_volatility, WithinAbs(0.0, 1e-9));
}

TEST_CASE("volatility needs two observations before it means anything") {
    RollingFeatureState rolling{{std::chrono::seconds{60}}};
    rolling.observe_sample(local_time_from_epoch_micros(1'000'000), 5000.0);
    const auto summary = rolling.summarize(local_time_from_epoch_micros(1'500'000));
    CHECK_FALSE(summary[0].realized_volatility.has_value());
}

TEST_CASE("an invalid interval does not create a spurious volatility jump") {
    // A book that was invalid for a while and came back at a different price
    // must not contribute one enormous change spanning the outage.
    RollingFeatureState rolling{{std::chrono::seconds{60}}};
    rolling.observe_sample(local_time_from_epoch_micros(1'000'000), 5000.0);
    rolling.observe_sample(local_time_from_epoch_micros(2'000'000), std::nullopt);
    rolling.observe_sample(local_time_from_epoch_micros(3'000'000), std::nullopt);
    rolling.observe_sample(local_time_from_epoch_micros(4'000'000), 9000.0);
    rolling.observe_sample(local_time_from_epoch_micros(5'000'000), 9100.0);

    const auto summary = rolling.summarize(local_time_from_epoch_micros(5'500'000));
    REQUIRE(summary[0].realized_volatility.has_value());
    // Only the 9000 -> 9100 change counts; the 5000 -> 9000 jump across the
    // hole does not.
    CHECK_THAT(*summary[0].realized_volatility, WithinAbs(100.0, 1e-6));
}

TEST_CASE("expiry bounds memory without losing an in-window change") {
    RollingFeatureState rolling{{std::chrono::seconds{10}}};
    for (std::int64_t second = 1; second <= 100; ++second) {
        const auto at = local_time_from_epoch_micros(second * 1'000'000);
        rolling.observe(trade_event(TradeSide::BuyYes, 10), at);
        rolling.observe_sample(at, 5000.0 + static_cast<double>(second));
        rolling.expire(at);
    }

    const auto summary = rolling.summarize(local_time_from_epoch_micros(100'000'000));
    // The window is half-open: samples strictly after the cutoff, so seconds
    // 91 through 100 -- ten samples yielding nine one-unit changes.
    REQUIRE(summary[0].realized_volatility.has_value());
    CHECK_THAT(*summary[0].realized_volatility, WithinAbs(3.0, 1e-6));
    CHECK(summary[0].trades == 10);
}

TEST_CASE("the sampler attaches window summaries to each row") {
    const auto state = state_with({{Price{4800}, Quantity{100}}}, {{Price{5200}, Quantity{100}}});
    FeatureSampler sampler{kTicker, std::chrono::seconds{1}, {std::chrono::seconds{10}}};

    sampler.advance(state, local_time_from_epoch_micros(1'000'000), [](const FeatureRow&) {});
    sampler.observe_event(trade_event(TradeSide::BuyYes, 250),
                          local_time_from_epoch_micros(1'500'000));

    std::vector<FeatureRow> rows;
    sampler.advance(state, local_time_from_epoch_micros(2'000'000),
                    [&](const FeatureRow& r) { rows.push_back(r); });

    REQUIRE(rows.size() == 1);
    REQUIRE(rows[0].windows.size() == 1);
    CHECK(rows[0].windows[0].trades == 1);
    CHECK(rows[0].windows[0].signed_trade_volume == 250);
}
