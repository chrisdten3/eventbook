// build_features - turns a journal into a one-second research dataset.
//
// Replays the journal through the same normalizer and the same MarketState the
// live collector uses, sampling the book on a fixed interval. The dataset is
// therefore derived from the recorded bytes and nothing else: rerunning this
// after a feature bug is fixed costs seconds, not another day of collection.
//
// No labels are produced here. Anything depending on future information belongs
// in a separate offline pass (AGENTS.md, "Feature definitions"), because a
// feature engine that can see the future is a leak waiting to be discovered.

#include <spdlog/spdlog.h>
#include <CLI/CLI.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

#include "eventbook/common/version.hpp"
#include "eventbook/features/feature_engine.hpp"
#include "eventbook/replay/replay.hpp"

int main(int argc, char** argv) {
    CLI::App app{"eventbook build_features - derive a one-second dataset from a journal"};
    app.set_version_flag("--version", eventbook::describe_build());

    std::string journal_dir;
    app.add_option("-j,--journal-dir", journal_dir, "Directory holding journal segments")
        ->required();

    std::string output_path;
    app.add_option("-o,--output", output_path, "CSV file to write")->required();

    std::int64_t interval_seconds{1};
    app.add_option("-i,--interval", interval_seconds, "Sampling interval in seconds");

    std::string prefix;
    app.add_option("-p,--prefix", prefix, "Only replay segments with this filename prefix");

    std::vector<std::int64_t> window_seconds{10, 60};
    app.add_option("-w,--window", window_seconds,
                   "Trailing window lengths in seconds (repeatable)");

    std::string log_level{"info"};
    app.add_option("--log-level", log_level, "trace|debug|info|warn|error");

    CLI11_PARSE(app, argc, argv);
    spdlog::set_pattern("%v");
    spdlog::set_level(spdlog::level::from_str(log_level));

    std::vector<std::chrono::seconds> windows;
    windows.reserve(window_seconds.size());
    for (const auto seconds_in_window : window_seconds) {
        windows.emplace_back(seconds_in_window);
    }
    std::sort(windows.begin(), windows.end());

    std::ofstream out{output_path};
    if (!out) {
        spdlog::error("cannot open {}", output_path);
        return 1;
    }
    // The header is written from the configured windows, not from the sampler,
    // because the sampler cannot exist until the journal names its market.
    out << eventbook::feature_row_header(windows) << "\n";

    std::optional<eventbook::FeatureSampler> sampler;
    std::uint64_t written{0};

    eventbook::ReplayOptions options;
    options.before_record = [&](const eventbook::JournalRecord& record,
                                const eventbook::MarketEvent* event,
                                const eventbook::MarketState& state) {
        if (!sampler.has_value()) {
            sampler.emplace(record.market_ticker.value_or(state.book().ticker()),
                            std::chrono::seconds{interval_seconds}, windows);
        }
        // Sample first, then let the event land. The row describes the book as
        // of the boundary, using only what arrived at or before it.
        sampler->advance(state, record.local_receive_time, [&](const eventbook::FeatureRow& row) {
            out << to_csv(row) << "\n";
            ++written;
        });
        sampler->observe(record.exchange_time, record.sequence);
        if (event != nullptr) {
            sampler->observe_event(*event, record.local_receive_time);
        }
    };
    options.at_end = [&](const eventbook::MarketState& state, eventbook::LocalTimestamp at) {
        if (sampler.has_value()) {
            sampler->finish(state, at, [&](const eventbook::FeatureRow& row) {
                out << to_csv(row) << "\n";
                ++written;
            });
        }
    };

    const auto started = eventbook::local_now();
    const auto result = eventbook::replay_directory(journal_dir, prefix, options);
    if (!result) {
        spdlog::error("replay failed: {}", to_string(result.error().kind));
        return 1;
    }
    out.flush();
    if (!out) {
        spdlog::error("write failed on {}", output_path);
        return 1;
    }

    const auto elapsed = eventbook::local_now() - started;
    const double seconds =
        std::chrono::duration_cast<std::chrono::duration<double>>(elapsed).count();

    const std::uint64_t invalid = sampler.has_value() ? sampler->invalid_rows() : 0;
    spdlog::info("--- feature build ---");
    spdlog::info("market               {}", result->metadata.market_ticker.value);
    spdlog::info("source hash          {:016x}", result->final_state_hash);
    spdlog::info("messages replayed    {}", result->messages);
    spdlog::info("interval             {}s", interval_seconds);
    spdlog::info("rows written         {}", written);
    spdlog::info(
        "rows on invalid book {} ({:.3f}%)", invalid,
        written > 0 ? 100.0 * static_cast<double>(invalid) / static_cast<double>(written) : 0.0);
    spdlog::info("elapsed              {:.2f}s", seconds);
    spdlog::info("output               {}", output_path);
    return 0;
}
