#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "eventbook/common/identifiers.hpp"
#include "eventbook/common/result.hpp"
#include "eventbook/common/time.hpp"

namespace eventbook {

/// Schema version, written into every record.
///
/// Per record rather than once in a header, deliberately. A journal is an
/// append-only file that a crash can truncate and an operator can concatenate,
/// and a header is exactly the byte range those operations destroy. Repeating
/// the version costs a handful of bytes that compress to nothing and makes any
/// surviving line independently interpretable.
inline constexpr std::uint32_t kJournalVersion = 1;

/// What a journal line describes.
///
/// Lifecycle records exist because AGENTS.md requires a forced disconnect to
/// produce an explicit gap or recovery record. Without them a journal cannot
/// distinguish "the market went quiet" from "we were disconnected and missed
/// everything", and those demand opposite conclusions in research.
enum class JournalRecordKind {
    SessionStarted,   ///< run metadata: build, market, price convention
    Message,          ///< a raw wire payload exactly as received
    ConnectionLost,   ///< the socket dropped; everything after is a new connection
    GapDetected,      ///< a sequence number was skipped
    SnapshotApplied,  ///< book rebuilt from a fresh snapshot, ending an invalid interval
    SessionEnded,     ///< final counters
};

[[nodiscard]] std::string_view to_string(JournalRecordKind kind);
[[nodiscard]] std::optional<JournalRecordKind> journal_record_kind_from_string(
    std::string_view text);

/// One line of the journal.
///
/// A single struct with optionals rather than a variant, because the file
/// format is one self-describing JSON object per line and a reader should not
/// need to know which alternative it is about to decode before decoding it.
///
/// `payload` holds the bytes as they arrived, unmodified. That is the whole
/// point of the raw layer: a parser bug or a feature bug must be fixable by
/// replaying the original journal, never by collecting the market again.
struct JournalRecord {
    JournalRecordKind kind{JournalRecordKind::Message};
    LocalTimestamp local_receive_time;
    std::optional<ExchangeTimestamp> exchange_time;

    /// Identifies one connection within a run. Increments on every reconnect,
    /// so a reader can tell which records share an uninterrupted stream --
    /// and therefore which sequence numbers are comparable.
    std::uint64_t connection_id{};

    std::optional<SubscriptionId> subscription;
    std::optional<SequenceNumber> sequence;

    std::string message_type;  ///< the venue's `type`, or the lifecycle reason
    std::optional<MarketTicker> market_ticker;
    std::optional<std::string> market_id;

    std::string payload;  ///< raw bytes for Message; JSON metadata for lifecycle
};

enum class JournalErrorKind {
    CannotOpen,
    WriteFailed,
    ReadFailed,
    MalformedRecord,
    UnsupportedVersion,
    MissingField,
};

[[nodiscard]] std::string_view to_string(JournalErrorKind kind);

struct JournalError {
    JournalErrorKind kind{};
    std::uint64_t line{};  ///< 1-based; 0 when not line-specific
    std::string detail;

    [[nodiscard]] friend bool operator==(const JournalError&, const JournalError&) = default;
};

/// Counters AGENTS.md requires a writer to expose.
///
/// A session with an unhandled drop is invalid for research, so drops are
/// counted rather than logged and forgotten.
struct JournalWriterStats {
    std::uint64_t records_written{};
    std::uint64_t bytes_written{};
    std::uint64_t write_failures{};
    std::uint64_t dropped_records{};
};

struct JournalWriterConfig {
    /// Flush to the operating system every N records. Buffering is what keeps a
    /// 20-message-per-second feed from issuing a syscall per message; flushing
    /// periodically is what bounds how much a crash can cost.
    std::uint64_t flush_every{256};
};

/// Append-only writer for a versioned JSONL journal.
class JournalWriter {
public:
    [[nodiscard]] static Result<JournalWriter, JournalError> create(
        const std::filesystem::path& path, JournalWriterConfig config = {});

    ~JournalWriter();
    JournalWriter(JournalWriter&&) noexcept;
    JournalWriter& operator=(JournalWriter&&) noexcept;
    JournalWriter(const JournalWriter&) = delete;
    JournalWriter& operator=(const JournalWriter&) = delete;

    /// Append one record. A failure is counted before it is returned, so the
    /// caller cannot lose the fact that something was dropped by ignoring the
    /// return value.
    [[nodiscard]] std::optional<JournalError> write(const JournalRecord& record);

    [[nodiscard]] std::optional<JournalError> flush();

    [[nodiscard]] const JournalWriterStats& stats() const;

private:
    struct Impl;
    explicit JournalWriter(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

/// Sequential reader for a journal file.
class JournalReader {
public:
    [[nodiscard]] static Result<JournalReader, JournalError> open(
        const std::filesystem::path& path);

    ~JournalReader();
    JournalReader(JournalReader&&) noexcept;
    JournalReader& operator=(JournalReader&&) noexcept;
    JournalReader(const JournalReader&) = delete;
    JournalReader& operator=(const JournalReader&) = delete;

    /// The next record, or nullopt at a clean end of file.
    ///
    /// A malformed line is an error carrying its line number rather than a
    /// silent skip. The caller decides what to do about it -- and a malformed
    /// FINAL line specifically means the writer died mid-record, which is an
    /// expected outcome of a crash and does not invalidate what came before.
    [[nodiscard]] Result<std::optional<JournalRecord>, JournalError> next();

    [[nodiscard]] std::uint64_t line_number() const;

private:
    struct Impl;
    explicit JournalReader(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

/// Serialize one record to its JSONL line, without the trailing newline.
/// Exposed so the format can be tested directly rather than only round-tripped.
[[nodiscard]] std::string encode_journal_record(const JournalRecord& record);

/// Parse one JSONL line.
[[nodiscard]] Result<JournalRecord, JournalError> decode_journal_record(std::string_view line);

}  // namespace eventbook
