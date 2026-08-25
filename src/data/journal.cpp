#include "eventbook/data/journal.hpp"

#include <nlohmann/json.hpp>

#include <fstream>
#include <ios>
#include <utility>

namespace eventbook {
namespace {

using nlohmann::json;

constexpr const char* kFieldVersion = "v";
constexpr const char* kFieldKind = "kind";
constexpr const char* kFieldLocalTime = "t_local_us";
constexpr const char* kFieldExchangeTime = "t_exchange_us";
constexpr const char* kFieldConnection = "conn";
constexpr const char* kFieldSubscription = "sid";
constexpr const char* kFieldSequence = "seq";
constexpr const char* kFieldType = "type";
constexpr const char* kFieldTicker = "ticker";
constexpr const char* kFieldMarketId = "market_id";
constexpr const char* kFieldPayload = "payload";

}  // namespace

std::string_view to_string(JournalRecordKind kind) {
    switch (kind) {
        case JournalRecordKind::SessionStarted:
            return "session_started";
        case JournalRecordKind::Message:
            return "message";
        case JournalRecordKind::ConnectionLost:
            return "connection_lost";
        case JournalRecordKind::GapDetected:
            return "gap_detected";
        case JournalRecordKind::SnapshotApplied:
            return "snapshot_applied";
        case JournalRecordKind::SessionEnded:
            return "session_ended";
    }
    return "unknown";
}

std::optional<JournalRecordKind> journal_record_kind_from_string(std::string_view text) {
    if (text == "session_started") {
        return JournalRecordKind::SessionStarted;
    }
    if (text == "message") {
        return JournalRecordKind::Message;
    }
    if (text == "connection_lost") {
        return JournalRecordKind::ConnectionLost;
    }
    if (text == "gap_detected") {
        return JournalRecordKind::GapDetected;
    }
    if (text == "snapshot_applied") {
        return JournalRecordKind::SnapshotApplied;
    }
    if (text == "session_ended") {
        return JournalRecordKind::SessionEnded;
    }
    return std::nullopt;
}

std::string_view to_string(JournalErrorKind kind) {
    switch (kind) {
        case JournalErrorKind::CannotOpen:
            return "journal file could not be opened";
        case JournalErrorKind::WriteFailed:
            return "write failed";
        case JournalErrorKind::ReadFailed:
            return "read failed";
        case JournalErrorKind::MalformedRecord:
            return "record is not valid JSON";
        case JournalErrorKind::UnsupportedVersion:
            return "record schema version is not supported";
        case JournalErrorKind::MissingField:
            return "record is missing a required field";
    }
    return "unknown journal error";
}

std::string encode_journal_record(const JournalRecord& record) {
    json line;
    line[kFieldVersion] = kJournalVersion;
    line[kFieldKind] = to_string(record.kind);
    line[kFieldLocalTime] = epoch_micros(record.local_receive_time);
    line[kFieldConnection] = record.connection_id;
    line[kFieldType] = record.message_type;
    line[kFieldPayload] = record.payload;

    // Optional fields are omitted rather than written null, so a reader can
    // tell "the venue did not supply this" from "the venue supplied nothing",
    // and so quiet fields cost nothing per line.
    if (record.exchange_time.has_value()) {
        line[kFieldExchangeTime] = epoch_micros(*record.exchange_time);
    }
    if (record.subscription.has_value()) {
        line[kFieldSubscription] = record.subscription->value;
    }
    if (record.sequence.has_value()) {
        line[kFieldSequence] = record.sequence->value;
    }
    if (record.market_ticker.has_value()) {
        line[kFieldTicker] = record.market_ticker->value;
    }
    if (record.market_id.has_value()) {
        line[kFieldMarketId] = *record.market_id;
    }
    return line.dump();
}

Result<JournalRecord, JournalError> decode_journal_record(std::string_view line) {
    const auto document = json::parse(line, nullptr, /*allow_exceptions=*/false);
    if (document.is_discarded() || !document.is_object()) {
        return Failure{JournalError{JournalErrorKind::MalformedRecord, 0, {}}};
    }

    const auto version = document.find(kFieldVersion);
    if (version == document.end() || !version->is_number_unsigned()) {
        return Failure{JournalError{JournalErrorKind::MissingField, 0, kFieldVersion}};
    }
    if (version->get<std::uint32_t>() != kJournalVersion) {
        return Failure{JournalError{JournalErrorKind::UnsupportedVersion, 0,
                                    std::to_string(version->get<std::uint64_t>())}};
    }

    const auto kind_field = document.find(kFieldKind);
    if (kind_field == document.end() || !kind_field->is_string()) {
        return Failure{JournalError{JournalErrorKind::MissingField, 0, kFieldKind}};
    }
    const auto kind = journal_record_kind_from_string(kind_field->get_ref<const std::string&>());
    if (!kind.has_value()) {
        return Failure{
            JournalError{JournalErrorKind::MalformedRecord, 0, kind_field->get<std::string>()}};
    }

    JournalRecord record;
    record.kind = *kind;

    const auto local_time = document.find(kFieldLocalTime);
    if (local_time == document.end() || !local_time->is_number_integer()) {
        return Failure{JournalError{JournalErrorKind::MissingField, 0, kFieldLocalTime}};
    }
    record.local_receive_time = local_time_from_epoch_micros(local_time->get<std::int64_t>());

    const auto connection = document.find(kFieldConnection);
    if (connection == document.end() || !connection->is_number_unsigned()) {
        return Failure{JournalError{JournalErrorKind::MissingField, 0, kFieldConnection}};
    }
    record.connection_id = connection->get<std::uint64_t>();

    const auto type = document.find(kFieldType);
    if (type == document.end() || !type->is_string()) {
        return Failure{JournalError{JournalErrorKind::MissingField, 0, kFieldType}};
    }
    record.message_type = type->get<std::string>();

    const auto payload = document.find(kFieldPayload);
    if (payload == document.end() || !payload->is_string()) {
        return Failure{JournalError{JournalErrorKind::MissingField, 0, kFieldPayload}};
    }
    record.payload = payload->get<std::string>();

    if (const auto it = document.find(kFieldExchangeTime);
        it != document.end() && it->is_number_integer()) {
        record.exchange_time = exchange_time_from_epoch_micros(it->get<std::int64_t>());
    }
    if (const auto it = document.find(kFieldSubscription);
        it != document.end() && it->is_number_integer()) {
        record.subscription = SubscriptionId{it->get<std::int64_t>()};
    }
    if (const auto it = document.find(kFieldSequence);
        it != document.end() && it->is_number_unsigned()) {
        record.sequence = SequenceNumber{it->get<std::uint64_t>()};
    }
    if (const auto it = document.find(kFieldTicker); it != document.end() && it->is_string()) {
        record.market_ticker = MarketTicker{it->get<std::string>()};
    }
    if (const auto it = document.find(kFieldMarketId); it != document.end() && it->is_string()) {
        record.market_id = it->get<std::string>();
    }
    return record;
}

struct JournalWriter::Impl {
    std::ofstream out;
    JournalWriterConfig config;
    JournalWriterStats stats;
    std::uint64_t since_flush{0};
};

JournalWriter::JournalWriter(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

JournalWriter::~JournalWriter() = default;
JournalWriter::JournalWriter(JournalWriter&&) noexcept = default;
JournalWriter& JournalWriter::operator=(JournalWriter&&) noexcept = default;

Result<JournalWriter, JournalError> JournalWriter::create(const std::filesystem::path& path,
                                                          JournalWriterConfig config) {
    auto impl = std::make_unique<Impl>();
    impl->config = config;
    // Append rather than truncate: a journal is append-only, and reopening one
    // must never silently discard what a previous run recorded.
    impl->out.open(path, std::ios::binary | std::ios::app);
    if (!impl->out) {
        return Failure{JournalError{JournalErrorKind::CannotOpen, 0, path.string()}};
    }
    return JournalWriter{std::move(impl)};
}

std::optional<JournalError> JournalWriter::write(const JournalRecord& record) {
    const auto line = encode_journal_record(record);

    impl_->out.write(line.data(), static_cast<std::streamsize>(line.size()));
    impl_->out.put('\n');
    if (!impl_->out) {
        // Counted before returning, so a caller that ignores the return value
        // still cannot lose the fact that a record was dropped.
        ++impl_->stats.write_failures;
        ++impl_->stats.dropped_records;
        return JournalError{JournalErrorKind::WriteFailed, 0, {}};
    }

    ++impl_->stats.records_written;
    impl_->stats.bytes_written += line.size() + 1;

    if (++impl_->since_flush >= impl_->config.flush_every) {
        return flush();
    }
    return std::nullopt;
}

std::optional<JournalError> JournalWriter::flush() {
    impl_->since_flush = 0;
    impl_->out.flush();
    if (!impl_->out) {
        ++impl_->stats.write_failures;
        return JournalError{JournalErrorKind::WriteFailed, 0, "flush"};
    }
    return std::nullopt;
}

const JournalWriterStats& JournalWriter::stats() const {
    return impl_->stats;
}

struct JournalReader::Impl {
    std::ifstream in;
    std::uint64_t line_number{0};
};

JournalReader::JournalReader(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

JournalReader::~JournalReader() = default;
JournalReader::JournalReader(JournalReader&&) noexcept = default;
JournalReader& JournalReader::operator=(JournalReader&&) noexcept = default;

Result<JournalReader, JournalError> JournalReader::open(const std::filesystem::path& path) {
    auto impl = std::make_unique<Impl>();
    impl->in.open(path, std::ios::binary);
    if (!impl->in) {
        return Failure{JournalError{JournalErrorKind::CannotOpen, 0, path.string()}};
    }
    return JournalReader{std::move(impl)};
}

Result<std::optional<JournalRecord>, JournalError> JournalReader::next() {
    std::string line;
    while (std::getline(impl_->in, line)) {
        ++impl_->line_number;
        // Blank lines are tolerated: concatenating journals is a normal
        // operation and trailing newlines are easy to acquire.
        if (line.empty()) {
            continue;
        }
        auto record = decode_journal_record(line);
        if (!record) {
            auto error = record.error();
            error.line = impl_->line_number;
            return Failure{error};
        }
        return std::optional<JournalRecord>{*record};
    }

    if (impl_->in.bad()) {
        return Failure{JournalError{JournalErrorKind::ReadFailed, impl_->line_number, {}}};
    }
    return std::optional<JournalRecord>{std::nullopt};
}

std::uint64_t JournalReader::line_number() const {
    return impl_->line_number;
}

}  // namespace eventbook
