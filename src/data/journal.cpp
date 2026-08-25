#include "eventbook/data/journal.hpp"

#include <zstd.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstdio>
#include <fstream>
#include <ios>
#include <utility>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <fcntl.h>
#include <unistd.h>
#endif

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

// zstd frame magic. Compression is detected from content rather than from a
// file extension, so a renamed or extensionless segment still reads correctly.
constexpr std::array<unsigned char, 4> kZstdMagic{0x28, 0xB5, 0x2F, 0xFD};

/// Compact UTC stamp for a segment filename: 20260825T090847Z.
///
/// Chosen so filenames sort lexicographically into chronological order, which
/// lets a reader process a directory correctly without parsing anything.
std::string segment_stamp(LocalTimestamp at) {
    // Reuse the RFC 3339 formatter rather than re-deriving the calendar, then
    // strip the characters that are illegal or merely noisy in a filename.
    const auto text = format_rfc3339(ExchangeTimestamp{at.value});
    std::string stamp;
    stamp.reserve(16);
    for (std::size_t i = 0; i < text.size() && stamp.size() < 15; ++i) {
        const char character = text[i];
        if (character == '-' || character == ':') {
            continue;
        }
        if (character == '.') {
            break;
        }
        stamp.push_back(character);
    }
    stamp.push_back('Z');
    return stamp;
}

/// Push data to durable storage. Closing a file hands it to the operating
/// system; only fsync survives a power loss.
void fsync_file(const std::filesystem::path& path) {
#if defined(__unix__) || defined(__APPLE__)
    const int descriptor = ::open(path.c_str(), O_RDONLY);
    if (descriptor >= 0) {
        ::fsync(descriptor);
        ::close(descriptor);
    }
#else
    (void)path;  // No portable equivalent; the caller is told this is a no-op.
#endif
}

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
        case JournalErrorKind::CompressionFailed:
            return "zstd compression failed";
        case JournalErrorKind::DecompressionFailed:
            return "zstd decompression failed";
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
    std::filesystem::path directory;
    std::string prefix;
    std::filesystem::path segment_path;
    JournalWriterConfig config;
    JournalWriterStats stats;

    /// Records buffered since the last flush, as raw JSONL text. Compression
    /// happens on the whole batch so one zstd frame covers many records.
    std::string pending;
    std::uint64_t pending_records{0};
    std::uint64_t segment_uncompressed{0};
    LocalTimestamp segment_opened;

    [[nodiscard]] std::optional<JournalError> open_segment() {
        const auto now = local_now();
        const std::string stamp = segment_stamp(now);
        const std::string extension = std::string{".jsonl"} + (config.compress ? ".zst" : "");

        // Every name carries a zero-padded counter, including the first. That
        // uniformity is what makes lexicographic order match chronological
        // order: an optional suffix would not, because '-' (0x2D) sorts before
        // '.' (0x2E), so "...Z-1.jsonl" would come BEFORE "...Z.jsonl" and
        // silently reorder segments during replay.
        std::filesystem::path candidate;
        int counter = 0;
        do {
            std::array<char, 8> suffix{};
            std::snprintf(suffix.data(), suffix.size(), "-%03d", counter);
            candidate = directory / (prefix + "-" + stamp + suffix.data() + extension);
            ++counter;
        } while (std::filesystem::exists(candidate) && counter < 1000);

        out.open(candidate, std::ios::binary | std::ios::app);
        if (!out) {
            return JournalError{JournalErrorKind::CannotOpen, 0, candidate.string()};
        }
        segment_path = candidate;
        segment_opened = now;
        segment_uncompressed = 0;
        ++stats.segments_created;
        return std::nullopt;
    }

    [[nodiscard]] std::optional<JournalError> write_block(const std::string& block) {
        if (block.empty()) {
            return std::nullopt;
        }
        if (!config.compress) {
            out.write(block.data(), static_cast<std::streamsize>(block.size()));
            if (!out) {
                return JournalError{JournalErrorKind::WriteFailed, 0, {}};
            }
            stats.bytes_on_disk += block.size();
            return std::nullopt;
        }

        std::vector<char> compressed(ZSTD_compressBound(block.size()));
        const std::size_t written =
            ZSTD_compress(compressed.data(), compressed.size(), block.data(), block.size(),
                          config.compression_level);
        if (ZSTD_isError(written) != 0U) {
            return JournalError{JournalErrorKind::CompressionFailed, 0, ZSTD_getErrorName(written)};
        }
        out.write(compressed.data(), static_cast<std::streamsize>(written));
        if (!out) {
            return JournalError{JournalErrorKind::WriteFailed, 0, {}};
        }
        stats.bytes_on_disk += written;
        return std::nullopt;
    }
};

JournalWriter::JournalWriter(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

JournalWriter::~JournalWriter() = default;
JournalWriter::JournalWriter(JournalWriter&&) noexcept = default;
JournalWriter& JournalWriter::operator=(JournalWriter&&) noexcept = default;

Result<JournalWriter, JournalError> JournalWriter::create(const std::filesystem::path& directory,
                                                          std::string prefix,
                                                          JournalWriterConfig config) {
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    if (ec && !std::filesystem::is_directory(directory)) {
        return Failure{JournalError{JournalErrorKind::CannotOpen, 0, directory.string()}};
    }

    auto impl = std::make_unique<Impl>();
    impl->directory = directory;
    impl->prefix = std::move(prefix);
    impl->config = config;
    if (auto problem = impl->open_segment()) {
        return Failure{*problem};
    }
    return JournalWriter{std::move(impl)};
}

std::optional<JournalError> JournalWriter::write(const JournalRecord& record) {
    const auto line = encode_journal_record(record);
    impl_->pending.append(line);
    impl_->pending.push_back('\n');
    ++impl_->pending_records;

    const auto added = line.size() + 1;
    ++impl_->stats.records_written;
    impl_->stats.uncompressed_bytes += added;
    impl_->segment_uncompressed += added;

    if (impl_->pending_records >= impl_->config.flush_every) {
        if (auto problem = flush()) {
            return problem;
        }
    }

    // Size is checked against uncompressed bytes because that is the figure a
    // caller can reason about; compressed size depends on how repetitive the
    // market happened to be.
    const bool by_size =
        impl_->config.rotate_bytes > 0 && impl_->segment_uncompressed >= impl_->config.rotate_bytes;
    const bool by_time = impl_->config.rotate_interval.count() > 0 &&
                         (local_now() - impl_->segment_opened) >= impl_->config.rotate_interval;
    if (by_size || by_time) {
        return rotate();
    }
    return std::nullopt;
}

std::optional<JournalError> JournalWriter::flush() {
    if (!impl_->pending.empty()) {
        if (auto problem = impl_->write_block(impl_->pending)) {
            // Counted before returning, so a caller that ignores the return
            // value still cannot lose the fact that records were dropped.
            ++impl_->stats.write_failures;
            impl_->stats.dropped_records += impl_->pending_records;
            impl_->pending.clear();
            impl_->pending_records = 0;
            return problem;
        }
        impl_->pending.clear();
        impl_->pending_records = 0;
    }

    impl_->out.flush();
    if (!impl_->out) {
        ++impl_->stats.write_failures;
        return JournalError{JournalErrorKind::WriteFailed, 0, "flush"};
    }
    return std::nullopt;
}

std::optional<JournalError> JournalWriter::rotate() {
    auto problem = flush();
    const auto closing = impl_->segment_path;
    impl_->out.close();
    if (impl_->config.fsync_on_rotate) {
        fsync_file(closing);
    }
    if (auto opened = impl_->open_segment()) {
        return opened;
    }
    return problem;
}

const std::filesystem::path& JournalWriter::current_segment() const {
    return impl_->segment_path;
}

const JournalWriterStats& JournalWriter::stats() const {
    return impl_->stats;
}

std::vector<std::filesystem::path> list_journal_segments(const std::filesystem::path& directory,
                                                         std::string_view prefix) {
    std::vector<std::filesystem::path> segments;
    std::error_code ec;
    if (!std::filesystem::is_directory(directory, ec)) {
        return segments;
    }
    for (const auto& entry : std::filesystem::directory_iterator{directory, ec}) {
        if (!entry.is_regular_file()) {
            continue;
        }
        const auto name = entry.path().filename().string();
        if (name.find(".jsonl") == std::string::npos) {
            continue;
        }
        if (!prefix.empty() && name.rfind(prefix, 0) != 0) {
            continue;
        }
        segments.push_back(entry.path());
    }
    // The stamp sorts chronologically as text, so ordering needs no parsing.
    std::sort(segments.begin(), segments.end());
    return segments;
}

struct JournalReader::Impl {
    std::ifstream in;
    std::uint64_t line_number{0};
    bool compressed{false};

    // Decompression state. Nothing here is allocated for a plain-text journal.
    std::unique_ptr<ZSTD_DCtx, decltype(&ZSTD_freeDCtx)> context{nullptr, ZSTD_freeDCtx};
    std::vector<char> input;
    std::vector<char> output;
    std::size_t input_size{0};
    std::size_t input_offset{0};
    std::string decoded;  ///< decompressed bytes not yet split into lines
    std::size_t decoded_offset{0};
    bool source_exhausted{false};

    /// Pull one line out of the decompressed buffer, decompressing more when
    /// the buffer holds no complete line. Streaming rather than decompressing
    /// the whole segment because a segment can be hundreds of megabytes.
    [[nodiscard]] bool next_compressed_line(std::string& line, bool& failed, std::string& detail) {
        failed = false;
        while (true) {
            const auto newline = decoded.find('\n', decoded_offset);
            if (newline != std::string::npos) {
                line.assign(decoded, decoded_offset, newline - decoded_offset);
                decoded_offset = newline + 1;
                return true;
            }
            // Compact rather than grow without bound.
            if (decoded_offset > 0) {
                decoded.erase(0, decoded_offset);
                decoded_offset = 0;
            }
            if (source_exhausted) {
                if (!decoded.empty()) {
                    line = decoded;
                    decoded.clear();
                    return true;
                }
                return false;
            }

            if (input_offset == input_size) {
                in.read(input.data(), static_cast<std::streamsize>(input.size()));
                input_size = static_cast<std::size_t>(in.gcount());
                input_offset = 0;
                if (input_size == 0) {
                    source_exhausted = true;
                    continue;
                }
            }

            ZSTD_inBuffer in_buffer{input.data(), input_size, input_offset};
            ZSTD_outBuffer out_buffer{output.data(), output.size(), 0};
            const std::size_t status =
                ZSTD_decompressStream(context.get(), &out_buffer, &in_buffer);
            if (ZSTD_isError(status) != 0U) {
                failed = true;
                detail = ZSTD_getErrorName(status);
                return false;
            }
            input_offset = in_buffer.pos;
            decoded.append(output.data(), out_buffer.pos);
        }
    }
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

    // Detect compression from the magic bytes rather than the extension, so a
    // renamed segment still reads.
    std::array<char, kZstdMagic.size()> magic{};
    impl->in.read(magic.data(), static_cast<std::streamsize>(magic.size()));
    const auto read_bytes = static_cast<std::size_t>(impl->in.gcount());
    impl->in.clear();
    impl->in.seekg(0);

    impl->compressed = read_bytes == kZstdMagic.size() &&
                       std::equal(kZstdMagic.begin(), kZstdMagic.end(), magic.begin(),
                                  [](unsigned char expected, char actual) {
                                      return expected == static_cast<unsigned char>(actual);
                                  });

    if (impl->compressed) {
        impl->context.reset(ZSTD_createDCtx());
        if (!impl->context) {
            return Failure{JournalError{JournalErrorKind::DecompressionFailed, 0, "no context"}};
        }
        impl->input.resize(ZSTD_DStreamInSize());
        impl->output.resize(ZSTD_DStreamOutSize());
    }
    return JournalReader{std::move(impl)};
}

Result<std::optional<JournalRecord>, JournalError> JournalReader::next() {
    std::string line;
    while (true) {
        if (impl_->compressed) {
            bool failed = false;
            std::string detail;
            if (!impl_->next_compressed_line(line, failed, detail)) {
                if (failed) {
                    return Failure{JournalError{JournalErrorKind::DecompressionFailed,
                                                impl_->line_number, detail}};
                }
                return std::optional<JournalRecord>{std::nullopt};
            }
        } else {
            if (!std::getline(impl_->in, line)) {
                if (impl_->in.bad()) {
                    return Failure{
                        JournalError{JournalErrorKind::ReadFailed, impl_->line_number, {}}};
                }
                return std::optional<JournalRecord>{std::nullopt};
            }
        }

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
}

std::uint64_t JournalReader::line_number() const {
    return impl_->line_number;
}

bool JournalReader::is_compressed() const {
    return impl_->compressed;
}

}  // namespace eventbook
