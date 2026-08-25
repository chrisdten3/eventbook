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

### 2. Normalized event layer *(M2)*

A `std::variant` over `BookSnapshot`, `BookDelta`, `PublicTrade`,
`SubscriptionAck`, `StreamError`, and `UnhandledMessage`. Connection lifecycle
and gap events are generated locally rather than parsed, and join the variant
when the session is built.

A tagged union rather than a class hierarchy: the set of event types is closed
and known, dispatch is exhaustive and checkable at compile time, and events stay
cheap value types with no heap allocation or virtual dispatch.

Everything here is on the **YES price scale**. The venue publishes YES bids and
NO bids; a NO bid is an offer to sell YES, so it is reflected exactly once, at
this boundary. See [data_dictionary.md](data_dictionary.md).

### 2b. Order book *(M2)*

`OrderBook` consumes normalized events and knows nothing about JSON, WebSocket
framing, or REST market objects. That is what lets live collection and offline
replay share one implementation.

**Validity is binary and conservative.** A book is `AwaitingSnapshot` until its
first snapshot and again after anything casts doubt on its state. While invalid,
every derived quantity is unavailable rather than stale — a feature row computed
from a half-known book is worse than no row at all.

**Two kinds of rejection.** Some say the *message* was wrong and leave the book
untouched: a message for another market, or a delta arriving with no snapshot to
apply it to. Others say our *state* is wrong and invalidate: a sequence gap, a
repeated sequence, a price off the market's tick grid, or a delta removing more
size than the level holds. That last one is where `apply_delta` returning a
`Result` pays off — the arithmetic reports that the transition is impossible,
and the book decides that impossibility means desynchronization.

**A snapshot is the only way back.** It deliberately does not check continuity
with what came before, because recovering from a gap means accepting a
discontinuity by definition.

**A crossed book is reported, not rejected.** It should be impossible in a
continuous market, but AGENTS.md warns against discarding crossed, locked, or
transitioning states before checking them against lifecycle messages. Treating
it as fatal would destroy the evidence needed to explain it.

**`state_hash()` is the determinism handle.** FNV-1a over validity, sequence,
and every level, in `std::map` key order so nothing incidental leaks in. M3's
replay engine proves itself by replaying one event log twice and comparing.

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
