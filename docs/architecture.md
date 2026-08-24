# Architecture

Status: **M0**. This document describes the intended shape of the system and is
updated as each milestone lands. Sections marked *(not built yet)* describe
design intent, not existing code.

## The one rule that shapes everything

Live collection and offline replay feed **the same normalized events** into
**the same** book, feature, and simulation logic. There is no separate
"historical" order book. If replaying a journal produced different state than
the live run did, every research result downstream would be untrustworthy.

```text
Kalshi REST/WebSocket -> raw journal -> parser/normalizer -> event dispatcher
                                                            |-> book builder
                                                            |-> trade state
                                                            |-> feature engine
                                                            `-> execution simulator

raw journal ----------> deterministic replay --------------^
```

## Layers

### 1. Raw layer (immutable)

Every payload is written to an append-only journal *before* anything interprets
it, together with local receive time, exchange time, connection id, sequence
number, message type, ticker, and a schema version.

The point is recoverability: a parser bug or a feature bug is fixed by replaying
the journal, never by re-collecting a market that has already closed.

### 2. Normalized event layer *(not built yet)*

A `std::variant` over `BookSnapshot`, `BookDelta`, `PublicTrade`,
`MarketStatusChange`, `MarketMetadataUpdate`, `ConnectionStarted`,
`ConnectionLost`, and `GapDetected`.

A tagged union rather than a class hierarchy: the set of event types is closed
and known, dispatch is exhaustive and checkable at compile time, and events stay
cheap value types with no heap allocation or virtual dispatch.

### 3. Derived research layer *(not built yet)*

One row per market per second while the book is valid. Fields are catalogued in
[data_dictionary.md](data_dictionary.md). Labels depend on future information and
are produced in a **separate offline pass**, never by the live feature engine.

## Determinism before concurrency

Market-state mutation happens on a single ordered event loop per replay.
Networking and journal writing may be asynchronous; book mutation is not.
No thread-per-market, and no parallel book mutation until deterministic replay
is proven correct and a benchmark shows a real need.

The test of determinism is a **state hash**: replaying one journal twice must
produce byte-identical final state and identical metrics.

## Module map

| Directory | Responsibility | Milestone |
|---|---|---|
| `include/eventbook/common/` | Value types, time, errors, build identity | M0 / M1 |
| `include/eventbook/api/` | REST client, WebSocket session, auth signing | M1 / M2 |
| `include/eventbook/data/` | Journal writer and reader, framing, compression | M3 |
| `include/eventbook/book/` | Order book, sequence tracking, validity state | M2 / M3 |
| `include/eventbook/replay/` | Journal-driven deterministic replay engine | M3 |
| `include/eventbook/features/` | One-second sampler and feature computation | M4 |
| `include/eventbook/research/` | Label generation, split manifests, evaluation | M5 |
| `include/eventbook/execution/` | Simulated orders, fill models, fees, policies | M6 / M7 |

All logic lives in the `eventbook_core` library. `apps/` contains argument
parsing and wiring only, so every code path is reachable from tests.

## Ownership model

- `eventbook_core` is a static library; applications and tests link it.
- Values and `std::unique_ptr` for ownership; raw pointers only as non-owning
  views with a documented lifetime.
- Interfaces (`RestClient`, `FillModel`, `ExecutionPolicy`, ...) are introduced
  when a second implementation or a test double actually needs them.

## Safety boundary

Version one is **read-only with respect to the exchange**. No code path may
submit, amend, cancel, or liquidate an order. The execution simulator produces
*simulated* fills against recorded historical depth; those are never described
as real fills.
