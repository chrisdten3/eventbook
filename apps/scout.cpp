// scout - describes the Kalshi market universe through the read-only REST API.
//
// This is M1's deliverable made observable: discover markets, apply the frozen
// inclusion rule, and report both what survived and what did not. The rejected
// half is the point. A universe you cannot account for is a universe someone
// can accuse you of having chosen to flatter a result.
//
// Read-only, like everything in version one. HttpMethod has no enumerator but
// Get, so this binary cannot place, amend, or cancel an order.

#include <spdlog/spdlog.h>
#include <CLI/CLI.hpp>

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include "eventbook/api/beast_http_transport.hpp"
#include "eventbook/api/eligibility.hpp"
#include "eventbook/api/market_query.hpp"
#include "eventbook/api/rest_client.hpp"
#include "eventbook/common/version.hpp"

namespace {

using namespace eventbook;

void report(const UniverseSelection& selection, const EligibilityCriteria& criteria) {
    spdlog::info("considered {} market(s)", selection.considered());
    spdlog::info("eligible   {} market(s) across {} event(s)", selection.eligible.size(),
                 markets_by_event(selection.eligible).size());

    if (selection.rejected.empty()) {
        return;
    }

    // Split the breakdown, because the two halves mean different things. A
    // structural exclusion says this software cannot represent the market; a
    // selection exclusion says we chose not to study it, and every one of those
    // has to be restated beside any result drawn from what remains.
    spdlog::info("excluded   {} market(s):", selection.rejected.size());
    for (const auto& [reason, count] : selection.rejection_breakdown()) {
        spdlog::info("  {:<10} {:>6}  {}", is_structural(reason) ? "structural" : "selection",
                     count, to_string(reason));
    }

    spdlog::info("selection rules in force (freeze these before analysis):");
    spdlog::info("  required_status         {}", criteria.required_status.has_value()
                                                     ? to_string(*criteria.required_status)
                                                     : std::string_view{"any"});
    spdlog::info("  minimum_time_to_close   {}s", criteria.minimum_time_to_close.count());
    spdlog::info("  minimum_open_interest   {} contract-hundredths",
                 criteria.minimum_open_interest.units);
}

void list_events(const UniverseSelection& selection, std::size_t limit) {
    const auto grouped = markets_by_event(selection.eligible);
    spdlog::info("eligible events ({} total, showing up to {}):", grouped.size(), limit);
    std::size_t shown = 0;
    for (const auto& [event, count] : grouped) {
        if (shown++ >= limit) {
            break;
        }
        spdlog::info("  {:<44} {:>4} market(s)", event.value, count);
    }
}

}  // namespace

int main(int argc, char** argv) {
    CLI::App app{"eventbook scout - describe the eligible Kalshi market universe (read-only)"};
    app.set_version_flag("--version", eventbook::describe_build());

    std::string series;
    app.add_option("-s,--series", series, "Restrict to one series ticker, e.g. KXFED");

    int page_limit{1000};
    app.add_option("--page-size", page_limit, "Markets per request (1-1000)")
        ->check(CLI::Range(1, 1000));

    std::size_t max_pages{50};
    app.add_option("--max-pages", max_pages, "Stop after this many pages");

    std::int64_t minimum_hours{1};
    app.add_option("--min-hours", minimum_hours,
                   "Exclude markets closing sooner than this many hours");

    std::int64_t minimum_open_interest{0};
    app.add_option("--min-open-interest", minimum_open_interest,
                   "Open-interest floor in contract-hundredths. Off by default: this is a "
                   "liquidity threshold and must be pre-registered, not tuned after seeing "
                   "results");

    bool use_demo{false};
    app.add_flag("--demo", use_demo, "Talk to the demo environment instead of production");

    std::size_t event_limit{20};
    app.add_option("--show-events", event_limit, "How many eligible events to list");

    std::string log_level{"info"};
    app.add_option("--log-level", log_level, "trace|debug|info|warn|error")->default_str("info");

    CLI11_PARSE(app, argc, argv);

    spdlog::set_pattern("%v");
    spdlog::set_level(spdlog::level::from_str(log_level));
    spdlog::info("{}", eventbook::describe_build());

    BeastHttpTransport transport{std::chrono::seconds{20}};
    KalshiRestClient client{transport,
                            use_demo ? KalshiEnvironment::Demo : KalshiEnvironment::Production};

    MarketQuery query;
    query.limit = page_limit;
    query.status = MarketStatusFilter::Open;
    if (!series.empty()) {
        query.series_ticker = SeriesTicker{series};
    }

    spdlog::info("querying {} ...", host_for(client.environment()));
    const auto markets = fetch_all_markets(client, query, max_pages);
    if (!markets) {
        spdlog::error("market discovery failed: {}", to_string(markets.error().kind));
        if (markets.error().kind == MarketQueryErrorKind::Rest) {
            spdlog::error("  rest: {} (status {})", to_string(markets.error().rest.kind),
                          markets.error().rest.status_code);
        }
        if (markets.error().kind == MarketQueryErrorKind::Parse) {
            spdlog::error("  parse: {} on field '{}'", to_string(markets.error().parse.kind),
                          markets.error().parse.field);
        }
        if (markets.error().kind == MarketQueryErrorKind::PageLimitReached) {
            // Observed: over 250,000 markets are open at once, so an unscoped
            // scout will always hit this. That is the correct outcome rather
            // than a limitation -- AGENTS.md directs version one at ONE
            // recurring series, and a truncated sweep of everything would be
            // both useless and quietly biased toward whatever the venue lists
            // first (which, at time of writing, is combo markets).
            spdlog::error("  narrow the sweep with --series, or raise --max-pages to cover it");
        }
        return 1;
    }

    EligibilityCriteria criteria;
    criteria.minimum_time_to_close = std::chrono::hours{minimum_hours};
    criteria.minimum_open_interest = Quantity{minimum_open_interest};

    const auto selection = select_universe(*markets, criteria, assume_exchange_clock(local_now()));

    report(selection, criteria);
    if (event_limit > 0 && !selection.eligible.empty()) {
        list_events(selection, event_limit);
    }
    return 0;
}
