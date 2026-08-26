#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "eventbook/api/ws_protocol.hpp"
#include "eventbook/book/market_state.hpp"
#include "eventbook/common/identifiers.hpp"
#include "eventbook/common/price.hpp"
#include "eventbook/common/result.hpp"
#include "eventbook/data/journal.hpp"

namespace eventbook {

/// What a journal needs to say about itself for replay to reproduce it.
///
/// Written into the SessionStarted record so a journal is SELF-DESCRIBING.
/// Replay must not depend on a configuration file that happened to be sitting
/// next to it: the price convention in particular is a subscription-time
/// decision the messages never restate, and getting it wrong silently mirrors
/// every ask rather than failing.
struct SessionMetadata {
    MarketTicker market_ticker;
    PriceConvention price_convention{PriceConvention::YesLegPricing};
    std::vector<PriceRange> price_grid;
    std::string build;  ///< the binary that produced the journal
};

[[nodiscard]] std::string encode_session_metadata(const SessionMetadata& metadata);

enum class ReplayErrorKind {
    NoSegments,
    JournalFailure,
    MissingSessionRecord,
    MalformedMetadata,
    ParseFailure,
};

[[nodiscard]] std::string_view to_string(ReplayErrorKind kind);

struct ReplayError {
    ReplayErrorKind kind{};
    JournalError journal;
    std::string detail;
};

[[nodiscard]] Result<SessionMetadata, ReplayError> decode_session_metadata(
    std::string_view json_text);

struct ReplayOptions {
    /// Use this instead of the journal's own SessionStarted record. Normally
    /// unnecessary and slightly dangerous -- it is here for journals recorded
    /// before metadata existed, and for deliberately replaying under a
    /// different assumption to see what breaks.
    std::optional<SessionMetadata> metadata_override;

    /// Stop at the first unparseable payload rather than counting it. Off by
    /// default: a recorder that kept running through a bad message should not
    /// have its whole journal rejected at replay time, and the count is what
    /// makes the damage visible.
    bool stop_on_parse_failure{false};

    /// Called before each record is applied, with the state as it currently
    /// stands. This is where a feature sampler hooks in: a row must describe
    /// the book as of an interval boundary using only events at or before it,
    /// so sampling has to happen before the next event lands. Observing
    /// afterwards would let information from after the boundary into the row.
    /// The event is nullptr for lifecycle records and for payloads that did
    /// not parse, so a caller never has to parse the payload a second time.
    std::function<void(const JournalRecord&, const MarketEvent*, const MarketState&)> before_record;

    /// Called once the journal is exhausted, so a sampler can close out its
    /// final partial interval.
    std::function<void(const MarketState&, LocalTimestamp)> at_end;
};

struct ReplayResult {
    SessionMetadata metadata;
    std::uint64_t segments{};
    std::uint64_t records{};
    std::uint64_t messages{};
    std::uint64_t lifecycle_records{};
    std::uint64_t parse_failures{};

    MarketStateStats market;

    /// The whole point of the milestone. Two replays of one journal must agree
    /// on this, or nothing computed downstream is reproducible.
    std::uint64_t final_state_hash{};

    std::optional<LocalTimestamp> first_record_time;
    std::optional<LocalTimestamp> last_record_time;
};

/// Replay a list of segments, in the order given.
///
/// Feeds each recorded payload through the SAME normalizer and the SAME
/// MarketState the live path uses. Nothing here re-implements book logic, which
/// is what makes a replayed result comparable to a live one rather than merely
/// similar to it.
[[nodiscard]] Result<ReplayResult, ReplayError> replay_segments(
    const std::vector<std::filesystem::path>& segments, const ReplayOptions& options = {});

/// Replay every segment in a directory, in chronological order.
[[nodiscard]] Result<ReplayResult, ReplayError> replay_directory(
    const std::filesystem::path& directory, std::string_view prefix = {},
    const ReplayOptions& options = {});

}  // namespace eventbook
