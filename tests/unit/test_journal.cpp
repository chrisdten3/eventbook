#include "eventbook/data/journal.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
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

std::filesystem::path scratch_path(const std::string& name) {
    return std::filesystem::temp_directory_path() / ("eventbook_journal_" + name + ".jsonl");
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
    const auto path = scratch_path("roundtrip");
    std::filesystem::remove(path);

    {
        auto writer = JournalWriter::create(path);
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
        CHECK(journal.stats().bytes_written > 0);
    }

    auto reader = JournalReader::open(path);
    REQUIRE(reader.has_value());
    auto stream = *std::move(reader);

    std::vector<std::uint64_t> sequences;
    while (true) {
        auto record = stream.next();
        REQUIRE(record.has_value());
        if (!record->has_value()) {
            break;
        }
        sequences.push_back((*record)->sequence->value);
    }

    REQUIRE(sequences.size() == 50);
    for (std::uint64_t i = 0; i < 50; ++i) {
        CHECK(sequences[i] == i + 1);
    }
    std::filesystem::remove(path);
}

TEST_CASE("opening an existing journal appends rather than truncating") {
    // A journal is append-only. Reopening one must never discard what a
    // previous run recorded.
    const auto path = scratch_path("append");
    std::filesystem::remove(path);

    for (int run = 0; run < 2; ++run) {
        auto writer = JournalWriter::create(path);
        REQUIRE(writer.has_value());
        auto journal = *std::move(writer);
        REQUIRE_FALSE(journal.write(sample_message()).has_value());
        REQUIRE_FALSE(journal.flush().has_value());
    }

    auto reader = JournalReader::open(path);
    REQUIRE(reader.has_value());
    auto stream = *std::move(reader);
    int count = 0;
    while (true) {
        auto record = stream.next();
        REQUIRE(record.has_value());
        if (!record->has_value())
            break;
        ++count;
    }
    CHECK(count == 2);
    std::filesystem::remove(path);
}

TEST_CASE("a truncated final line is reported with its line number") {
    // Exactly what a crash mid-write leaves behind. The records before it are
    // intact and must stay readable; only the last line is lost, and the reader
    // has to say which one so a caller can decide that is tolerable.
    const auto path = scratch_path("truncated");
    std::filesystem::remove(path);
    {
        std::ofstream out{path, std::ios::binary};
        out << encode_journal_record(sample_message()) << "\n";
        out << encode_journal_record(sample_message()) << "\n";
        const auto partial = encode_journal_record(sample_message());
        out << partial.substr(0, partial.size() / 2) << "\n";
    }

    auto reader = JournalReader::open(path);
    REQUIRE(reader.has_value());
    auto stream = *std::move(reader);

    REQUIRE(stream.next()->has_value());
    REQUIRE(stream.next()->has_value());

    const auto broken = stream.next();
    REQUIRE_FALSE(broken.has_value());
    CHECK(broken.error().kind == JournalErrorKind::MalformedRecord);
    CHECK(broken.error().line == 3);
    std::filesystem::remove(path);
}

TEST_CASE("blank lines are tolerated") {
    // Concatenating journals is a normal operation and trailing newlines are
    // easy to acquire.
    const auto path = scratch_path("blanks");
    std::filesystem::remove(path);
    {
        std::ofstream out{path, std::ios::binary};
        out << encode_journal_record(sample_message()) << "\n\n";
        out << encode_journal_record(sample_message()) << "\n";
    }

    auto reader = JournalReader::open(path);
    REQUIRE(reader.has_value());
    auto stream = *std::move(reader);
    int count = 0;
    while (true) {
        auto record = stream.next();
        REQUIRE(record.has_value());
        if (!record->has_value())
            break;
        ++count;
    }
    CHECK(count == 2);
    std::filesystem::remove(path);
}

TEST_CASE("opening an unwritable path is an error, not a crash") {
    const auto writer = JournalWriter::create("/nonexistent/eventbook/journal.jsonl");
    REQUIRE_FALSE(writer.has_value());
    CHECK(writer.error().kind == JournalErrorKind::CannotOpen);

    const auto reader = JournalReader::open("/nonexistent/eventbook/journal.jsonl");
    REQUIRE_FALSE(reader.has_value());
    CHECK(reader.error().kind == JournalErrorKind::CannotOpen);
}
