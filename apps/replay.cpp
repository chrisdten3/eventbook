// replay - reconstructs a recorded session from its journal.
//
// Feeds every recorded payload through the same normalizer and the same
// MarketState the live collector uses, then reports the final state hash. If
// that hash matches the one collect printed, the journal is a faithful record
// of the session and every downstream result computed from it is reproducible.
//
// Reads only. Nothing here touches the network.

#include <spdlog/spdlog.h>
#include <CLI/CLI.hpp>

#include <chrono>
#include <cstdint>
#include <string>

#include "eventbook/common/version.hpp"
#include "eventbook/replay/replay.hpp"

int main(int argc, char** argv) {
    CLI::App app{"eventbook replay - rebuild a session from its journal"};
    app.set_version_flag("--version", eventbook::describe_build());

    std::string journal_dir;
    app.add_option("-j,--journal-dir", journal_dir, "Directory holding journal segments")
        ->required();

    std::string prefix;
    app.add_option("-p,--prefix", prefix, "Only replay segments with this filename prefix");

    bool strict{false};
    app.add_flag("--strict", strict, "Stop at the first payload that fails to parse");

    std::uint64_t repeat{1};
    app.add_option("-n,--repeat", repeat,
                   "Replay this many times and verify every run agrees. This is the "
                   "determinism check: a differing hash means results are not reproducible");

    std::string log_level{"info"};
    app.add_option("--log-level", log_level, "trace|debug|info|warn|error");

    CLI11_PARSE(app, argc, argv);

    spdlog::set_pattern("%v");
    spdlog::set_level(spdlog::level::from_str(log_level));

    eventbook::ReplayOptions options;
    options.stop_on_parse_failure = strict;

    std::optional<std::uint64_t> agreed_hash;
    for (std::uint64_t run = 0; run < std::max<std::uint64_t>(repeat, 1); ++run) {
        const auto started = eventbook::local_now();
        const auto result = eventbook::replay_directory(journal_dir, prefix, options);
        if (!result) {
            spdlog::error("replay failed: {}", to_string(result.error().kind));
            if (!result.error().detail.empty()) {
                spdlog::error("  {}", result.error().detail);
            }
            if (result.error().journal.line != 0) {
                spdlog::error("  journal line {}: {}", result.error().journal.line,
                              to_string(result.error().journal.kind));
            }
            return 1;
        }

        const auto elapsed = eventbook::local_now() - started;
        const double seconds =
            std::chrono::duration_cast<std::chrono::duration<double>>(elapsed).count();

        if (run == 0) {
            spdlog::info("--- replay ---");
            spdlog::info("market               {}", result->metadata.market_ticker.value);
            spdlog::info(
                "price convention     {}",
                result->metadata.price_convention == eventbook::PriceConvention::YesLegPricing
                    ? "yes_leg"
                    : "no_leg");
            spdlog::info("recorded by          {}", result->metadata.build);
            spdlog::info("segments             {}", result->segments);
            spdlog::info("records              {}", result->records);
            spdlog::info("messages             {}", result->messages);
            spdlog::info("lifecycle records    {}", result->lifecycle_records);
            spdlog::info("parse failures       {}", result->parse_failures);
            spdlog::info("snapshots            {}", result->market.snapshots);
            spdlog::info("deltas               {}", result->market.deltas);
            spdlog::info("trades               {}", result->market.trades);
            spdlog::info("sequence gaps        {}", result->market.sequence_gaps);
            spdlog::info("rejected deltas      {}", result->market.rejected_deltas);
            spdlog::info("disconnections       {}", result->market.disconnections);
            spdlog::info("invalid book time    {:.3f}s",
                         std::chrono::duration_cast<std::chrono::duration<double>>(
                             result->market.invalid_time)
                             .count());
            spdlog::info("final state hash     {:016x}", result->final_state_hash);
            spdlog::info("replay took          {:.2f}s ({:.0f} messages/s)", seconds,
                         seconds > 0 ? static_cast<double>(result->messages) / seconds : 0.0);
        }

        if (!agreed_hash.has_value()) {
            agreed_hash = result->final_state_hash;
        } else if (*agreed_hash != result->final_state_hash) {
            // The one failure this tool exists to catch. Without determinism, a
            // change in results can never be attributed to a change in code.
            spdlog::error("NON-DETERMINISTIC: run {} produced {:016x}, expected {:016x}", run,
                          result->final_state_hash, *agreed_hash);
            return 1;
        }
    }

    if (repeat > 1) {
        spdlog::info("determinism          {} replays agree on {:016x}", repeat, *agreed_hash);
    }
    return 0;
}
