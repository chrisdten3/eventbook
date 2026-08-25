// collect - records one Kalshi market's order book from the live WebSocket.
//
// M2 scope: connect, subscribe, reconstruct the book, and display live best
// bid/ask. The immutable journal arrives in M3; this build holds state in
// memory and reports data-quality counters at the end.
//
// Read-only, like every binary in version one. HttpMethod has no enumerator but
// Get and the session sends nothing except subscribe commands and protocol
// pongs, so no code path here can place, amend, or cancel an order.

#include <fmt/format.h>
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
#include "eventbook/book/market_state.hpp"
#include "eventbook/book/order_book.hpp"
#include "eventbook/common/version.hpp"
#include "eventbook/data/journal.hpp"
#include "eventbook/replay/replay.hpp"

namespace {

using namespace eventbook;

std::string format_optional_price(const std::optional<Price>& price) {
    return price.has_value() ? format_price(*price) : std::string{"--"};
}

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

    std::string journal_dir;
    app.add_option("-j,--journal-dir", journal_dir,
                   "Directory to record an immutable journal into. Without it nothing is "
                   "written and the session cannot be replayed");

    std::int64_t rotate_minutes{60};
    app.add_option("--rotate-minutes", rotate_minutes, "Minutes before starting a new segment");

    bool no_compress{false};
    app.add_flag("--no-compress", no_compress, "Write plain JSONL instead of zstd");

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

    std::optional<JournalWriter> journal;
    if (!journal_dir.empty()) {
        JournalWriterConfig journal_config;
        journal_config.compress = !no_compress;
        journal_config.rotate_interval = std::chrono::minutes{rotate_minutes};
        auto opened = JournalWriter::create(journal_dir, "collect-" + ticker, journal_config);
        if (!opened) {
            spdlog::error("cannot open journal: {} ({})", to_string(opened.error().kind),
                          opened.error().detail);
            return 1;
        }
        journal = *std::move(opened);
        spdlog::info("journal: {}", journal->current_segment().string());
    }

    WebSocketSession session{config, *std::move(signer)};

    // The same type the replay engine drives. Sharing it is what makes a
    // replayed session comparable to a live one rather than merely similar:
    // there is exactly one implementation of what an event does to the book.
    MarketState state{market, grid};

    // The instant a message arrived, shared between the journal record and the
    // live book so the two cannot disagree.
    LocalTimestamp last_received_at{};

    // Written before any market data, so a journal is self-describing: replay
    // reads the price convention and grid from here rather than from a
    // configuration file that happens to be sitting next to it.
    if (journal) {
        SessionMetadata metadata;
        metadata.market_ticker = market;
        metadata.price_convention = session.price_convention();
        metadata.price_grid = grid;
        metadata.build = eventbook::describe_build();

        JournalRecord header;
        header.kind = JournalRecordKind::SessionStarted;
        header.local_receive_time = local_now();
        header.message_type = "session_started";
        header.market_ticker = market;
        header.payload = encode_session_metadata(metadata);
        if (const auto problem = journal->write(header)) {
            spdlog::error("journal write failed: {}", to_string(problem->kind));
        }
    }

    const auto journal_event = [&](JournalRecordKind kind, std::string_view type,
                                   std::string_view payload) {
        if (!journal) {
            return;
        }
        JournalRecord record;
        record.kind = kind;
        record.local_receive_time = local_now();
        record.connection_id = session.connection_id();
        record.message_type = std::string{type};
        record.market_ticker = market;
        record.payload = std::string{payload};
        if (const auto problem = journal->write(record)) {
            spdlog::error("journal write failed: {}", to_string(problem->kind));
        }
    };

    session.on_raw(
        [&](std::string_view payload, LocalTimestamp received_at, const MarketEvent* event) {
            // The event handler reuses this instant rather than reading the clock
            // again. on_raw fires immediately before on_event for the same message,
            // so this is the timestamp that lands in the journal -- and using it for
            // the live book too is what makes live and replayed metrics identical
            // rather than merely close.
            last_received_at = received_at;
            if (!journal) {
                return;
            }
            JournalRecord record;
            record.kind = JournalRecordKind::Message;
            record.local_receive_time = received_at;
            record.connection_id = session.connection_id();
            record.payload = std::string{payload};

            // Metadata is an index over the payload, never a replacement for it:
            // replay re-derives all of this by parsing the bytes.
            if (event != nullptr) {
                if (const auto ticker_of = market_ticker_of(*event)) {
                    record.market_ticker = *ticker_of;
                }
                record.sequence = sequence_of(*event);
                record.message_type = std::visit(
                    [](const auto& payload_kind) -> std::string {
                        using Kind = std::decay_t<decltype(payload_kind)>;
                        if constexpr (std::is_same_v<Kind, BookSnapshot>)
                            return "orderbook_snapshot";
                        else if constexpr (std::is_same_v<Kind, BookDelta>)
                            return "orderbook_delta";
                        else if constexpr (std::is_same_v<Kind, PublicTrade>)
                            return "trade";
                        else if constexpr (std::is_same_v<Kind, SubscriptionAck>)
                            return "subscribed";
                        else if constexpr (std::is_same_v<Kind, StreamError>)
                            return "error";
                        else
                            return "unhandled";
                    },
                    *event);
            } else {
                record.message_type = "unparsed";
            }
            if (const auto problem = journal->write(record)) {
                spdlog::error("journal write failed: {}", to_string(problem->kind));
            }
        });

    std::uint64_t delta_count{0};
    std::uint64_t sequence_shift{0};
    bool recovery_requested{false};

    const auto started = local_now();
    auto last_report = started;

    const auto report_if_due = [&](LocalTimestamp now) {
        if (report_every <= 0 || now - last_report < std::chrono::seconds{report_every}) {
            return;
        }
        last_report = now;
        const auto& book = state.book();
        spdlog::info(
            "book {:<7} bid {:>7} x {:<10} ask {:>7} x {:<10} spread {}", to_string(book.status()),
            format_optional_price(book.best_bid()), format_quantity(book.depth(BookSide::Bid, 1)),
            format_optional_price(book.best_ask()), format_quantity(book.depth(BookSide::Ask, 1)),
            book.spread().has_value() ? format_price(Price{book.spread()->units})
                                      : std::string{"--"});
    };

    session.on_notice([&](WsSessionNotice notice, std::string_view detail) {
        // A disconnection is journalled explicitly. Without it a reader cannot
        // distinguish a quiet market from a period where we were simply absent,
        // and those demand opposite conclusions.
        if (notice == WsSessionNotice::Disconnected) {
            journal_event(JournalRecordKind::ConnectionLost, "connection_lost", detail);
        }
        if (notice == WsSessionNotice::ParseFailure) {
            spdlog::warn("parse failure: {}", detail.substr(0, 200));
        } else if (detail.empty()) {
            spdlog::info("session: {}", to_string(notice));
        } else {
            spdlog::info("session: {} ({})", to_string(notice), detail);
        }
    });

    session.on_event([&](const MarketEvent& event) {
        // The timestamp recorded in the journal, not a fresh clock read. Replay
        // feeds MarketState exactly this value, so the two paths cannot
        // disagree about invalid_time or anything else derived from it.
        const auto now = last_received_at;

        if (const auto* snapshot = std::get_if<BookSnapshot>(&event)) {
            sequence_shift = 0;
            recovery_requested = false;
            (void)snapshot;
        } else if (const auto* delta = std::get_if<BookDelta>(&event)) {
            ++delta_count;
            if (simulate_gap_after > 0 &&
                delta_count == static_cast<std::uint64_t>(simulate_gap_after)) {
                // Deliberate corruption, entirely local: skip one sequence
                // number so the book sees a hole the venue never sent. Proves
                // gap detection against real traffic without needing the venue
                // to misbehave.
                sequence_shift = 1;
                spdlog::warn("injecting a simulated sequence gap after {} deltas", delta_count);
            }
            if (sequence_shift != 0) {
                BookDelta adjusted = *delta;
                adjusted.sequence = SequenceNumber{delta->sequence.value + sequence_shift};
                if (const auto rejection = state.apply(MarketEvent{adjusted}, now)) {
                    spdlog::warn("delta rejected: {} (book now {})", to_string(*rejection),
                                 to_string(state.book().status()));
                    if (invalidates_book(*rejection) && !recovery_requested) {
                        recovery_requested = true;
                        spdlog::warn("requesting a fresh snapshot");
                        session.reconnect();
                    }
                }
                report_if_due(now);
                return;
            }
        }

        if (const auto rejection = state.apply(event, now)) {
            spdlog::warn("delta rejected: {} (book now {})", to_string(*rejection),
                         to_string(state.book().status()));
            // A snapshot is the only way back, so ask for one immediately
            // rather than discarding deltas until something else reconnects us.
            if (invalidates_book(*rejection) && !recovery_requested) {
                recovery_requested = true;
                spdlog::warn("requesting a fresh snapshot");
                session.reconnect();
            }
        }
        report_if_due(now);
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

    const auto finished = local_now();
    state.finish(finished);

    if (journal) {
        // The closing record carries the counters, so a journal can be audited
        // for completeness without re-deriving them.
        JournalRecord footer;
        footer.kind = JournalRecordKind::SessionEnded;
        footer.local_receive_time = finished;
        footer.connection_id = session.connection_id();
        footer.message_type = "session_ended";
        footer.market_ticker = market;
        footer.payload = fmt::format(
            R"({{"messages":{},"snapshots":{},"deltas":{},"trades":{},"gaps":{},)"
            R"("rejected_deltas":{},"parse_failures":{},"final_state_hash":"{:016x}"}})",
            session.stats().messages_received, state.stats().snapshots, state.stats().deltas,
            state.stats().trades, state.stats().sequence_gaps, state.stats().rejected_deltas,
            session.stats().parse_failures, state.state_hash());
        if (const auto problem = journal->write(footer)) {
            spdlog::error("journal write failed: {}", to_string(problem->kind));
        }
        if (const auto problem = journal->flush()) {
            spdlog::error("journal flush failed: {}", to_string(problem->kind));
        }
    }

    const auto elapsed = finished - started;
    const double wall_seconds =
        std::chrono::duration_cast<std::chrono::duration<double>>(elapsed).count();
    const auto& stats = session.stats();
    const auto& market_stats = state.stats();

    spdlog::info("--- session report ---");
    spdlog::info("elapsed              {:.1f}s", wall_seconds);
    spdlog::info("connections          {}", stats.connections);
    spdlog::info("reconnects           {}", stats.reconnects);
    spdlog::info(
        "messages             {} ({:.1f}/s)", stats.messages_received,
        wall_seconds > 0 ? static_cast<double>(stats.messages_received) / wall_seconds : 0.0);
    spdlog::info("bytes                {}", stats.bytes_received);
    spdlog::info("snapshots            {}", market_stats.snapshots);
    spdlog::info("deltas               {}", market_stats.deltas);
    spdlog::info("trades               {}", market_stats.trades);
    spdlog::info("sequence gaps        {}", market_stats.sequence_gaps);
    spdlog::info("rejected deltas      {}", market_stats.rejected_deltas);
    spdlog::info("rejected snapshots   {}", market_stats.rejected_snapshots);
    spdlog::info("parse failures       {}", stats.parse_failures);
    spdlog::info("unhandled messages   {}", market_stats.unhandled_messages);
    spdlog::info("stream errors        {}", market_stats.stream_errors);
    spdlog::info("crossed observations {}", market_stats.crossed_observations);
    spdlog::info(
        "invalid book time    {:.3f}s",
        std::chrono::duration_cast<std::chrono::duration<double>>(market_stats.invalid_time)
            .count());
    spdlog::info("final book           {} bid {} ask {}", to_string(state.book().status()),
                 format_optional_price(state.book().best_bid()),
                 format_optional_price(state.book().best_ask()));
    spdlog::info("final state hash     {:016x}", state.state_hash());
    if (journal) {
        const auto& journal_stats = journal->stats();
        spdlog::info("journal records      {}", journal_stats.records_written);
        spdlog::info("journal segments     {}", journal_stats.segments_created);
        spdlog::info("journal uncompressed {} bytes", journal_stats.uncompressed_bytes);
        spdlog::info("journal on disk      {} bytes ({:.1f}x)", journal_stats.bytes_on_disk,
                     journal_stats.bytes_on_disk > 0
                         ? static_cast<double>(journal_stats.uncompressed_bytes) /
                               static_cast<double>(journal_stats.bytes_on_disk)
                         : 0.0);
        spdlog::info("journal drops        {}", journal_stats.dropped_records);
        spdlog::info("journal write errors {}", journal_stats.write_failures);
    }

    // A session with unexplained drops is not usable for research, so say so
    // rather than leaving it to be noticed later.
    const bool journal_clean =
        !journal || (journal->stats().dropped_records == 0 && journal->stats().write_failures == 0);
    const bool clean = stats.parse_failures == 0 && market_stats.rejected_deltas == 0 &&
                       market_stats.rejected_snapshots == 0 && journal_clean;
    spdlog::info("data quality         {}", clean ? "clean" : "DEGRADED - see counters above");
    return 0;
}
