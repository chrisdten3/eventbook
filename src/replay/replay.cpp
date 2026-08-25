#include "eventbook/replay/replay.hpp"

#include <nlohmann/json.hpp>

#include <string>
#include <utility>

namespace eventbook {
namespace {

using nlohmann::json;

}  // namespace

std::string_view to_string(ReplayErrorKind kind) {
    switch (kind) {
        case ReplayErrorKind::NoSegments:
            return "no journal segments found";
        case ReplayErrorKind::JournalFailure:
            return "journal could not be read";
        case ReplayErrorKind::MissingSessionRecord:
            return "journal has no session_started record";
        case ReplayErrorKind::MalformedMetadata:
            return "session metadata could not be parsed";
        case ReplayErrorKind::ParseFailure:
            return "a recorded payload did not parse";
    }
    return "unknown replay error";
}

std::string encode_session_metadata(const SessionMetadata& metadata) {
    json document;
    document["market_ticker"] = metadata.market_ticker.value;
    document["price_convention"] =
        metadata.price_convention == PriceConvention::YesLegPricing ? "yes_leg" : "no_leg";
    document["build"] = metadata.build;

    // The grid travels with the journal so replay validates prices exactly as
    // the live run did. Fetching it fresh at replay time would be wrong: a
    // market's grid can change, and replay must reproduce the past, not the
    // present.
    auto grid = json::array();
    for (const auto& band : metadata.price_grid) {
        grid.push_back(
            json{{"start", band.start.units}, {"end", band.end.units}, {"step", band.step.units}});
    }
    document["price_ranges"] = std::move(grid);
    return document.dump();
}

Result<SessionMetadata, ReplayError> decode_session_metadata(std::string_view json_text) {
    const auto document = json::parse(json_text, nullptr, /*allow_exceptions=*/false);
    if (document.is_discarded() || !document.is_object()) {
        return Failure{ReplayError{ReplayErrorKind::MalformedMetadata, {}, "not an object"}};
    }

    SessionMetadata metadata;
    if (const auto it = document.find("market_ticker"); it != document.end() && it->is_string()) {
        metadata.market_ticker = MarketTicker{it->get<std::string>()};
    } else {
        return Failure{ReplayError{ReplayErrorKind::MalformedMetadata, {}, "market_ticker"}};
    }

    if (const auto it = document.find("price_convention");
        it != document.end() && it->is_string()) {
        metadata.price_convention = it->get<std::string>() == "no_leg"
                                        ? PriceConvention::NoLegPricing
                                        : PriceConvention::YesLegPricing;
    } else {
        return Failure{ReplayError{ReplayErrorKind::MalformedMetadata, {}, "price_convention"}};
    }

    if (const auto it = document.find("build"); it != document.end() && it->is_string()) {
        metadata.build = it->get<std::string>();
    }

    if (const auto it = document.find("price_ranges"); it != document.end() && it->is_array()) {
        for (const auto& band : *it) {
            if (!band.is_object()) {
                return Failure{ReplayError{ReplayErrorKind::MalformedMetadata, {}, "price_ranges"}};
            }
            PriceRange range;
            range.start = Price{band.value("start", 0)};
            range.end = Price{band.value("end", 0)};
            range.step = PriceDelta{band.value("step", 0)};
            metadata.price_grid.push_back(range);
        }
    }
    return metadata;
}

Result<ReplayResult, ReplayError> replay_segments(
    const std::vector<std::filesystem::path>& segments, const ReplayOptions& options) {
    if (segments.empty()) {
        return Failure{ReplayError{ReplayErrorKind::NoSegments, {}, {}}};
    }

    ReplayResult result;
    std::optional<SessionMetadata> metadata = options.metadata_override;
    std::optional<MarketState> state;
    std::optional<LocalTimestamp> last_time;

    for (const auto& segment : segments) {
        auto reader = JournalReader::open(segment);
        if (!reader) {
            return Failure{
                ReplayError{ReplayErrorKind::JournalFailure, reader.error(), segment.string()}};
        }
        auto stream = *std::move(reader);
        ++result.segments;

        while (true) {
            auto next = stream.next();
            if (!next) {
                return Failure{
                    ReplayError{ReplayErrorKind::JournalFailure, next.error(), segment.string()}};
            }
            if (!next->has_value()) {
                break;
            }
            const JournalRecord& record = **next;

            ++result.records;
            if (!result.first_record_time.has_value()) {
                result.first_record_time = record.local_receive_time;
            }
            last_time = record.local_receive_time;

            if (record.kind == JournalRecordKind::SessionStarted) {
                ++result.lifecycle_records;
                if (!metadata.has_value()) {
                    auto parsed = decode_session_metadata(record.payload);
                    if (!parsed) {
                        return Failure{parsed.error()};
                    }
                    metadata = *parsed;
                }
                continue;
            }

            // Everything below needs the book, which needs the metadata. A
            // journal whose first records precede its session record cannot be
            // replayed, and saying so beats guessing a convention.
            if (!metadata.has_value()) {
                return Failure{
                    ReplayError{ReplayErrorKind::MissingSessionRecord, {}, segment.string()}};
            }
            if (!state.has_value()) {
                state.emplace(metadata->market_ticker, metadata->price_grid);
            }

            if (record.kind == JournalRecordKind::ConnectionLost) {
                ++result.lifecycle_records;
                // The book cannot span a disconnection: whatever the venue did
                // while we were away is unknown.
                state->on_disconnected(record.local_receive_time);
                continue;
            }
            if (record.kind != JournalRecordKind::Message) {
                ++result.lifecycle_records;
                continue;
            }

            ++result.messages;
            auto event = parse_ws_message(record.payload, metadata->price_convention);
            if (!event) {
                ++result.parse_failures;
                if (options.stop_on_parse_failure) {
                    return Failure{ReplayError{
                        ReplayErrorKind::ParseFailure,
                        {},
                        std::string{to_string(event.error().kind)} + " on " + event.error().field}};
                }
                continue;
            }
            (void)state->apply(*event, record.local_receive_time);
        }
    }

    if (!metadata.has_value()) {
        return Failure{ReplayError{ReplayErrorKind::MissingSessionRecord, {}, {}}};
    }
    result.metadata = *metadata;
    result.last_record_time = last_time;

    if (state.has_value()) {
        if (last_time.has_value()) {
            state->finish(*last_time);
        }
        result.market = state->stats();
        result.final_state_hash = state->state_hash();
    } else {
        // A journal with metadata but no messages still has a well-defined
        // final state: an empty, never-snapshotted book.
        MarketState empty{metadata->market_ticker, metadata->price_grid};
        result.final_state_hash = empty.state_hash();
    }
    return result;
}

Result<ReplayResult, ReplayError> replay_directory(const std::filesystem::path& directory,
                                                   std::string_view prefix,
                                                   const ReplayOptions& options) {
    return replay_segments(list_journal_segments(directory, prefix), options);
}

}  // namespace eventbook
