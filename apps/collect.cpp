// collect - records raw Kalshi market data to an immutable journal.
//
// M0 status: argument parsing and build reporting only. No network code exists
// yet, and by design none of it will ever place, amend, or cancel an order:
// version one of this project is strictly read-only with respect to the
// exchange (AGENTS.md, "Safety boundary").

#include "eventbook/common/version.hpp"

#include <spdlog/spdlog.h>
#include <CLI/CLI.hpp>

#include <string>
#include <vector>

int main(int argc, char** argv) {
    CLI::App app{"eventbook collect - read-only Kalshi market data recorder"};
    app.set_version_flag("--version", eventbook::describe_build());

    std::string config_path;
    app.add_option("-c,--config", config_path, "Path to a YAML configuration file")
        ->check(CLI::ExistingFile);

    std::vector<std::string> tickers;
    app.add_option("-m,--market", tickers, "Market ticker to record (repeatable)");

    std::string log_level{"info"};
    app.add_option("--log-level", log_level, "trace|debug|info|warn|error")->default_str("info");

    CLI11_PARSE(app, argc, argv);

    spdlog::set_level(spdlog::level::from_str(log_level));
    spdlog::info("{}", eventbook::describe_build());

    if (tickers.empty()) {
        spdlog::warn("no markets requested; nothing to record");
    } else {
        spdlog::info("requested {} market(s)", tickers.size());
    }

    spdlog::warn("collection is not implemented yet (M2 delivers the WebSocket slice)");
    return 0;
}
