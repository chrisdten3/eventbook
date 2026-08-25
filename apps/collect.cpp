// collect - records one Kalshi market's order book from the live WebSocket.
//
// M2 scope: connect, subscribe, reconstruct the book, and display live best
// bid/ask. The immutable journal arrives in M3; this build holds state in
// memory and reports data-quality counters at the end.
//
// Read-only, like every binary in version one. HttpMethod has no enumerator but
// Get and the session sends nothing except subscribe commands and protocol
// pongs, so no code path here can place, amend, or cancel an order.

#include <spdlog/spdlog.h>
#include <CLI/CLI.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <variant>
#include <vector>

#include "eventbook/api/beast_http_transport.hpp"
#include "eventbook/api/market.hpp"
#include "eventbook/api/rest_client.hpp"
#include "eventbook/api/signing.hpp"
#include "eventbook/api/ws_session.hpp"
#include "eventbook/book/order_book.hpp"
#include "eventbook/common/version.hpp"

namespace {

using namespace eventbook;

std::string format_optional_price(const std::optional<Price>& price) {
    return price.has_value() ? format_price(*price) : std::string{"--"};
}

/// Everything the run needs to report afterwards.
///
/// AGENTS.md requires per-session counts of gaps, snapshot refreshes, parse
/// failures, and time spent with an invalid book. A session whose drops are
/// unaccounted for is not usable for research, so these are first-class output
/// rather than log noise.
struct RunCounters {
    std::uint64_t snapshots{};
    std::uint64_t deltas{};
    std::uint64_t trades{};
    std::uint64_t gaps{};
    std::uint64_t rejected_deltas{};
    std::uint64_t crossed_observations{};
    std::chrono::microseconds invalid_time{0};
};

}  // namespace

int main(int argc, char** argv) {
    CLI::App app{"eventbook collect - read-only Kalshi order book recorder"};
    app.set_version_flag("--version", eventbook::describe_build());

    std::string ticker;
    app.add_option("-m,--market", ticker, "Market ticker to record")->required();

    std::int64_t seconds{1800};
    app.add_option("-d,--duration", seconds, "Seconds to record (0 runs until interrupted)");

    bool use_demo{false};
    app.add_flag("--demo", use_demo, "Use the demo environment");

    bool no_yes_price{false};
    app.add_flag("--no-yes-price", no_yes_price,
                 "Subscribe in no-leg pricing instead of the YES scale");

    std::int64_t report_every{10};
    app.add_option("--report-every", report_every, "Seconds between book displays");

    std::int64_t simulate_gap_after{0};
    app.add_option("--simulate-gap-after", simulate_gap_after,
                   "Skip a sequence number after N deltas, to prove gap detection. "
                   "Corrupts the local stream deliberately; never use while recording "
                   "data intended for research");

    std::string log_level{"info"};
    app.add_option("--log-level", log_level, "trace|debug|info|warn|error");

    CLI11_PARSE(app, argc, argv);

    spdlog::set_pattern("%H:%M:%S.%e %^%l%$ %v");
    spdlog::set_level(spdlog::level::from_str(log_level));
    spdlog::info("{}", eventbook::describe_build());

    auto signer = load_signer_from_environment();
    if (!signer) {
        spdlog::error("credentials unavailable: {}", to_string(signer.error().kind));
        if (signer.error().kind == CredentialErrorKind::KeyUnreadable) {
            spdlog::error("  {}", to_string(signer.error().key_error));
        }
        spdlog::error("set {} and {}", kKeyIdEnvironmentVariable, kKeyPathEnvironmentVariable);
        return 1;
    }

    const auto environment = use_demo ? KalshiEnvironment::Demo : KalshiEnvironment::Production;
    const MarketTicker market{ticker};

    // The price grid comes from the venue, never assumed: across 4,000 live
    // markets the tick is $0.0100 about half the time and $0.0010 the rest.
    // Without it the book cannot tell an impossible price from a legal one.
    BeastHttpTransport transport{std::chrono::seconds{20}};
    KalshiRestClient rest{transport, environment};
    std::vector<PriceRange> grid;
    if (const auto response = rest.get("/markets/" + ticker)) {
        if (const auto parsed = parse_market_response(response->body)) {
            grid = parsed->price_ranges;
        } else {
            spdlog::warn("could not parse market metadata: {} (field '{}')",
                         to_string(parsed.error().kind), parsed.error().field);
        }
    } else {
        spdlog::warn("could not fetch market metadata: {}", to_string(response.error().kind));
    }
    if (grid.empty()) {
        spdlog::warn("no price grid for {}; tick validation disabled, bounds still enforced",
                     ticker);
    } else {
        spdlog::info("price grid: {} band(s), tick {}", grid.size(),
                     format_price(Price{grid.front().step.units}));
    }

    WsSessionConfig config;
    config.environment = environment;
    config.subscription.market_ticker = market;
    config.subscription.channels = {"orderbook_delta", "trade"};
    config.subscription.use_yes_price = !no_yes_price;

    WebSocketSession session{config, *std::move(signer)};
    OrderBook book{market, grid};
    RunCounters counters;

    std::uint64_t delta_count{0};
    std::uint64_t sequence_shift{0};
    bool recovery_requested{false};
    std::optional<LocalTimestamp> invalid_since;

    const auto started = local_now();
    auto last_report = started;

    session.on_notice([](WsSessionNotice notice, std::string_view detail) {
        if (notice == WsSessionNotice::ParseFailure) {
            spdlog::warn("parse failure: {}", detail.substr(0, 200));
        } else if (detail.empty()) {
            spdlog::info("session: {}", to_string(notice));
        } else {
            spdlog::info("session: {} ({})", to_string(notice), detail);
        }
    });

    session.on_event([&](const MarketEvent& event) {
        const auto now = local_now();

        if (const auto* snapshot = std::get_if<BookSnapshot>(&event)) {
            ++counters.snapshots;
            // A snapshot re-bases the local sequence, so any deliberate shift
            // applied earlier is discarded with it.
            sequence_shift = 0;
            recovery_requested = false;
            if (const auto rejection = book.apply(*snapshot)) {
                spdlog::error("snapshot rejected: {}", to_string(*rejection));
            } else {
                spdlog::info("snapshot: {} bid level(s), {} ask level(s)",
                             book.level_count(BookSide::Bid), book.level_count(BookSide::Ask));
            }
        } else if (const auto* delta = std::get_if<BookDelta>(&event)) {
            ++counters.deltas;
            ++delta_count;

            BookDelta adjusted = *delta;
            // Deliberate corruption, entirely local: skip one sequence number so
            // the book sees a hole that the venue never sent. This proves gap
            // detection on real traffic without needing the venue to misbehave,
            // and every later delta keeps the same shift so the stream stays
            // self-consistent after the single induced break.
            if (simulate_gap_after > 0 &&
                delta_count == static_cast<std::uint64_t>(simulate_gap_after)) {
                sequence_shift = 1;
                spdlog::warn("injecting a simulated sequence gap after {} deltas", delta_count);
            }
            adjusted.sequence = SequenceNumber{delta->sequence.value + sequence_shift};

            if (const auto rejection = book.apply(adjusted)) {
                ++counters.rejected_deltas;
                if (*rejection == BookRejection::SequenceGap) {
                    ++counters.gaps;
                }
                spdlog::warn("delta rejected: {} (book now {})", to_string(*rejection),
                             to_string(book.status()));
                // A snapshot is the only way back, so ask for one immediately
                // rather than discarding deltas until something else happens to
                // reconnect us.
                if (invalidates_book(*rejection) && !recovery_requested) {
                    recovery_requested = true;
                    spdlog::warn("requesting a fresh snapshot");
                    session.reconnect();
                }
            }
        } else if (std::holds_alternative<PublicTrade>(event)) {
            ++counters.trades;
        }

        // Time spent with an unusable book is a data-quality figure AGENTS.md
        // asks for by name, so it is measured rather than estimated.
        if (!book.is_valid() && !invalid_since.has_value()) {
            invalid_since = now;
        } else if (book.is_valid() && invalid_since.has_value()) {
            counters.invalid_time += now - *invalid_since;
            invalid_since.reset();
        }
        if (book.is_crossed()) {
            ++counters.crossed_observations;
        }

        if (report_every > 0 && now - last_report >= std::chrono::seconds{report_every}) {
            last_report = now;
            spdlog::info("book {:<7} bid {:>7} x {:<10} ask {:>7} x {:<10} spread {}",
                         to_string(book.status()), format_optional_price(book.best_bid()),
                         format_quantity(book.depth(BookSide::Bid, 1)),
                         format_optional_price(book.best_ask()),
                         format_quantity(book.depth(BookSide::Ask, 1)),
                         book.spread().has_value() ? format_price(Price{book.spread()->units})
                                                   : std::string{"--"});
        }
    });

    std::thread deadline;
    if (seconds > 0) {
        spdlog::info("recording {} for {}s", ticker, seconds);
        deadline = std::thread{[&session, seconds] {
            std::this_thread::sleep_for(std::chrono::seconds{seconds});
            session.stop();
        }};
    } else {
        spdlog::info("recording {} until interrupted", ticker);
    }

    session.run();
    if (deadline.joinable()) {
        deadline.join();
    }

    if (invalid_since.has_value()) {
        counters.invalid_time += local_now() - *invalid_since;
    }

    const auto elapsed = local_now() - started;
    const double wall_seconds =
        std::chrono::duration_cast<std::chrono::duration<double>>(elapsed).count();
    const auto& stats = session.stats();

    spdlog::info("--- session report ---");
    spdlog::info("elapsed              {:.1f}s", wall_seconds);
    spdlog::info("connections          {}", stats.connections);
    spdlog::info("reconnects           {}", stats.reconnects);
    spdlog::info(
        "messages             {} ({:.1f}/s)", stats.messages_received,
        wall_seconds > 0 ? static_cast<double>(stats.messages_received) / wall_seconds : 0.0);
    spdlog::info("bytes                {}", stats.bytes_received);
    spdlog::info("snapshots            {}", counters.snapshots);
    spdlog::info("deltas               {}", counters.deltas);
    spdlog::info("trades               {}", counters.trades);
    spdlog::info("sequence gaps        {}", counters.gaps);
    spdlog::info("rejected deltas      {}", counters.rejected_deltas);
    spdlog::info("parse failures       {}", stats.parse_failures);
    spdlog::info("unhandled messages   {}", stats.unhandled_messages);
    spdlog::info("stream errors        {}", stats.stream_errors);
    spdlog::info("crossed observations {}", counters.crossed_observations);
    spdlog::info(
        "invalid book time    {:.3f}s",
        std::chrono::duration_cast<std::chrono::duration<double>>(counters.invalid_time).count());
    spdlog::info("final book           {} bid {} ask {}", to_string(book.status()),
                 format_optional_price(book.best_bid()), format_optional_price(book.best_ask()));

    // A session with unexplained drops is not usable for research, so say so
    // rather than leaving it to be noticed later.
    const bool clean = stats.parse_failures == 0 && counters.rejected_deltas == 0;
    spdlog::info("data quality         {}", clean ? "clean" : "DEGRADED - see counters above");
    return 0;
}
