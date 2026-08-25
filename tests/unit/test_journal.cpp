#include "eventbook/data/journal.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using eventbook::decode_journal_record;
using eventbook::encode_journal_record;
using eventbook::exchange_time_from_epoch_micros;
using eventbook::JournalErrorKind;
using eventbook::JournalReader;
using eventbook::JournalRecord;
using eventbook::JournalRecordKind;
using eventbook::JournalWriter;
using eventbook::JournalWriterConfig;
using eventbook::kJournalVersion;
using eventbook::local_time_from_epoch_micros;
using eventbook::MarketTicker;
using eventbook::SequenceNumber;
using eventbook::SubscriptionId;

namespace {

/// A fresh, empty directory per test, so segments from one cannot leak into
/// another's listing.
std::filesystem::path scratch_dir(const std::string& name) {
    const auto path = std::filesystem::temp_directory_path() / ("eventbook_journal_" + name);
    std::filesystem::remove_all(path);
    std::filesystem::create_directories(path);
    return path;
}

/// Read every record across every segment in a directory, in order.
std::vector<JournalRecord> read_all(const std::filesystem::path& directory) {
    std::vector<JournalRecord> records;
    for (const auto& segment : eventbook::list_journal_segments(directory)) {
        auto reader = JournalReader::open(segment);
        REQUIRE(reader.has_value());
        auto stream = *std::move(reader);
        while (true) {
            auto record = stream.next();
            REQUIRE(record.has_value());
            if (!record->has_value()) {
                break;
            }
            records.push_back(**record);
        }
    }
    return records;
}

JournalRecord sample_message() {
    JournalRecord record;
    record.kind = JournalRecordKind::Message;
    record.local_receive_time = local_time_from_epoch_micros(1'787'630'734'965'123);
    record.exchange_time = exchange_time_from_epoch_micros(1'787'630'734'900'000);
    record.connection_id = 3;
    record.subscription = SubscriptionId{2};
    record.sequence = SequenceNumber{4171};
    record.message_type = "orderbook_delta";
    record.market_ticker = MarketTicker{"KXBTCD-26AUG2517-T79499.99"};
    record.market_id = "9b0f6b43-5b68-4f9f-9f02-9a2d1b8ac1a1";
    record.payload = R"({"type":"orderbook_delta","msg":{"delta_fp":"-54.00"}})";
    return record;
}

}  // namespace

TEST_CASE("a record round-trips through encode and decode") {
    const auto original = sample_message();
    const auto decoded = decode_journal_record(encode_journal_record(original));
    REQUIRE(decoded.has_value());

    CHECK(decoded->kind == original.kind);
    CHECK(decoded->connection_id == original.connection_id);
    CHECK(decoded->subscription == original.subscription);
    CHECK(decoded->sequence == original.sequence);
    CHECK(decoded->message_type == original.message_type);
    CHECK(decoded->market_ticker == original.market_ticker);
    CHECK(decoded->market_id == original.market_id);
    CHECK(eventbook::epoch_micros(decoded->local_receive_time) ==
          eventbook::epoch_micros(original.local_receive_time));
    REQUIRE(decoded->exchange_time.has_value());
    CHECK(eventbook::epoch_micros(*decoded->exchange_time) ==
          eventbook::epoch_micros(*original.exchange_time));
}

TEST_CASE("the raw payload survives byte for byte") {
    // The entire premise of the raw layer: a parser bug must be fixable by
    // replaying the journal, which is only true if the journal holds exactly
    // what arrived. Quotes, backslashes, and unicode all have to come back
    // unchanged.
    JournalRecord record = sample_message();
    record.payload = R"({"a":"quote \" and backslash \\ and unicode é","b":[1,2,3]})";

    const auto decoded = decode_journal_record(encode_journal_record(record));
    REQUIRE(decoded.has_value());
    CHECK(decoded->payload == record.payload);
}

TEST_CASE("every record carries the schema version") {
    // Per record rather than in a header, so a truncated or concatenated file
    // still has interpretable lines.
    const auto line = encode_journal_record(sample_message());
    CHECK(line.find("\"v\":" + std::to_string(kJournalVersion)) != std::string::npos);
}

TEST_CASE("absent optional fields are omitted, not written null") {
    JournalRecord record;
    record.kind = JournalRecordKind::ConnectionLost;
    record.local_receive_time = local_time_from_epoch_micros(1);
    record.connection_id = 1;
    record.message_type = "read timeout";

    const auto line = encode_journal_record(record);
    CHECK(line.find("seq") == std::string::npos);
    CHECK(line.find("ticker") == std::string::npos);
    CHECK(line.find("null") == std::string::npos);

    const auto decoded = decode_journal_record(line);
    REQUIRE(decoded.has_value());
    CHECK_FALSE(decoded->sequence.has_value());
    CHECK_FALSE(decoded->market_ticker.has_value());
    CHECK(decoded->kind == JournalRecordKind::ConnectionLost);
}

TEST_CASE("every record kind survives a round trip") {
    for (const auto kind : {JournalRecordKind::SessionStarted, JournalRecordKind::Message,
                            JournalRecordKind::ConnectionLost, JournalRecordKind::GapDetected,
                            JournalRecordKind::SnapshotApplied, JournalRecordKind::SessionEnded}) {
        JournalRecord record = sample_message();
        record.kind = kind;
        const auto decoded = decode_journal_record(encode_journal_record(record));
        INFO("kind=" << to_string(kind));
        REQUIRE(decoded.has_value());
        CHECK(decoded->kind == kind);
    }
}

TEST_CASE("malformed lines are reported rather than skipped") {
    CHECK(decode_journal_record("not json").error().kind == JournalErrorKind::MalformedRecord);
    CHECK(decode_journal_record("[]").error().kind == JournalErrorKind::MalformedRecord);

    const auto missing = decode_journal_record(R"({"v":1,"kind":"message"})");
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error().kind == JournalErrorKind::MissingField);

    const auto unknown_kind =
        decode_journal_record(R"({"v":1,"kind":"telepathy","t_local_us":1,"conn":1,)"
                              R"("type":"x","payload":"y"})");
    REQUIRE_FALSE(unknown_kind.has_value());
    CHECK(unknown_kind.error().kind == JournalErrorKind::MalformedRecord);
}

TEST_CASE("a future schema version is refused, not guessed at") {
    const auto future = decode_journal_record(R"({"v":99,"kind":"message","t_local_us":1,"conn":1,)"
                                              R"("type":"x","payload":"y"})");
    REQUIRE_FALSE(future.has_value());
    CHECK(future.error().kind == JournalErrorKind::UnsupportedVersion);
}

TEST_CASE("a written journal reads back in order") {
    const auto dir = scratch_dir("roundtrip");
    {
        JournalWriterConfig config;
        config.compress = false;
        auto writer = JournalWriter::create(dir, "test", config);
        REQUIRE(writer.has_value());
        auto journal = *std::move(writer);

        for (std::uint64_t i = 1; i <= 50; ++i) {
            JournalRecord record = sample_message();
            record.sequence = SequenceNumber{i};
            REQUIRE_FALSE(journal.write(record).has_value());
        }
        REQUIRE_FALSE(journal.flush().has_value());
        CHECK(journal.stats().records_written == 50);
        CHECK(journal.stats().write_failures == 0);
        CHECK(journal.stats().dropped_records == 0);
        CHECK(journal.stats().segments_created == 1);
    }

    const auto records = read_all(dir);
    REQUIRE(records.size() == 50);
    for (std::uint64_t i = 0; i < 50; ++i) {
        CHECK(records[i].sequence->value == i + 1);
    }
    std::filesystem::remove_all(dir);
}

TEST_CASE("compressed segments round-trip and are detected by content") {
    const auto dir = scratch_dir("compressed");
    {
        auto writer = JournalWriter::create(dir, "test");  // compression on by default
        REQUIRE(writer.has_value());
        auto journal = *std::move(writer);
        for (std::uint64_t i = 1; i <= 500; ++i) {
            JournalRecord record = sample_message();
            record.sequence = SequenceNumber{i};
            REQUIRE_FALSE(journal.write(record).has_value());
        }
        REQUIRE_FALSE(journal.flush().has_value());
        CHECK(journal.current_segment().extension() == ".zst");
    }

    const auto segments = eventbook::list_journal_segments(dir);
    REQUIRE(segments.size() == 1);

    auto reader = JournalReader::open(segments.front());
    REQUIRE(reader.has_value());
    CHECK(reader->is_compressed());

    const auto records = read_all(dir);
    REQUIRE(records.size() == 500);
    CHECK(records.front().sequence->value == 1);
    CHECK(records.back().sequence->value == 500);
    // The payload must survive compression byte for byte.
    CHECK(records.back().payload == sample_message().payload);
    std::filesystem::remove_all(dir);
}

TEST_CASE("compression actually shrinks the journal") {
    // Journal records are near-identical, which is exactly the shape zstd
    // exploits. The assertion is deliberately loose -- this pins that
    // compression is working at all, not a particular ratio.
    const auto plain_dir = scratch_dir("ratio_plain");
    const auto zstd_dir = scratch_dir("ratio_zstd");

    for (bool compress : {false, true}) {
        JournalWriterConfig config;
        config.compress = compress;
        auto writer = JournalWriter::create(compress ? zstd_dir : plain_dir, "test", config);
        REQUIRE(writer.has_value());
        auto journal = *std::move(writer);
        for (std::uint64_t i = 1; i <= 2000; ++i) {
            JournalRecord record = sample_message();
            record.sequence = SequenceNumber{i};
            REQUIRE_FALSE(journal.write(record).has_value());
        }
        REQUIRE_FALSE(journal.flush().has_value());
        if (compress) {
            INFO("uncompressed=" << journal.stats().uncompressed_bytes
                                 << " on_disk=" << journal.stats().bytes_on_disk);
            CHECK(journal.stats().bytes_on_disk * 4 < journal.stats().uncompressed_bytes);
        } else {
            CHECK(journal.stats().bytes_on_disk == journal.stats().uncompressed_bytes);
        }
    }
    std::filesystem::remove_all(plain_dir);
    std::filesystem::remove_all(zstd_dir);
}

TEST_CASE("segments rotate on size and stay individually readable") {
    const auto dir = scratch_dir("rotate_size");
    {
        JournalWriterConfig config;
        config.compress = false;
        config.flush_every = 10;
        config.rotate_bytes = 4096;  // a few dozen records
        auto writer = JournalWriter::create(dir, "test", config);
        REQUIRE(writer.has_value());
        auto journal = *std::move(writer);
        for (std::uint64_t i = 1; i <= 400; ++i) {
            JournalRecord record = sample_message();
            record.sequence = SequenceNumber{i};
            REQUIRE_FALSE(journal.write(record).has_value());
        }
        REQUIRE_FALSE(journal.flush().has_value());
        CHECK(journal.stats().segments_created > 1);
    }

    const auto segments = eventbook::list_journal_segments(dir);
    CHECK(segments.size() > 1);

    // Every record survives the boundaries, in order, with none duplicated or
    // lost -- which is the only thing rotation must not break.
    const auto records = read_all(dir);
    REQUIRE(records.size() == 400);
    for (std::uint64_t i = 0; i < 400; ++i) {
        CHECK(records[i].sequence->value == i + 1);
    }
    std::filesystem::remove_all(dir);
}

TEST_CASE("a forced rotation starts a new segment without losing records") {
    const auto dir = scratch_dir("rotate_forced");
    {
        JournalWriterConfig config;
        config.compress = true;
        auto writer = JournalWriter::create(dir, "test", config);
        REQUIRE(writer.has_value());
        auto journal = *std::move(writer);

        REQUIRE_FALSE(journal.write(sample_message()).has_value());
        const auto first = journal.current_segment();
        REQUIRE_FALSE(journal.rotate().has_value());
        CHECK(journal.current_segment() != first);
        REQUIRE_FALSE(journal.write(sample_message()).has_value());
        REQUIRE_FALSE(journal.flush().has_value());
        CHECK(journal.stats().segments_created == 2);
    }
    CHECK(eventbook::list_journal_segments(dir).size() == 2);
    CHECK(read_all(dir).size() == 2);
    std::filesystem::remove_all(dir);
}

TEST_CASE("segment names sort chronologically") {
    // The reader relies on lexicographic order matching time order, so it never
    // has to parse a filename.
    const auto dir = scratch_dir("ordering");
    {
        auto writer = JournalWriter::create(dir, "test");
        REQUIRE(writer.has_value());
        auto journal = *std::move(writer);
        for (int i = 0; i < 3; ++i) {
            REQUIRE_FALSE(journal.write(sample_message()).has_value());
            REQUIRE_FALSE(journal.rotate().has_value());
        }
    }
    const auto segments = eventbook::list_journal_segments(dir);
    REQUIRE(segments.size() >= 3);
    CHECK(std::is_sorted(segments.begin(), segments.end()));
    std::filesystem::remove_all(dir);
}

TEST_CASE("list_journal_segments ignores unrelated files") {
    const auto dir = scratch_dir("listing");
    {
        auto writer = JournalWriter::create(dir, "alpha");
        REQUIRE(writer.has_value());
        auto journal = *std::move(writer);
        REQUIRE_FALSE(journal.write(sample_message()).has_value());
    }
    {
        auto writer = JournalWriter::create(dir, "beta");
        REQUIRE(writer.has_value());
        auto journal = *std::move(writer);
        REQUIRE_FALSE(journal.write(sample_message()).has_value());
    }
    {
        std::ofstream noise{dir / "notes.txt"};
        noise << "not a journal\n";
    }

    CHECK(eventbook::list_journal_segments(dir).size() == 2);
    CHECK(eventbook::list_journal_segments(dir, "alpha").size() == 1);
    std::filesystem::remove_all(dir);
}

TEST_CASE("a compressed segment truncated mid-frame keeps its earlier frames") {
    // What a crash leaves behind. Writing one zstd frame per flush batch is
    // what makes the surviving prefix readable at all: a single stream spanning
    // the whole segment would lose everything after the failure point.
    const auto dir = scratch_dir("truncated_zstd");
    std::filesystem::path segment;
    {
        JournalWriterConfig config;
        config.flush_every = 50;
        auto writer = JournalWriter::create(dir, "test", config);
        REQUIRE(writer.has_value());
        auto journal = *std::move(writer);
        for (std::uint64_t i = 1; i <= 200; ++i) {
            JournalRecord record = sample_message();
            record.sequence = SequenceNumber{i};
            REQUIRE_FALSE(journal.write(record).has_value());
        }
        REQUIRE_FALSE(journal.flush().has_value());
        segment = journal.current_segment();
    }

    const auto full_size = std::filesystem::file_size(segment);
    std::string bytes;
    {
        std::ifstream in{segment, std::ios::binary};
        bytes.assign(std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{});
    }
    {
        std::ofstream out{segment, std::ios::binary | std::ios::trunc};
        out.write(bytes.data(), static_cast<std::streamsize>(full_size * 3 / 4));
    }

    auto reader = JournalReader::open(segment);
    REQUIRE(reader.has_value());
    auto stream = *std::move(reader);
    std::size_t recovered = 0;
    while (true) {
        auto record = stream.next();
        if (!record.has_value()) {
            break;  // the incomplete final frame
        }
        if (!record->has_value()) {
            break;
        }
        ++recovered;
    }
    // Not all 200, but the complete frames before the tear must survive.
    CHECK(recovered >= 50);
    CHECK(recovered < 200);
    std::filesystem::remove_all(dir);
}

TEST_CASE("an unwritable directory is an error, not a crash") {
    const auto writer = JournalWriter::create("/nonexistent/eventbook/journal", "test");
    REQUIRE_FALSE(writer.has_value());
    CHECK(writer.error().kind == JournalErrorKind::CannotOpen);

    const auto reader = JournalReader::open("/nonexistent/eventbook/journal.jsonl");
    REQUIRE_FALSE(reader.has_value());
    CHECK(reader.error().kind == JournalErrorKind::CannotOpen);
}
