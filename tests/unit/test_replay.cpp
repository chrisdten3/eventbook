#include "eventbook/replay/replay.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>
#include <vector>

using eventbook::decode_session_metadata;
using eventbook::encode_session_metadata;
using eventbook::JournalRecord;
using eventbook::JournalRecordKind;
using eventbook::JournalWriter;
using eventbook::JournalWriterConfig;
using eventbook::local_time_from_epoch_micros;
using eventbook::MarketState;
using eventbook::MarketTicker;
using eventbook::parse_ws_message;
using eventbook::Price;
using eventbook::PriceConvention;
using eventbook::PriceDelta;
using eventbook::PriceRange;
using eventbook::ReplayErrorKind;
using eventbook::ReplayOptions;
using eventbook::SessionMetadata;

namespace {

const MarketTicker kTicker{"TEST-1"};
const std::vector<PriceRange> kCentGrid{PriceRange{Price{0}, Price{10000}, PriceDelta{100}}};

SessionMetadata metadata() {
    SessionMetadata meta;
    meta.market_ticker = kTicker;
    meta.price_convention = PriceConvention::YesLegPricing;
    meta.price_grid = kCentGrid;
    meta.build = "eventbook test";
    return meta;
}

std::filesystem::path scratch_dir(const std::string& name) {
    const auto path = std::filesystem::temp_directory_path() / ("eventbook_replay_" + name);
    std::filesystem::remove_all(path);
    std::filesystem::create_directories(path);
    return path;
}

std::string snapshot_payload(std::uint64_t seq) {
    return R"({"type":"orderbook_snapshot","sid":1,"seq":)" + std::to_string(seq) +
           R"(,"msg":{"market_ticker":"TEST-1",)"
           R"("yes_dollars_fp":[["0.4800","10.00"],["0.4700","25.00"]],)"
           R"("no_dollars_fp":[["0.5200","20.00"],["0.5300","30.00"]]}})";
}

std::string delta_payload(std::uint64_t seq, const char* price, const char* delta,
                          const char* side) {
    return R"({"type":"orderbook_delta","sid":1,"seq":)" + std::to_string(seq) +
           R"(,"msg":{"market_ticker":"TEST-1","price_dollars":")" + price + R"(","delta_fp":")" +
           delta + R"(","side":")" + side + R"("}})";
}

/// The payload sequence used by every test below, so live and replay are
/// unambiguously being fed the same thing.
std::vector<std::string> scripted_payloads() {
    return {
        snapshot_payload(1),
        delta_payload(2, "0.4800", "5.00", "yes"),
        delta_payload(3, "0.5200", "-20.00", "no"),   // empties an ask level
        delta_payload(4, "0.4600", "40.00", "yes"),   // new bid level
        delta_payload(5, "0.4700", "-25.00", "yes"),  // empties a bid level
        R"({"type":"trade","sid":2,"msg":{"trade_id":"t1","market_ticker":"TEST-1",)"
        R"("yes_price_dollars":"0.4800","count_fp":"3.00","taker_side":"yes"}})",
    };
}

/// Write a journal containing the scripted payloads.
std::filesystem::path write_journal(const std::string& name, bool compress = true,
                                    std::uint64_t rotate_bytes = 0) {
    const auto dir = scratch_dir(name);
    JournalWriterConfig config;
    config.compress = compress;
    config.rotate_bytes = rotate_bytes;
    config.rotate_interval = std::chrono::minutes{0};

    auto writer = JournalWriter::create(dir, "test", config);
    REQUIRE(writer.has_value());
    auto journal = *std::move(writer);

    JournalRecord session;
    session.kind = JournalRecordKind::SessionStarted;
    session.local_receive_time = local_time_from_epoch_micros(1'000'000);
    session.connection_id = 1;
    session.message_type = "session_started";
    session.payload = encode_session_metadata(metadata());
    REQUIRE_FALSE(journal.write(session).has_value());

    std::int64_t at = 2'000'000;
    for (const auto& payload : scripted_payloads()) {
        JournalRecord record;
        record.kind = JournalRecordKind::Message;
        record.local_receive_time = local_time_from_epoch_micros(at);
        record.connection_id = 1;
        record.message_type = "message";
        record.market_ticker = kTicker;
        record.payload = payload;
        REQUIRE_FALSE(journal.write(record).has_value());
        at += 1'000'000;
    }
    REQUIRE_FALSE(journal.flush().has_value());
    return dir;
}

}  // namespace

TEST_CASE("session metadata round-trips") {
    const auto decoded = decode_session_metadata(encode_session_metadata(metadata()));
    REQUIRE(decoded.has_value());
    CHECK(decoded->market_ticker == kTicker);
    CHECK(decoded->price_convention == PriceConvention::YesLegPricing);
    REQUIRE(decoded->price_grid.size() == 1);
    CHECK(decoded->price_grid[0].step == PriceDelta{100});
    CHECK(decoded->build == "eventbook test");
}

TEST_CASE("the price convention travels with the journal") {
    // It is a subscription-time decision the messages never restate, so a
    // journal that did not carry it could only be replayed by guessing -- and a
    // wrong guess mirrors every ask instead of failing.
    SessionMetadata meta = metadata();
    meta.price_convention = PriceConvention::NoLegPricing;
    const auto decoded = decode_session_metadata(encode_session_metadata(meta));
    REQUIRE(decoded.has_value());
    CHECK(decoded->price_convention == PriceConvention::NoLegPricing);
}

TEST_CASE("replaying a journal rebuilds the book") {
    const auto dir = write_journal("basic");
    const auto result = eventbook::replay_directory(dir);
    REQUIRE(result.has_value());

    CHECK(result->segments == 1);
    CHECK(result->messages == 6);
    CHECK(result->parse_failures == 0);
    CHECK(result->market.snapshots == 1);
    CHECK(result->market.deltas == 4);
    CHECK(result->market.trades == 1);
    CHECK(result->market.rejected_deltas == 0);
    CHECK(result->market.sequence_gaps == 0);
    CHECK(result->metadata.market_ticker == kTicker);
    std::filesystem::remove_all(dir);
}

TEST_CASE("replaying the same journal twice yields the same state hash and metrics") {
    // The guarantee the whole milestone exists to provide. Without it, a change
    // in results can never be attributed to a change in code.
    const auto dir = write_journal("determinism");

    const auto first = eventbook::replay_directory(dir);
    const auto second = eventbook::replay_directory(dir);
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());

    CHECK(first->final_state_hash == second->final_state_hash);
    CHECK(first->records == second->records);
    CHECK(first->messages == second->messages);
    CHECK(first->market.deltas == second->market.deltas);
    CHECK(first->market.rejected_deltas == second->market.rejected_deltas);
    CHECK(first->market.invalid_time == second->market.invalid_time);
    CHECK(first->market.crossed_observations == second->market.crossed_observations);
    std::filesystem::remove_all(dir);
}

TEST_CASE("replay reproduces live processing exactly, not approximately") {
    // Live and replay must share one implementation of "what does this event do
    // to the book". This drives the same payloads through MarketState directly,
    // the way collect does, and compares against what replay produces from the
    // journal. Equal hashes mean the two paths cannot have diverged.
    MarketState live{kTicker, kCentGrid};
    std::int64_t at = 2'000'000;
    for (const auto& payload : scripted_payloads()) {
        const auto event = parse_ws_message(payload, PriceConvention::YesLegPricing);
        REQUIRE(event.has_value());
        (void)live.apply(*event, local_time_from_epoch_micros(at));
        at += 1'000'000;
    }
    live.finish(local_time_from_epoch_micros(at - 1'000'000));

    const auto dir = write_journal("live_vs_replay");
    const auto replayed = eventbook::replay_directory(dir);
    REQUIRE(replayed.has_value());

    CHECK(replayed->final_state_hash == live.state_hash());
    CHECK(replayed->market.deltas == live.stats().deltas);
    CHECK(replayed->market.trades == live.stats().trades);
    CHECK(replayed->market.invalid_time == live.stats().invalid_time);
    std::filesystem::remove_all(dir);
}

TEST_CASE("replay spans segment boundaries") {
    // Rotation must not change the answer: a journal split across files has to
    // replay identically to one that is not.
    const auto split = write_journal("split", true, /*rotate_bytes=*/1);
    const auto whole = write_journal("whole", true, /*rotate_bytes=*/0);

    const auto from_split = eventbook::replay_directory(split);
    const auto from_whole = eventbook::replay_directory(whole);
    REQUIRE(from_split.has_value());
    REQUIRE(from_whole.has_value());

    CHECK(from_split->segments > from_whole->segments);
    CHECK(from_split->final_state_hash == from_whole->final_state_hash);
    CHECK(from_split->messages == from_whole->messages);
    std::filesystem::remove_all(split);
    std::filesystem::remove_all(whole);
}

TEST_CASE("compression does not change the replayed result") {
    const auto compressed = write_journal("compressed", true);
    const auto plain = write_journal("plain", false);

    const auto a = eventbook::replay_directory(compressed);
    const auto b = eventbook::replay_directory(plain);
    REQUIRE(a.has_value());
    REQUIRE(b.has_value());
    CHECK(a->final_state_hash == b->final_state_hash);
    std::filesystem::remove_all(compressed);
    std::filesystem::remove_all(plain);
}

TEST_CASE("a disconnection invalidates the book across the gap") {
    const auto dir = scratch_dir("disconnect");
    {
        JournalWriterConfig config;
        config.rotate_interval = std::chrono::minutes{0};
        auto writer = JournalWriter::create(dir, "test", config);
        REQUIRE(writer.has_value());
        auto journal = *std::move(writer);

        JournalRecord session;
        session.kind = JournalRecordKind::SessionStarted;
        session.local_receive_time = local_time_from_epoch_micros(1'000'000);
        session.payload = encode_session_metadata(metadata());
        REQUIRE_FALSE(journal.write(session).has_value());

        JournalRecord snapshot;
        snapshot.kind = JournalRecordKind::Message;
        snapshot.local_receive_time = local_time_from_epoch_micros(2'000'000);
        snapshot.payload = snapshot_payload(1);
        REQUIRE_FALSE(journal.write(snapshot).has_value());

        JournalRecord lost;
        lost.kind = JournalRecordKind::ConnectionLost;
        lost.local_receive_time = local_time_from_epoch_micros(3'000'000);
        lost.message_type = "read timeout";
        REQUIRE_FALSE(journal.write(lost).has_value());

        // Five seconds pass before a fresh snapshot arrives.
        JournalRecord recovered;
        recovered.kind = JournalRecordKind::Message;
        recovered.local_receive_time = local_time_from_epoch_micros(8'000'000);
        recovered.payload = snapshot_payload(1);
        REQUIRE_FALSE(journal.write(recovered).has_value());
        REQUIRE_FALSE(journal.flush().has_value());
    }

    const auto result = eventbook::replay_directory(dir);
    REQUIRE(result.has_value());
    CHECK(result->market.disconnections == 1);
    CHECK(result->market.snapshots == 2);
    // Exactly the five seconds between the drop and the fresh snapshot.
    CHECK(result->market.invalid_time == std::chrono::seconds{5});
    std::filesystem::remove_all(dir);
}

TEST_CASE("an unparseable payload is counted, not fatal") {
    const auto dir = scratch_dir("badpayload");
    {
        JournalWriterConfig config;
        config.rotate_interval = std::chrono::minutes{0};
        auto writer = JournalWriter::create(dir, "test", config);
        REQUIRE(writer.has_value());
        auto journal = *std::move(writer);

        JournalRecord session;
        session.kind = JournalRecordKind::SessionStarted;
        session.local_receive_time = local_time_from_epoch_micros(1'000'000);
        session.payload = encode_session_metadata(metadata());
        REQUIRE_FALSE(journal.write(session).has_value());

        JournalRecord broken;
        broken.kind = JournalRecordKind::Message;
        broken.local_receive_time = local_time_from_epoch_micros(2'000'000);
        broken.payload = "{not a message";
        REQUIRE_FALSE(journal.write(broken).has_value());
        REQUIRE_FALSE(journal.flush().has_value());
    }

    const auto tolerant = eventbook::replay_directory(dir);
    REQUIRE(tolerant.has_value());
    CHECK(tolerant->parse_failures == 1);

    // Opt in to strictness when a clean journal is required.
    ReplayOptions strict;
    strict.stop_on_parse_failure = true;
    const auto refused = eventbook::replay_directory(dir, {}, strict);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().kind == ReplayErrorKind::ParseFailure);
    std::filesystem::remove_all(dir);
}

TEST_CASE("a journal without a session record cannot be replayed") {
    // Guessing the price convention would silently mirror every ask, so refusing
    // is the only safe answer.
    const auto dir = scratch_dir("nosession");
    {
        JournalWriterConfig config;
        config.rotate_interval = std::chrono::minutes{0};
        auto writer = JournalWriter::create(dir, "test", config);
        REQUIRE(writer.has_value());
        auto journal = *std::move(writer);
        JournalRecord record;
        record.kind = JournalRecordKind::Message;
        record.local_receive_time = local_time_from_epoch_micros(1'000'000);
        record.payload = snapshot_payload(1);
        REQUIRE_FALSE(journal.write(record).has_value());
        REQUIRE_FALSE(journal.flush().has_value());
    }

    const auto result = eventbook::replay_directory(dir);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().kind == ReplayErrorKind::MissingSessionRecord);

    // Unless the caller supplies the metadata explicitly.
    ReplayOptions options;
    options.metadata_override = metadata();
    const auto forced = eventbook::replay_directory(dir, {}, options);
    REQUIRE(forced.has_value());
    CHECK(forced->market.snapshots == 1);
    std::filesystem::remove_all(dir);
}

TEST_CASE("replaying nothing is an error rather than an empty success") {
    const auto dir = scratch_dir("empty");
    const auto result = eventbook::replay_directory(dir);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().kind == ReplayErrorKind::NoSegments);
    std::filesystem::remove_all(dir);
}
