# AGENTS.md

## Project identity

This repository is **EventBook**, a C++20 market-microstructure and execution-research platform for Kalshi binary event contracts.

The project is being built primarily as a learning project. The user should finish it with stronger knowledge of:

- Modern C++ and the C++ build/tooling ecosystem.
- Asynchronous HTTP and WebSocket programming.
- Event-driven systems and deterministic replay.
- Electronic limit-order books and prediction-market mechanics.
- Market microstructure research.
- Transaction-cost analysis and execution algorithms.
- Testing, measurement, and defensible empirical claims.

The target portfolio description is:

> Built a C++20 event-driven platform that records and reconstructs Kalshi L2 order books, measures short-horizon price formation, and compares execution strategies under explicit latency, fee, partial-fill, and queue-position assumptions.

This is not initially a live-trading bot. The first complete product is a reliable data recorder, replay engine, research pipeline, and historical execution simulator.

## Agent operating mode

The user intends to work through the project and learn with the agent. Optimize for understanding and durable engineering skill, not merely for producing code quickly.

### Teaching contract

When helping with this repository, the agent must:

1. Explain the purpose of a component before implementing it.
2. Identify the C++ concepts the task exercises.
3. Break work into the smallest useful vertical slice.
4. Let the user attempt meaningful pieces when they are learning a concept.
5. Review the user's attempt concretely before replacing it.
6. Explain compiler, linker, runtime, protocol, and data-quality failures in plain language.
7. Show important invariants and failure cases, not only the happy path.
8. Write or update tests with production code.
9. Verify behavior with commands and report the actual results.
10. Distinguish observed facts, modeling assumptions, and inferences.

Do not dump a large finished subsystem without explanation unless the user explicitly asks for end-to-end implementation. Prefer this sequence:

1. Define the behavior and acceptance criteria.
2. Sketch the relevant types and interfaces.
3. Explain the new C++ or market concept.
4. Implement one small slice.
5. Compile and test it.
6. Review what was learned.
7. Continue to the next slice.

When there are multiple reasonable designs, present at most three, recommend one, and state the tradeoff. Avoid asking questions whose answers do not materially change the implementation.

### Do not hide C++ behind libraries

Libraries should handle protocol plumbing, cryptography, compression, and serialization. Core project logic should remain visible in the repository.

The user should personally understand and be able to explain:

- Ownership and lifetime of the main objects.
- Value semantics, references, pointers, and RAII.
- Why fixed-point integers are used for price and quantity.
- How snapshots and deltas reconstruct an order book.
- How asynchronous callbacks or coroutines are sequenced.
- How sequence gaps and reconnects are handled.
- How deterministic replay is achieved.
- Why an L2 feed cannot reveal exact queue position.
- How each execution strategy makes decisions.
- Why the train/test split prevents leakage.

Do not introduce templates, metaprogramming, lock-free structures, custom allocators, or complex inheritance merely to make the code appear sophisticated.

## Product goal

The platform should answer two connected research questions.

### Price-formation question

> Do order-book imbalance, microprice, and signed trade flow predict the direction and magnitude of short-horizon price changes in electronic prediction markets?

### Execution question

> Can a simple signal- and liquidity-aware execution policy improve implementation shortfall or completion quality relative to immediate execution, aggressive TWAP, and passive-then-cross baselines?

The predictive study should support the execution study. Do not turn the repository into a generic forecasting project detached from execution.

## Scope

### Required version-one capabilities

1. Discover and describe eligible Kalshi markets through REST.
2. Authenticate and connect to the Kalshi WebSocket.
3. Record order-book snapshots, order-book deltas, public trades, and relevant lifecycle messages.
4. Preserve immutable raw events with exchange and local timestamps.
5. Reconstruct aggregated L2 books deterministically.
6. Detect invalid state and recover through a fresh snapshot.
7. Generate one-second research features.
8. Study short-horizon price formation with walk-forward validation.
9. Replay historical events through an execution simulator.
10. Compare four execution policies under multiple queue assumptions.
11. Produce reproducible result tables, figures, and a 5-10 page research report.

### Explicit non-goals for version one

- Trading real money.
- Profit claims or a production trading strategy.
- An Avellaneda-Stoikov market maker.
- Exact L3/order-level reconstruction.
- Cross-exchange arbitrage.
- Multi-asset portfolio optimization.
- Deep learning.
- Reinforcement learning.
- A graphical frontend.
- Kafka, Kubernetes, distributed microservices, or cloud-scale infrastructure.
- Nanosecond-latency claims.
- Lock-free queues or custom allocators before measurement shows a need.

Possible extensions belong in `docs/extensions.md`, not in the active milestone.

## External references and API rules

Use current official Kalshi documentation as the source of truth. The reference Python repository is useful for operational ideas but may use older API shapes.

Primary references:

- Kalshi API documentation: <https://docs.kalshi.com/>
- WebSocket quick start: <https://docs.kalshi.com/getting_started/quick_start_websockets>
- Order-book updates: <https://docs.kalshi.com/websockets/orderbook-updates>
- Public trades: <https://docs.kalshi.com/websockets/public-trades>
- Order-book interpretation: <https://docs.kalshi.com/getting_started/orderbook_responses>
- Order direction and YES-price convention: <https://docs.kalshi.com/getting_started/order_direction>
- Fixed-point migration: <https://docs.kalshi.com/getting_started/fixed_point_migration>
- Historical-data tiers: <https://docs.kalshi.com/getting_started/historical_data>
- Current order-book endpoint: <https://docs.kalshi.com/api-reference/market/get-market-orderbook>
- Current trade endpoint: <https://docs.kalshi.com/api-reference/market/get-trades>
- Rate limits: <https://docs.kalshi.com/getting_started/rate_limits>
- Current V2 order endpoint: <https://docs.kalshi.com/api-reference/orders/create-order-v2>
- Reference repository: <https://github.com/rodlaf/KalshiMarketMaker>

Before implementing API code, verify the current schema in the official documentation. API fields, hosts, price structures, quantity formats, and deprecation dates can change.

Never commit:

- API keys.
- Private keys.
- Authentication headers.
- Signed URLs.
- Account identifiers.
- Raw user fills or positions unless the user intentionally supplies sanitized fixtures.

Use environment variables or a local ignored configuration file. Tests must use fixtures and fake clients, never production credentials.

## Safety boundary

Version one is read-only with respect to the exchange.

No code path may submit, amend, cancel, or liquidate real orders. If an order-entry adapter is later added:

1. It must default to disabled.
2. Demo and production environments must be represented by distinct types or explicit configuration.
3. Production writes must require an unmistakable opt-in.
4. Tests must prove that default configuration cannot send a write request.
5. The user must explicitly authorize any live-trading work.

The execution simulator creates simulated orders only. Never describe simulated fills as real fills.

## Recommended technology

Use the smallest stack that supports the project:

- Language: C++20.
- Build: CMake and CMake Presets.
- Dependency manager: vcpkg unless the repository already uses another consistent tool.
- HTTP/WebSocket: Boost.Asio and Boost.Beast.
- TLS and RSA-PSS signing: OpenSSL.
- JSON: simdjson for high-volume parsing; nlohmann/json is acceptable for small configuration or tests.
- Logging: spdlog.
- Formatting: fmt.
- CLI: CLI11.
- Tests: Catch2.
- Compression: Zstandard.
- Configuration: YAML or TOML, with a single library chosen consistently.
- Derived research storage: Parquet when practical; CSV is acceptable for the first vertical slice.
- Analysis and plots: a small Python layer using Polars/pandas, NumPy, statsmodels/scikit-learn, and matplotlib/seaborn is allowed.

The system and simulator remain C++. Python is a research client, not a replacement for the C++ pipeline.

Do not add a dependency without stating:

- Why it is needed.
- What capability it replaces.
- Whether it appears in runtime, test, or research code.
- How it affects setup and portability.

## Intended repository structure

```text
eventbook/
|-- AGENTS.md
|-- CMakeLists.txt
|-- CMakePresets.json
|-- README.md
|-- LICENSE
|-- .gitignore
|-- config/
|   `-- example.yaml
|-- cmake/
|-- include/eventbook/
|   |-- api/
|   |-- book/
|   |-- common/
|   |-- data/
|   |-- execution/
|   |-- features/
|   |-- replay/
|   `-- research/
|-- src/
|-- apps/
|   |-- collect.cpp
|   |-- replay.cpp
|   |-- build_features.cpp
|   `-- simulate_execution.cpp
|-- tests/
|   |-- fixtures/
|   |-- unit/
|   `-- integration/
|-- research/
|   |-- notebooks/
|   `-- scripts/
|-- docs/
|   |-- architecture.md
|   |-- data_dictionary.md
|   |-- experiment_protocol.md
|   |-- research_report.md
|   `-- extensions.md
|-- data/
|   |-- raw/
|   `-- derived/
`-- results/
```

`data/raw`, `data/derived`, and large generated results must be ignored by Git. Include only small, sanitized fixtures needed for tests and reproducibility.

## Architecture principles

### One event model, two data sources

Live collection and offline replay must feed the same normalized event types into the same book and feature logic.

The architecture should conceptually be:

```text
Kalshi REST/WebSocket -> raw journal -> parser/normalizer -> event dispatcher
                                                            |-> book builder
                                                            |-> trade state
                                                            |-> feature engine
                                                            `-> execution simulator

raw journal ----------> deterministic replay --------------^
```

Do not implement separate live and historical versions of order-book logic.

### Determinism before concurrency

Use a single ordered event-processing path for each replay. Networking and file writing may be asynchronous, but market-state mutations should initially occur on one deterministic event loop.

Do not create one thread per market. Do not parallelize book mutations until correctness and deterministic replay are established.

### Raw data is immutable

Record the received payload before applying transformations. A parsing or feature bug must be fixable by replaying the original journal without collecting the market again.

Each journal record should preserve at least:

- Local receive timestamp.
- Exchange timestamp when present.
- Connection/session identifier.
- Subscription or stream identifier when present.
- Sequence number when present.
- Message type.
- Market ticker and market ID when present.
- Raw payload.
- Schema/journal version.

The writer may buffer and compress records, but it must expose dropped-message and write-failure counters. Any session with an unhandled drop is invalid for research.

### Strong domain types

Do not pass monetary values and quantities around as unlabelled `double`, `int`, or `std::string` values.

Prefer strong value types such as:

```cpp
struct Price {
    std::int32_t units; // 1 unit = $0.0001
};

struct Quantity {
    std::int64_t units; // 1 unit = 0.01 contract
};

struct SequenceNumber {
    std::uint64_t value;
};
```

Names may evolve, but the representations and conversion rules must be documented and tested.

Use `std::chrono` types for durations and time points. Do not represent all time values as anonymous integers.

### Explicit interfaces

Prefer composition and small interfaces over deep inheritance. Likely boundaries include:

- `RestClient`
- `WebSocketSession`
- `EventJournalWriter`
- `EventJournalReader`
- `MessageNormalizer`
- `OrderBook`
- `MarketState`
- `FeatureEngine`
- `ReplayEngine`
- `ExecutionPolicy`
- `FillModel`
- `FeeModel`

Interfaces should be introduced when they enable testing or multiple implementations, not in anticipation of hypothetical future requirements.

## Prediction-market order-book conventions

Kalshi binary contracts settle to $1 for the winning outcome and $0 for the losing outcome.

The venue may expose YES bids and NO bids rather than a conventional bid/ask pair. On the legacy two-price representation:

```text
YES ask price = $1.00 - best NO bid price
```

When the WebSocket supports `use_yes_price: true`, subscribe with it explicitly and normalize NO-side levels onto the YES-price scale. Do not rely on a future default.

Every market's valid price grid is defined by its current `price_ranges`. Do not assume a one-cent tick or key behavior off a human-readable structure name.

Order-book state is aggregated by price level. It does not reveal:

- Individual order IDs.
- Exact time priority within a level.
- Whether a depth reduction was a cancellation ahead of or behind a simulated order.
- The counterfactual reaction to a simulated trade.

These limitations must appear in simulator documentation and the research report.

## Order-book model

The initial implementation should prioritize clarity and correctness. A suitable representation is:

```cpp
using BidLevels = std::map<Price, Quantity, std::greater<Price>>;
using AskLevels = std::map<Price, Quantity, std::less<Price>>;
```

An array or flatter representation may be benchmarked later because the price domain is bounded. Do not optimize the representation before a working benchmark exists.

The order book must support:

- Reset from snapshot.
- Apply positive or negative quantity delta.
- Remove zero levels.
- Return best bid and ask.
- Return depth over the first `k` levels.
- Validate price grid and quantity.
- Mark itself invalid after a sequence gap or malformed transition.
- Reject deltas before the first valid snapshot.
- Generate a stable state hash for replay testing.

Core invariants include:

- Quantities are nonnegative.
- Zero-quantity levels are absent.
- Prices are inside the market's valid domain and grid.
- A valid continuous book is not crossed.
- Sequence numbers are processed in order according to documented channel semantics.
- State is not considered valid between a detected gap and a fresh snapshot.

Locked, crossed, paused, or transitioning states should be investigated against lifecycle messages before being discarded as impossible.

## Event and data schema

Start with three layers.

### Raw layer

Append-only messages exactly as received, plus receipt metadata. Store as versioned JSONL, optionally compressed with Zstandard.

### Normalized event layer

Use a tagged union such as `std::variant` for types like:

- `BookSnapshot`
- `BookDelta`
- `PublicTrade`
- `MarketStatusChange`
- `MarketMetadataUpdate`
- `ConnectionStarted`
- `ConnectionLost`
- `GapDetected`

### Derived research layer

One row per market per second while the book is valid. Include:

- Market/event identifiers.
- Sample and exchange time.
- Best bid, best ask, midpoint, and spread.
- L1, L3, and L5 bid/ask depth.
- L1, L3, and L5 imbalance.
- Microprice and microprice-minus-midpoint.
- Trade counts and quantities over rolling windows.
- Signed trade-flow imbalance.
- Book-add and book-remove intensity.
- Realized volatility.
- Time to scheduled close.
- Market price bucket and distance from 0.50.
- Data-validity flags.
- Future labels generated in a separate leakage-safe pass.

Maintain `docs/data_dictionary.md` as fields are introduced.

## Feature definitions

For top-`k` bid and ask depth `B_k` and `A_k`:

```text
imbalance_k = (B_k - A_k) / (B_k + A_k)
```

Return missing when both depths are zero. Do not silently divide by an epsilon without documenting it.

For best bid `b`, best ask `a`, bid quantity `Q_b`, and ask quantity `Q_a`:

```text
microprice = (a * Q_b + b * Q_a) / (Q_b + Q_a)
```

Use signed public trade direction derived from the documented taker book/outcome fields. Test all side conversions with explicit fixtures.

Primary labels:

- Direction of the next midpoint change.
- Midpoint change after 1 second.
- Midpoint change after 5 seconds.
- Midpoint change after 30 seconds.

Do not generate labels inside the live feature engine. Labels depend on future information and belong in a separate offline pass.

## Research universe

Version one must study a homogeneous recurring family rather than mixing unrelated categories.

Use this process:

1. Run a seven-day scout using predefined standard-binary filters.
2. Exclude multivariate/combo markets and unsupported price structures only when technically necessary.
3. Compare recurring series by number of contracts, update rate, trade rate, spread, visible depth, and time with a valid two-sided book.
4. Choose one recurring series and freeze the inclusion rule before final analysis.
5. Capture every eligible market in that series rather than selecting only favorable periods.

Target data gate:

- At least 10 trading days.
- At least 20 completed contracts or market events.
- At least several hundred thousand valid order-book updates.
- Enough book/trade activity for meaningful execution tasks.

If multiple sibling threshold contracts belong to one event, treat the event/date—not the individual ticker—as the independent split unit.

## Research protocol

### Primary hypothesis

Order-book imbalance and microprice displacement contain incremental information about the next midpoint move beyond the current midpoint and spread.

### Model sequence

Start with interpretable baselines:

1. No-information/base-rate predictor.
2. Sign of L1 imbalance.
3. Microprice displacement alone.
4. Logistic regression using imbalance, microprice, trade flow, spread, volatility, price bucket, and time-to-close.

Do not add gradient boosting or neural networks until the baselines, validation, and leakage checks are correct.

### Validation

Use chronological event-level splits:

- Earliest 60% of events: train.
- Next 20%: validation and threshold selection.
- Final 20%: untouched test.

Never randomly split individual one-second rows. Keep sibling contracts from the same underlying event in the same partition.

Report:

- Sample counts and class balance.
- Accuracy conditional on a midpoint move.
- Log loss.
- ROC-AUC where appropriate.
- Calibration.
- 1-, 5-, and 30-second markout.
- Confidence intervals clustered or bootstrapped at the market-event level.
- Performance by spread, volatility, price, liquidity, and time-to-close bucket.

Predefine the final test before examining final-test performance. Record experiment configuration and code revision with each result.

### Research integrity

Do not:

- Tune on the test set.
- Treat millions of correlated rows as millions of independent observations.
- Remove difficult periods without a predefined data-quality rule.
- Describe association as causal market impact.
- Report only the best hyperparameter or market subset.
- Claim alpha from classification accuracy alone.

## Execution simulator

The simulator consumes the same ordered events as replay. It must be event-driven rather than candle-based.

### Execution tasks

Create both buy and sell tasks at eligible historical starting points.

Initial task grid:

- Horizons: 60 seconds and 300 seconds.
- Target sizes: 1%, 5%, and 10% of visible opposing top-five depth.
- Configurable decision interval and simulated latency.
- A cap that prevents a simulated child order from dominating displayed depth.

Overlapping simulated tasks are allowed as independent counterfactual replays, but statistical inference must account for correlation by clustering at market-event or market-day level.

### Required policies

1. **Immediate aggressive**: cross the displayed book at task arrival.
2. **Aggressive TWAP**: split quantity into equal time slices and cross on schedule.
3. **Passive then cross**: rest at the best price, update according to its stated rule, and aggressively complete at the deadline.
4. **Signal and urgency aware**: choose passive or aggressive behavior using spread, predicted adverse movement, volatility, remaining quantity, remaining time, and completion risk.

Every policy decision must use only information available at that simulated timestamp.

### Aggressive fills

Aggressive simulated orders consume visible opposing depth level by level after configured latency. Support partial completion when insufficient displayed depth exists.

The historical market path does not react to the simulated order. Describe this as historical replay with displayed-depth consumption, not as true counterfactual market-impact simulation.

### Passive fills and queue uncertainty

L2 data does not reveal exact queue priority. Implement at least these fill models:

1. **Conservative**: only qualifying observed trades reduce quantity ahead.
2. **Proportional**: cancellations at the level reduce quantity ahead in proportion to the estimated share ahead.
3. **Optimistic**: qualifying cancellations reduce quantity ahead before quantity behind.

Initialize queue ahead from visible quantity at order-acceptance time. Clearly document rules for:

- Same-price trades.
- Price improvement.
- Price movement through the simulated order.
- Partial fills.
- Cancels and replaces.
- Reconnection or invalid-book intervals.
- Market pause and close.
- Deadline completion.

Headline results should be robust under conservative and proportional assumptions. Optimistic results alone are not sufficient.

### Fees

Implement fees behind a `FeeModel` interface because Kalshi fees can vary by market and over time. Version all fee assumptions. If exact fees are unavailable, report both zero-fee and documented-fee scenarios rather than embedding an unexplained constant.

### Metrics

For a buy program:

```text
implementation_shortfall = execution_vwap - arrival_midpoint + fees / filled_quantity
```

Reverse the price sign for a sell program.

Report:

- Implementation shortfall in ticks and cents per contract.
- VWAP.
- Fill rate.
- Completion rate.
- Time to completion.
- Fees.
- 1-, 5-, and 30-second post-fill markout.
- Execution-cost variance and tail percentiles.
- Results by target size, horizon, spread, volatility, and queue assumption.

Do not use basis points as the only metric because relative costs near $0.01 and $0.99 can be misleading.

## Testing requirements

Every core bug fix should add a regression test. Avoid tests that merely reproduce implementation details.

### Unit tests

- Fixed-point parsing, formatting, arithmetic, overflow, and invalid input.
- YES/NO and bid/ask conversion.
- Market price-grid validation.
- Snapshot loading.
- Positive and negative delta application.
- Zero-level removal.
- Best-price and depth calculations.
- Imbalance and microprice.
- Sequence-gap detection.
- Feature rolling windows.
- Aggressive multi-level fills.
- Passive queue progression under each fill model.
- Implementation-shortfall sign conventions.

### Property/invariant tests

- Quantities never become negative.
- Prices remain in valid bounds.
- Applying the same valid event log twice produces the same final state hash.
- A book is invalid before its first snapshot and after a gap.
- An execution policy never fills more than its target.
- Filled quantity plus remaining quantity equals target quantity.
- A policy cannot access events after its current simulated time.

### Integration tests

- Replay a small sanitized snapshot/delta/trade fixture.
- Force a disconnect and verify resubscription/resnapshot behavior with a fake server or scripted client.
- Run feature generation on a fixture and compare an approved output.
- Run all four execution policies on a known scenario.

### Live smoke tests

Live tests must be opt-in, read-only, short, and clearly labeled. They should verify schema compatibility without requiring order permissions.

## Data-quality requirements

Track and report:

- Connections and reconnects.
- Messages by type.
- Sequence gaps.
- Snapshot refreshes.
- Parse failures.
- Invalid deltas.
- Journal write failures.
- Dropped messages.
- Time spent with an invalid book.
- Exchange-to-local timestamp difference, labeled as an observed timestamp difference rather than one-way latency.

Do not silently discard malformed messages. Preserve the raw payload when safe, increment a counter, and mark affected state invalid when correctness is uncertain.

## Performance work

Correctness precedes optimization.

Before optimizing:

1. Create a reproducible benchmark.
2. Measure wall time, CPU, allocations, and throughput.
3. Identify the actual bottleneck.
4. Change one dimension at a time.
5. Verify identical behavior after the change.

Useful eventual benchmarks include:

- JSON messages parsed per second.
- Book deltas applied per second.
- Replay events per second.
- Feature rows produced per second.
- Memory per active market.
- Journal compression ratio and write throughput.

Do not claim the project is low latency merely because it is written in C++.

## C++ style and engineering standards

- Prefer RAII and automatic storage duration.
- Prefer values, references, `std::unique_ptr`, and `std::optional` over raw owning pointers.
- Raw pointers may be used only as non-owning views with clear lifetime guarantees.
- Use `const` aggressively where it communicates intent.
- Prefer scoped enums.
- Use `std::expected` if available in the chosen toolchain; otherwise use a consistent explicit result/error type for recoverable failures.
- Do not use exceptions for normal market-data conditions such as an empty side of book.
- Exceptions are acceptable for construction failures or irrecoverable setup errors when handled at an application boundary.
- Avoid global mutable state.
- Avoid macros except include guards and necessary library/platform integration.
- Keep public headers minimal.
- Include what a file uses.
- Use descriptive domain names over single-letter names outside formulas and short loops.
- Keep formatting automated and consistent.
- Treat compiler warnings as errors in project code where practical.
- Use sanitizers in a development preset.

Recommended build presets eventually include:

- `dev`: debug symbols, warnings, address and undefined-behavior sanitizers.
- `release`: optimized reproducible build.
- `coverage`: test coverage if supported.

The exact commands must be documented in `README.md` once the build system exists. Agents must use the repository's documented commands rather than inventing parallel workflows.

## Git and change discipline

- Inspect the working tree before editing.
- Preserve unrelated user changes.
- Make focused changes that correspond to one milestone or issue.
- Do not perform destructive Git operations.
- Do not commit secrets or large data.
- Update documentation when behavior, schemas, assumptions, or commands change.
- Do not make or push commits unless the user asks.

A good change should normally contain:

- A clear behavior change.
- Tests.
- Documentation when necessary.
- Verification output.
- A short explanation of what the user should learn from it.

## Milestones and acceptance criteria

### M0: Repository and C++ foundation

Deliverables:

- CMake project builds locally.
- Dependency setup is reproducible.
- `eventbook_tests` runs one example test.
- Development sanitizer preset works.
- README contains build and test commands.

Learning focus:

- Translation units, headers, linking, CMake targets, compiler diagnostics, RAII, and unit testing.

### M1: Domain types and REST market discovery

Deliverables:

- Fixed-point `Price` and `Quantity` types.
- Timestamp and identifier types.
- Read-only REST client.
- Market metadata parsing, pagination, and predefined eligibility filters.
- Fixtures and tests for current API schemas.

Acceptance criteria:

- No floating-point canonical monetary representation.
- Pagination is tested.
- Unsupported markets are rejected for an explicit documented reason.

Learning focus:

- Strong types, parsing, error handling, HTTP, TLS, JSON, and dependency boundaries.

### M2: One-market WebSocket vertical slice

Deliverables:

- Authenticated WebSocket connection.
- Subscription to one market's order-book and trade streams.
- Snapshot and delta parsing.
- Live best bid/ask display.

Acceptance criteria:

- Run for at least 30 minutes.
- Receive snapshot, deltas, and trades when activity occurs.
- Detect a deliberately simulated sequence gap.
- No exchange write permissions are used.

Learning focus:

- Asynchronous control flow, ownership across callbacks/coroutines, TLS WebSockets, message framing, and sequence numbers.

### M3: Journal and deterministic replay

Deliverables:

- Versioned append-only journal.
- Compressed rotation.
- Journal reader.
- Replay engine using the same normalizer and book logic as live processing.
- Final-state hash.

Acceptance criteria:

- A 24-hour collection completes with zero unhandled drops.
- Replaying the same journal repeatedly yields the same state hash and metrics.
- Forced disconnect produces an explicit gap/recovery record.

Learning focus:

- Serialization, buffered I/O, producer/consumer boundaries, determinism, and recovery.

### M4: Feature dataset and descriptive research

Deliverables:

- One-second feature sampler.
- Data dictionary.
- Derived file export.
- Initial liquidity, spread, depth, volatility, and time-to-close plots.

Acceptance criteria:

- Feature calculations have fixture-based tests.
- Invalid-book intervals cannot generate normal feature rows.
- Raw and derived timestamps can be traced.

Learning focus:

- Rolling state, numerical care, dataset design, and descriptive microstructure.

### M5: Price-formation experiment

Deliverables:

- Frozen universe and split manifest.
- Leakage-safe label pass.
- Baselines and logistic regression.
- Evaluation tables and confidence intervals.

Acceptance criteria:

- Sibling markets stay in the same split.
- Test set remains untouched until the experiment is frozen.
- Results include failures and null findings.

Learning focus:

- Hypotheses, leakage, walk-forward validation, calibration, clustered uncertainty, and honest interpretation.

### M6: Execution simulator

Deliverables:

- Simulated order lifecycle.
- Aggressive depth sweeping.
- Passive queue models.
- Latency and fee models.
- Immediate, TWAP, and passive-then-cross baselines.

Acceptance criteria:

- All quantity/accounting invariants hold.
- Known fixtures produce hand-verifiable fills and costs.
- Queue assumptions are configurable and visible in results.

Learning focus:

- Execution mechanics, partial fills, queue uncertainty, implementation shortfall, and counterfactual limitations.

### M7: Adaptive execution and final study

Deliverables:

- Interpretable adaptive policy.
- Full task grid.
- Robustness by queue, fee, latency, size, horizon, and market regime.
- Final report.

Acceptance criteria:

- Adaptive decisions use only contemporaneous information.
- Results are compared against every baseline.
- Headline conclusion survives reasonable assumptions or is reported as inconclusive.

Learning focus:

- Turning signals into decisions, benchmark design, sensitivity analysis, and research communication.

### M8: Portfolio hardening

Deliverables:

- Reproducible commands from raw fixture to final result.
- Clean README and architecture document.
- Representative sample output.
- Benchmark table.
- Final résumé bullets using actual measured quantities.

Acceptance criteria:

- A new reader can build, test, replay a sample, and understand the research claims.
- No secret or proprietary data is required for the demonstration.

## Definition of done for an agent task

A task is not done merely because code was written. Before reporting completion, the agent should, as applicable:

1. Compile the affected targets.
2. Run focused tests.
3. Run the broader test suite when practical.
4. Run formatting or static checks configured by the repository.
5. Inspect relevant Git diff without altering unrelated changes.
6. State what was changed.
7. State what was verified and the exact outcome.
8. Call out assumptions, limitations, or remaining risks.
9. Explain the key C++ and market concepts the user should retain.

If a check cannot be run, state why. Never imply validation occurred when it did not.

## Agent response format during implementation

For substantial learning tasks, use this compact structure:

1. **Goal**: the behavior being added.
2. **Concepts**: the C++ and market concepts involved.
3. **Design**: key types, ownership, invariants, and failure modes.
4. **Implementation**: the focused change.
5. **Verification**: commands and observed results.
6. **Takeaways**: what the user should be able to explain afterward.
7. **Next slice**: the smallest logical continuation.

Do not over-format trivial answers. When the user is confused, use a concrete example or trace before introducing formal terminology.

## Review policy

When reviewing user-written code, prioritize:

1. Correctness and data integrity.
2. Ownership and lifetime safety.
3. Protocol/schema correctness.
4. Determinism.
5. Testability.
6. Clarity.
7. Performance only after measurement.

Separate feedback into:

- Must fix.
- Should improve.
- Optional extension.

Explain why each must-fix issue can change market state, simulated fills, or research results.

## Research claims and résumé rules

All public claims must be traceable to a saved experiment configuration and output.

Acceptable language:

- "Aggregated L2 order book."
- "Historical replay."
- "Simulated implementation shortfall."
- "Queue-position sensitivity analysis."
- "Observed short-horizon association."
- "Displayed-depth consumption."

Avoid unless directly supported:

- "Production low-latency trading system."
- "L3 order book."
- "True market impact."
- "Profitable strategy."
- "Live fills."
- "Guaranteed execution improvement."

Candidate résumé bullets should use actual values only:

- Built a C++20 asynchronous market-data system recording and deterministically replaying Kalshi WebSocket snapshots, L2 deltas, and public trades across `[N]` contracts, with fixed-point price handling and sequence-gap recovery.
- Reconstructed aggregated limit-order books and generated spread, depth, microprice, imbalance, trade-flow, volatility, and time-to-close features over `[N]` events and `[N]` messages.
- Evaluated short-horizon price formation using chronological event-level validation and market-event bootstrap confidence intervals.
- Developed an event-driven execution simulator supporting displayed-depth consumption, partial fills, configurable latency and fees, and multiple L2 queue-position assumptions.
- Compared immediate, TWAP, passive, and adaptive execution policies, changing simulated shortfall by `[X]` ticks at `[Y]%` completion under the stated conservative fill model.

Do not fill placeholders until results exist.

## Immediate first task

If the repository contains only this file, begin with M0. The first vertical slice after project setup is:

> Connect to one Kalshi market, record its snapshot/delta/trade messages, reconstruct the current book, and replay the captured session to the same final state.

Do not begin with market making, predictive modeling, or optimization. Everything later depends on trustworthy market data and deterministic book reconstruction.
