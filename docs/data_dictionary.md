# Data dictionary

Status: **M0**. No datasets exist yet. This file is the contract that every
recorded and derived field must be registered in before it is used in analysis.
It is updated in the same change that introduces a field, never afterwards.

## Conventions

- **Money** is fixed-point. One `Price` unit = $0.0001. A YES contract settles
  at $1.00 (10,000 units) or $0.00.
- **Quantity** is fixed-point. One `Quantity` unit = 0.01 contract.
- Floating point is never the canonical representation of money or size. It
  appears only in derived statistics that are explicitly documented as such.
- **Times** are `std::chrono` types. Every record carries both a local receive
  timestamp and, when the venue supplies one, an exchange timestamp.
- The difference between them is reported as an **observed timestamp
  difference**, not as one-way latency: clock offset and one-way delay cannot be
  separated from a single direction of measurement.
- All book state is normalized onto the **YES price scale**. On the legacy
  two-price representation, `YES ask = $1.00 - best NO bid`.

## Layer 1 - raw journal record *(schema v0, not built yet)*

| Field | Type | Notes |
|---|---|---|
| `journal_version` | uint16 | Schema version of this record |
| `local_recv_ns` | int64 | Steady/system receive time, nanoseconds |
| `exchange_ts` | optional int64 | Venue timestamp when present |
| `connection_id` | uint64 | Identifies one WebSocket session |
| `stream_id` | optional string | Subscription/channel identifier |
| `sequence` | optional uint64 | Venue sequence number when present |
| `message_type` | string | As reported by the venue |
| `market_ticker` | optional string | |
| `market_id` | optional string | |
| `payload` | bytes | Exact bytes received, unmodified |

## Layer 2 - normalized events *(not built yet)*

Registered here as each variant alternative is implemented.

## Layer 3 - derived research rows *(not built yet)*

One row per market per second, emitted **only while the book is valid**.
Planned fields, grouped:

- **Identity**: event id, series, market ticker, sample time, exchange time.
- **Top of book**: best bid, best ask, midpoint, spread.
- **Depth**: bid/ask depth at L1, L3, L5.
- **Imbalance**: `imbalance_k = (B_k - A_k) / (B_k + A_k)` for k in {1,3,5};
  **missing, not zero**, when `B_k + A_k == 0`.
- **Microprice**: `(a * Q_b + b * Q_a) / (Q_b + Q_a)`, and microprice minus mid.
- **Trade flow**: counts and quantities over rolling windows, signed trade-flow
  imbalance derived from documented taker book/outcome fields.
- **Book intensity**: add and remove intensity.
- **Volatility**: realized volatility over rolling windows.
- **Context**: time to scheduled close, price bucket, distance from $0.50.
- **Validity flags**: book valid, gap seen in window, snapshot refreshed.

## Labels *(M5, separate offline pass)*

Labels use future information and are therefore **never** computed by the live
feature engine.

| Label | Definition |
|---|---|
| `dir_next` | Sign of the next midpoint change |
| `dmid_1s` | Midpoint change after 1 second |
| `dmid_5s` | Midpoint change after 5 seconds |
| `dmid_30s` | Midpoint change after 30 seconds |

## Known limitations of L2 data

Aggregated level-2 data does **not** reveal individual order ids, exact time
priority within a price level, whether a depth reduction happened ahead of or
behind a simulated order, or how the market would have reacted to a simulated
trade. Every simulator result inherits these limitations and must state them.
