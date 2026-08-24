# Data dictionary

Status: **M1**. Market metadata is registered below; no recorded or derived
datasets exist yet. This file is the contract that every recorded and derived
field must be registered in before it is used in analysis. It is updated in the
same change that introduces a field, never afterwards.

## Conventions

- **Money** is fixed-point. One `Price` unit = $0.0001. A YES contract settles
  at $1.00 (10,000 units) or $0.00.
- **Quantity** is fixed-point. One `Quantity` unit = 0.01 contract.
- Floating point is never the canonical representation of money or size. It
  appears only in derived statistics that are explicitly documented as such.
- **Times** are `std::chrono` types at **microsecond** resolution. Not
  nanoseconds: the venue publishes milliseconds (`ts_ms`), so microseconds lose
  nothing the exchange actually sends, while nanoseconds would advertise
  measurement precision this system does not have.
- Every record carries both a local receive timestamp and, when the venue
  supplies one, an exchange timestamp. They come from unsynchronized clocks and
  are separate types (`LocalTimestamp`, `ExchangeTimestamp`) so that they cannot
  be subtracted casually.
- The difference between them is reported as an **observed timestamp
  difference**, not as one-way latency: clock offset and one-way delay cannot be
  separated from a single direction of measurement.
- All book state is normalized onto the **YES price scale**. On the legacy
  two-price representation, `YES ask = $1.00 - best NO bid`.

## Layer 0 - market metadata *(REST, M1)*

Parsed by `parse_market` from `GET /trade-api/v2/markets`. Unknown JSON fields
are ignored; the fields below are required unless marked optional.

| Field | Type | Wire field | Notes |
|---|---|---|---|
| `ticker` | `MarketTicker` | `ticker` | One tradeable contract |
| `event_ticker` | `EventTicker` | `event_ticker` | Partition unit for train/test splits |
| `market_type` | string | `market_type` | `binary` for standard contracts |
| `status` | `MarketStatus` | `status` | See status vocabulary below |
| `price_level_structure` | string | `price_level_structure` | `linear_cent`, `deci_cent`, ... |
| `price_ranges` | `vector<PriceRange>` | `price_ranges` | `{start, end, step}`; `step` is a `PriceDelta` |
| `open_time` | `ExchangeTimestamp` | `open_time` | RFC 3339 |
| `close_time` | `ExchangeTimestamp` | `close_time` | RFC 3339 |
| `expected_expiration_time` | optional | `expected_expiration_time` | Nullable on the wire |
| `mve_collection_ticker` | optional string | `mve_collection_ticker` | Present only on multivariate markets |
| `yes_bid` / `yes_ask` | `Price` | `yes_bid_dollars` / `yes_ask_dollars` | Snapshot, **not** book state |
| `yes_bid_size` / `yes_ask_size` | `Quantity` | `yes_bid_size_fp` / `yes_ask_size_fp` | Fractional: `"95.12"` is 9,512 units |
| `last_price` | `Price` | `last_price_dollars` | Snapshot |
| `volume` / `volume_24h` | `Quantity` | `volume_fp` / `volume_24h_fp` | |
| `open_interest` | `Quantity` | `open_interest_fp` | |
| `can_close_early` | bool | `can_close_early` | |

The quote fields are a **snapshot taken when the request was served**, not an
order book. They are for scouting a research universe by spread, depth, and
activity. Book state comes from the WebSocket in M2 and never from here.

### Status vocabulary

The `status` **field** and the `status` **query parameter** use different
vocabularies. Verified against the live API:

| Query `status=` | Field `status` values |
|---|---|
| `unopened` | `initialized` |
| `open` | `active` |
| `closed` | `closed`, `determined` |
| `settled` | `finalized` |

`MarketStatusFilter` models the query side and `MarketStatus` the field side, so
the two cannot be interchanged. An unrecognized field value parses to
`MarketStatus::Unknown` rather than failing, so a new venue state does not stop
a recorder; the eligibility filter rejects `Unknown` explicitly.

### Price grid

Tick size is per market and is read from `price_ranges`, never assumed.
Observed live: `linear_cent` steps by `$0.0100` (100 `Price` units) and
`deci_cent` by `$0.0010` (10 units).

## Inclusion rule *(M1)*

`EligibilityCriteria` is the frozen inclusion rule. It must be recorded with an
experiment configuration and reported with any result derived from it.

Exclusions divide in two, and the distinction governs reporting rather than
execution.

**Structural** — this software cannot faithfully represent the market. These
are facts about the code, not choices, and are not configurable.

| Reason | Rule |
|---|---|
| `Multivariate` | non-empty `mve_collection_ticker` |
| `NotBinaryMarket` | `market_type != "binary"` |
| `UnknownStatus` | `status` not recognized by this build |
| `NoPriceGrid` | `price_ranges` empty |
| `MalformedPriceGrid` | non-positive step, inverted band, or width not a whole number of ticks |
| `PriceGridOutOfBounds` | band outside `[$0.0000, $1.0000]` |
| `OverlappingPriceBands` | a price would sit on two tick grids at once |

**Selection** — a decision about scope. Every one of these must be
pre-registered and stated beside any finding.

| Reason | Default |
|---|---|
| `StatusNotRequested` | `required_status = Active` |
| `AlreadyClosed` | always applied |
| `ClosesTooSoon` | `minimum_time_to_close = 1 hour` |
| `InsufficientOpenInterest` | **0, i.e. disabled** |

The open-interest gate defaults to off deliberately. Liquidity correlates with
spread, depth, volatility, and the informativeness of order flow — everything
the study measures. Raising it after seeing results is cherry-picking, so using
it at all has to be a visible, justified act.

## Layer 1 - raw journal record *(schema v0, not built yet)*

| Field | Type | Notes |
|---|---|---|
| `journal_version` | uint16 | Schema version of this record |
| `local_recv_us` | int64 | Local wall-clock receive time, microseconds since epoch |
| `exchange_ts_us` | optional int64 | Venue timestamp, microseconds since epoch |
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
