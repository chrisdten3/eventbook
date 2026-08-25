#include "eventbook/book/order_book.hpp"

#include <catch2/catch_test_macros.hpp>

#include <variant>
#include <vector>

#include "eventbook/api/ws_protocol.hpp"
#include "fixtures.hpp"

using eventbook::BookDelta;
using eventbook::BookLevel;
using eventbook::BookRejection;
using eventbook::BookSide;
using eventbook::BookSnapshot;
using eventbook::BookStatus;
using eventbook::invalidates_book;
using eventbook::MarketTicker;
using eventbook::OrderBook;
using eventbook::parse_ws_message;
using eventbook::Price;
using eventbook::PriceConvention;
using eventbook::PriceDelta;
using eventbook::PriceRange;
using eventbook::Quantity;
using eventbook::QuantityDelta;
using eventbook::SequenceNumber;
using eventbook::SubscriptionId;
using eventbook::testing::read_fixture;

namespace {

// One cent per tick over the full settlement range, as KXFED publishes.
const std::vector<PriceRange> kCentGrid{PriceRange{Price{0}, Price{10000}, PriceDelta{100}}};

const MarketTicker kTicker{"TEST-1"};

OrderBook make_book(std::vector<PriceRange> grid = kCentGrid) {
    return OrderBook{kTicker, std::move(grid)};
}

BookSnapshot snapshot(std::uint64_t sequence, std::vector<BookLevel> bids,
                      std::vector<BookLevel> asks) {
    BookSnapshot event;
    event.market_ticker = kTicker;
    event.subscription = SubscriptionId{1};
    event.sequence = SequenceNumber{sequence};
    event.bids = std::move(bids);
    event.asks = std::move(asks);
    return event;
}

BookDelta delta(std::uint64_t sequence, BookSide side, Price price, std::int64_t change) {
    BookDelta event;
    event.market_ticker = kTicker;
    event.subscription = SubscriptionId{1};
    event.sequence = SequenceNumber{sequence};
    event.side = side;
    event.price = price;
    event.delta = QuantityDelta{change};
    return event;
}

}  // namespace

TEST_CASE("a book is invalid before its first snapshot") {
    auto book = make_book();
    CHECK(book.status() == BookStatus::AwaitingSnapshot);
    CHECK_FALSE(book.is_valid());
    CHECK_FALSE(book.best_bid().has_value());
    CHECK_FALSE(book.best_ask().has_value());
    CHECK(book.depth(BookSide::Bid, 5) == Quantity{});

    // A delta with nothing to apply it to is refused, and refusing it does not
    // make the book any more broken than it already is.
    const auto rejection = book.apply(delta(1, BookSide::Bid, Price{5000}, 100));
    REQUIRE(rejection.has_value());
    CHECK(*rejection == BookRejection::DeltaBeforeSnapshot);
    CHECK_FALSE(invalidates_book(*rejection));
}

TEST_CASE("a snapshot establishes the book") {
    auto book = make_book();
    REQUIRE_FALSE(
        book.apply(snapshot(1, {{Price{4800}, Quantity{1000}}, {Price{4700}, Quantity{500}}},
                            {{Price{5200}, Quantity{700}}, {Price{5300}, Quantity{900}}}))
            .has_value());

    CHECK(book.is_valid());
    CHECK(book.best_bid() == Price{4800});
    CHECK(book.best_ask() == Price{5200});
    CHECK(book.spread() == PriceDelta{400});
    CHECK(book.level_count(BookSide::Bid) == 2);
    CHECK(book.last_sequence() == SequenceNumber{1});
    CHECK_FALSE(book.is_crossed());
}

TEST_CASE("snapshots drop zero-quantity levels on the way in") {
    // The invariant that no level is ever zero holds from the first message
    // rather than being restored afterwards.
    auto book = make_book();
    REQUIRE_FALSE(
        book.apply(snapshot(1, {{Price{4800}, Quantity{1000}}, {Price{4700}, Quantity{0}}}, {}))
            .has_value());
    CHECK(book.level_count(BookSide::Bid) == 1);
    CHECK(book.best_bid() == Price{4800});
}

TEST_CASE("deltas add, reduce, and remove levels") {
    auto book = make_book();
    REQUIRE_FALSE(book.apply(snapshot(1, {{Price{4800}, Quantity{1000}}}, {})).has_value());

    SECTION("a positive delta grows a level") {
        REQUIRE_FALSE(book.apply(delta(2, BookSide::Bid, Price{4800}, 500)).has_value());
        CHECK(book.depth(BookSide::Bid, 1) == Quantity{1500});
    }

    SECTION("a positive delta at a new price creates a level") {
        REQUIRE_FALSE(book.apply(delta(2, BookSide::Bid, Price{4900}, 200)).has_value());
        CHECK(book.best_bid() == Price{4900});
        CHECK(book.level_count(BookSide::Bid) == 2);
    }

    SECTION("a negative delta shrinks a level") {
        REQUIRE_FALSE(book.apply(delta(2, BookSide::Bid, Price{4800}, -400)).has_value());
        CHECK(book.depth(BookSide::Bid, 1) == Quantity{600});
    }

    SECTION("a delta to exactly zero removes the level") {
        REQUIRE_FALSE(book.apply(delta(2, BookSide::Bid, Price{4800}, -1000)).has_value());
        CHECK(book.level_count(BookSide::Bid) == 0);
        CHECK_FALSE(book.best_bid().has_value());
        CHECK(book.is_valid());  // an empty side is ordinary, not an error
    }
}

TEST_CASE("a delta removing more size than exists invalidates the book") {
    // This is where apply_delta returning a Result finally has a caller. The
    // arithmetic says the transition is impossible; the book decides that means
    // our state disagrees with the venue's, so it must be rebuilt.
    auto book = make_book();
    REQUIRE_FALSE(book.apply(snapshot(1, {{Price{4800}, Quantity{1000}}}, {})).has_value());

    const auto rejection = book.apply(delta(2, BookSide::Bid, Price{4800}, -1001));
    REQUIRE(rejection.has_value());
    CHECK(*rejection == BookRejection::QuantityWouldGoNegative);
    CHECK(invalidates_book(*rejection));
    CHECK_FALSE(book.is_valid());
    CHECK_FALSE(book.best_bid().has_value());
}

TEST_CASE("a sequence gap invalidates the book") {
    auto book = make_book();
    REQUIRE_FALSE(book.apply(snapshot(1, {{Price{4800}, Quantity{1000}}}, {})).has_value());

    // seq 2 was missed.
    const auto rejection = book.apply(delta(3, BookSide::Bid, Price{4800}, 100));
    REQUIRE(rejection.has_value());
    CHECK(*rejection == BookRejection::SequenceGap);
    CHECK_FALSE(book.is_valid());
}

TEST_CASE("a repeated or out-of-order sequence invalidates the book") {
    auto book = make_book();
    REQUIRE_FALSE(book.apply(snapshot(5, {{Price{4800}, Quantity{1000}}}, {})).has_value());
    REQUIRE_FALSE(book.apply(delta(6, BookSide::Bid, Price{4800}, 100)).has_value());

    // Applying seq 6 again would double-count it. The venue documents seq as
    // incrementing by one, so anything else means we cannot reason about order.
    const auto rejection = book.apply(delta(6, BookSide::Bid, Price{4800}, 100));
    REQUIRE(rejection.has_value());
    CHECK(*rejection == BookRejection::SequenceRegressed);
    CHECK_FALSE(book.is_valid());
}

TEST_CASE("a fresh snapshot is the only way back from an invalid book") {
    auto book = make_book();
    REQUIRE_FALSE(book.apply(snapshot(1, {{Price{4800}, Quantity{1000}}}, {})).has_value());
    REQUIRE(book.apply(delta(9, BookSide::Bid, Price{4800}, 100)).has_value());
    REQUIRE_FALSE(book.is_valid());

    // Deltas stay refused however many arrive.
    CHECK(book.apply(delta(10, BookSide::Bid, Price{4800}, 100)) ==
          BookRejection::DeltaBeforeSnapshot);

    // The snapshot does not check continuity with what came before -- recovery
    // from a gap means accepting a discontinuity by definition.
    REQUIRE_FALSE(book.apply(snapshot(40, {{Price{4900}, Quantity{50}}}, {})).has_value());
    CHECK(book.is_valid());
    CHECK(book.best_bid() == Price{4900});
    CHECK(book.last_sequence() == SequenceNumber{40});
}

TEST_CASE("prices off the market's grid are refused") {
    auto book = make_book();
    REQUIRE_FALSE(book.apply(snapshot(1, {{Price{4800}, Quantity{1000}}}, {})).has_value());

    // $0.4850 is inside the range but not on a one-cent tick.
    const auto rejection = book.apply(delta(2, BookSide::Bid, Price{4850}, 100));
    REQUIRE(rejection.has_value());
    CHECK(*rejection == BookRejection::PriceOffGrid);
    CHECK_FALSE(book.is_valid());
}

TEST_CASE("prices outside settlement bounds are refused") {
    auto book = make_book({});  // no grid, so only bounds apply
    REQUIRE_FALSE(book.apply(snapshot(1, {{Price{4800}, Quantity{1000}}}, {})).has_value());

    const auto rejection = book.apply(delta(2, BookSide::Bid, Price{10100}, 100));
    REQUIRE(rejection.has_value());
    CHECK(*rejection == BookRejection::PriceOutOfBounds);
}

TEST_CASE("a message for another market changes nothing") {
    auto book = make_book();
    REQUIRE_FALSE(book.apply(snapshot(1, {{Price{4800}, Quantity{1000}}}, {})).has_value());

    BookDelta stray = delta(2, BookSide::Bid, Price{4800}, 500);
    stray.market_ticker = MarketTicker{"OTHER-1"};

    const auto rejection = book.apply(stray);
    REQUIRE(rejection.has_value());
    CHECK(*rejection == BookRejection::WrongMarket);
    CHECK_FALSE(invalidates_book(*rejection));
    // Still valid, still untouched.
    CHECK(book.is_valid());
    CHECK(book.depth(BookSide::Bid, 1) == Quantity{1000});
}

TEST_CASE("a rejected snapshot does not leave a half-applied book") {
    auto book = make_book();
    REQUIRE_FALSE(book.apply(snapshot(1, {{Price{4800}, Quantity{1000}}}, {})).has_value());

    // Second level is off-grid; a book containing only the first would be a
    // state that never existed on the venue.
    const auto rejection =
        book.apply(snapshot(2, {{Price{4700}, Quantity{10}}, {Price{4655}, Quantity{20}}}, {}));
    REQUIRE(rejection.has_value());
    CHECK(*rejection == BookRejection::PriceOffGrid);
    CHECK_FALSE(book.is_valid());
    CHECK(book.level_count(BookSide::Bid) == 0);
}

TEST_CASE("depth sums the best levels of a side") {
    auto book = make_book();
    REQUIRE_FALSE(book.apply(snapshot(1,
                                      {{Price{4800}, Quantity{100}},
                                       {Price{4700}, Quantity{200}},
                                       {Price{4600}, Quantity{300}}},
                                      {{Price{5200}, Quantity{50}}, {Price{5300}, Quantity{70}}}))
                      .has_value());

    CHECK(book.depth(BookSide::Bid, 1) == Quantity{100});
    CHECK(book.depth(BookSide::Bid, 2) == Quantity{300});
    CHECK(book.depth(BookSide::Bid, 3) == Quantity{600});
    // Asking for more levels than exist is not an error.
    CHECK(book.depth(BookSide::Bid, 10) == Quantity{600});
    CHECK(book.depth(BookSide::Ask, 5) == Quantity{120});
}

TEST_CASE("a crossed book is reported, not rejected") {
    // AGENTS.md warns against discarding crossed or transitioning states as
    // impossible before checking them against lifecycle messages. Treating this
    // as fatal would destroy the evidence needed to understand it.
    auto book = make_book();
    REQUIRE_FALSE(
        book.apply(snapshot(1, {{Price{5200}, Quantity{10}}}, {{Price{5000}, Quantity{10}}}))
            .has_value());

    CHECK(book.is_valid());
    CHECK(book.is_crossed());
}

TEST_CASE("quantities never go negative under any delta sequence") {
    // The invariant AGENTS.md states, exercised across the boundary where it is
    // most likely to break.
    auto book = make_book();
    REQUIRE_FALSE(book.apply(snapshot(1, {{Price{5000}, Quantity{500}}}, {})).has_value());

    std::uint64_t sequence = 2;
    for (const std::int64_t change : {100, -50, -400, 250, -300, 1000, -600}) {
        const auto rejection = book.apply(delta(sequence++, BookSide::Bid, Price{5000}, change));
        INFO("change=" << change);
        if (rejection.has_value()) {
            CHECK(*rejection == BookRejection::QuantityWouldGoNegative);
            break;
        }
        for (const auto& [price, quantity] : book.bids()) {
            CHECK(quantity.units > 0);
        }
    }
}

TEST_CASE("replaying one event log twice yields the same state hash") {
    // The determinism guarantee M3's replay engine depends on.
    const auto run = [] {
        auto book = make_book();
        (void)book.apply(snapshot(1, {{Price{4800}, Quantity{1000}}, {Price{4700}, Quantity{40}}},
                                  {{Price{5200}, Quantity{700}}}));
        (void)book.apply(delta(2, BookSide::Bid, Price{4800}, -200));
        (void)book.apply(delta(3, BookSide::Ask, Price{5300}, 90));
        (void)book.apply(delta(4, BookSide::Bid, Price{4700}, -40));
        return book.state_hash();
    };
    CHECK(run() == run());
}

TEST_CASE("the state hash separates states that differ") {
    auto left = make_book();
    auto right = make_book();
    (void)left.apply(snapshot(1, {{Price{4800}, Quantity{100}}}, {}));
    (void)right.apply(snapshot(1, {{Price{4800}, Quantity{101}}}, {}));
    CHECK(left.state_hash() != right.state_hash());

    // A level on the opposite side is a different book, even at the same price
    // and size -- which is why the digest carries a marker between the sides.
    auto bid_side = make_book();
    auto ask_side = make_book();
    (void)bid_side.apply(snapshot(1, {{Price{5000}, Quantity{10}}}, {}));
    (void)ask_side.apply(snapshot(1, {}, {{Price{5000}, Quantity{10}}}));
    CHECK(bid_side.state_hash() != ask_side.state_hash());

    // An invalidated book is not the same state as one that merely looks empty.
    auto invalidated = make_book();
    (void)invalidated.apply(snapshot(1, {}, {}));
    const auto valid_empty = invalidated.state_hash();
    invalidated.invalidate();
    CHECK(invalidated.state_hash() != valid_empty);
}

TEST_CASE("a real captured snapshot reconstructs the live top of book") {
    // End to end over the whole M2 chain: bytes off the socket, normalized onto
    // the YES scale, applied to the book. The result must match what
    // GET /markets independently reported for KXFED-27APR-T4.25.
    const auto event = parse_ws_message(read_fixture("ws_live_snapshot_noleg.json"),
                                        PriceConvention::NoLegPricing);
    REQUIRE(event.has_value());
    const auto* parsed = std::get_if<BookSnapshot>(&*event);
    REQUIRE(parsed != nullptr);

    OrderBook book{parsed->market_ticker, kCentGrid};
    REQUIRE_FALSE(book.apply(*parsed).has_value());

    CHECK(book.is_valid());
    CHECK(book.best_bid() == Price{1600});
    CHECK(book.best_ask() == Price{3500});
    CHECK(book.spread() == PriceDelta{1900});
    CHECK_FALSE(book.is_crossed());
    CHECK(book.level_count(BookSide::Bid) == 10);
    CHECK(book.level_count(BookSide::Ask) == 22);
}
