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

## Layer 2 - normalized events *(M2)*

`MarketEvent` is a `std::variant`. Every alternative below is produced by
`parse_ws_message`; connection lifecycle and gap events are generated locally
rather than parsed, and are registered when they are built.

| Alternative | Produced by | Carries |
|---|---|---|
| `BookSnapshot` | `orderbook_snapshot` | ticker, sid, seq, bids, asks |
| `BookDelta` | `orderbook_delta` | ticker, sid, seq, side, price, signed delta, exchange time |
| `PublicTrade` | `trade` | ticker, sid, trade id, YES price, quantity, taker side, block flag, exchange time |
| `SubscriptionAck` | `subscribed` | command id, channel, assigned sid |
| `StreamError` | `error` | command id, code, message, optional ticker |
| `UnhandledMessage` | any other `type` | the type string |

An unrecognized `type` is an event, not a failure. The venue adds channels over
time and a recorder that stopped on one would stop for no reason; the type is
counted while the raw payload is journalled intact.

### Everything is on the YES price scale

Kalshi publishes no bid/ask pair. It publishes **YES bids** and **NO bids**, and
a NO bid is economically an offer to sell YES: "buy NO at $0.30" and "sell YES
at $0.70" are the same order.

| Wire | Normalized |
|---|---|
| `yes_dollars_fp` entry | `BookSide::Bid` at the published price |
| `no_dollars_fp` entry | `BookSide::Ask` at `$1.00 − published` |
| delta `side: "yes"` | `BookSide::Bid` |
| delta `side: "no"` | `BookSide::Ask`, price reflected |
| taker bought `yes` | `TradeSide::BuyYes` |
| taker bought `no` | `TradeSide::SellYes` |

The conversion happens once, at the boundary. The book, feature engine, and
simulator all work in YES dollars and never rediscover the rule.

### The price convention is a required parameter

`use_yes_price: true` asks the venue to report NO-side levels already on the YES
scale. This is a **subscription-time decision the messages do not restate**, so
`parse_ws_message` must be told which is in force.

It cannot be inferred. `0.5400` is a legal price under either reading, so
nothing in a message reveals a mistake — and the mistake is not small. On the
documented snapshot example the best ask is **$0.44** under no-leg pricing and
**$0.54** under yes-leg pricing, from identical bytes.

The cheapest available check is that a correctly normalized two-sided book is
not crossed.

### Sequence numbers

`seq` is **per subscription** (`sid`), starts at 1, and increments by 1. Two
subscriptions on one connection carry two independent sequences, so gap
detection is keyed by `SubscriptionId` and never as a single counter for the
socket. Only `BookSnapshot` and `BookDelta` participate; `sequence_of` returns
`nullopt` for everything else so acks and errors cannot be misread as gaps.

### Confirmed against the live socket

`KXFED-27APR-T4.25` was subscribed twice at the same instant, once with each
setting. Field names are **identical** — `no_dollars_fp` either way — and only
the scale changes:

| `use_yes_price` | first `no_dollars_fp` entries |
|---|---|
| `false` | `0.0100`, `0.0200`, `0.0300`, `0.0600` |
| `true` | `0.9900`, `0.9800`, `0.9700`, `0.9400` |

Quantities are unchanged and the prices are exactly `1 − p`, so the two
encodings describe one book. A test asserts the normalizer produces byte-identical
bids and asks from both, and that the reconstructed top of book (`$0.16` /
`$0.35`) matches what `GET /markets` independently reported for the same market.

Misapplying the convention in **either** direction mirrors the ask side across
`$0.50` and drives the best ask below the best bid. That is why the
not-crossed check earns its place: it detects the error without needing to know
which convention was intended.

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

## Layer 3 - derived research layer *(M4)*

One row per market per sampling interval, default one second, produced by
`build_features` replaying a journal. Sampling is driven by **event
timestamps**, never a wall clock, so the dataset is a pure function of the
journal and rebuilding it after a bug fix costs seconds rather than another day
of collection.

Boundaries align to whole multiples of the interval since the epoch, not to the
first event, so two journals of the same market land on the same grid and can be
concatenated without resampling.

| Column | Type | Units | Notes |
|---|---|---|---|
| `market_ticker` | string | | |
| `sample_time_us` | int64 | µs since epoch | the interval boundary this row describes |
| `last_exchange_time_us` | int64 | µs since epoch | venue clock of the last event applied; empty when it carried none |
| `last_sequence` | uint64 | | traces the row to a journal record |
| `book_valid` | 0/1 | | **filter on this** |
| `best_bid`, `best_ask` | decimal | dollars | |
| `midpoint` | decimal | dollars | `(bid+ask)/2`; lands on a half tick |
| `spread` | decimal | dollars | `ask − bid` |
| `bid_depth_{1,3,5}` | decimal | contracts | sum over best k levels |
| `ask_depth_{1,3,5}` | decimal | contracts | |
| `imbalance_{1,3,5}` | float | dimensionless | `(B_k − A_k)/(B_k + A_k)`, in [−1, 1] |
| `microprice` | decimal | dollars | `(a·Q_b + b·Q_a)/(Q_b + Q_a)` |
| `microprice_displacement` | decimal | dollars | `microprice − midpoint` |
| `bid_levels`, `ask_levels` | int | | distinct price levels quoted |

### Rules this layer obeys

**Invalid intervals produce no normal rows.** A row is still emitted so the hole
is visible in the time series, but `book_valid` is 0 and every feature column is
**empty**. Empty, never zero or NaN — a fabricated zero is indistinguishable
from a measurement.

**Imbalance is missing when both sides are zero**, not divided by an epsilon.
AGENTS.md requires it, because a manufactured 0.0 would be indistinguishable
from a genuinely balanced book.

**Sampling happens before each event is applied**, so a row uses only events at
or before its boundary. Sampling afterwards would let information from after the
boundary into the row — a small leak, and precisely the kind M5's validity
depends on not having.

**No labels.** Anything depending on future information is produced in a
separate offline pass. A feature engine that can see the future is a leak
waiting to be discovered.

**Floating point appears only here.** Imbalance, midpoint, and microprice are
ratios and cannot be exact; every canonical price and size upstream stays an
exact integer.

### Trailing-window columns *(M4 slice 2)*

One group per configured window, default 10s and 60s, suffixed with the length.

| Column | Type | Units | Notes |
|---|---|---|---|
| `trades_Ws` | int | | executions in the window |
| `trade_volume_Ws` | decimal | contracts | total traded |
| `signed_trade_volume_Ws` | decimal | contracts | buying YES positive, selling YES negative |
| `trade_flow_imbalance_Ws` | float | dimensionless | signed / total, in [−1, 1]; **empty when nothing traded** |
| `book_adds_Ws` | int | | deltas that increased displayed size |
| `book_removes_Ws` | int | | deltas that decreased it |
| `realized_vol_Ws` | decimal | dollars | root sum of squared midpoint changes |

**Realized volatility uses absolute changes, not log returns.** A one-cent move
is a 69% log return at $0.01 and 2% at $0.50, so log returns would report
enormous volatility for every contract near the settlement bounds purely as an
artifact of price level. AGENTS.md warns about exactly this for relative
measures on binary contracts.

It is measured across the **sampled series**, not per event, so a busy second
and a quiet second are comparable. Changes spanning an invalid interval are
excluded: a book that went away at $0.50 and came back at $0.90 must not
contribute one enormous jump.

**Trade flow is empty, not zero, when nothing traded** — most rows. A zero would
claim balanced flow where there was no flow at all.

### First dataset

Built from the M3 acceptance journal, `KXBTCD-26AUG2617-T78749.99`:

| | |
|---|---|
| rows | 82,797 (one per second over 82,796s) |
| messages replayed | 559,301 |
| rows on an invalid book | 4 (0.005%) — the DNS outage at 02:46 |
| build time | 5.45s |
| size | 15 MB CSV |

The 4 invalid rows correspond exactly to the four-second reconnect, which is the
intended relationship between a data-quality incident and the dataset.

Descriptive summary over the 82,793 valid rows:

| | |
|---|---|
| spread | median **$0.0100**, max $0.5400 |
| midpoint range | $0.0800 → $0.7050 |
| `imbalance_1` | median **+0.419**, mean +0.236 |
| `microprice_displacement` | median +$0.00257, sd $0.01294 |
| `realized_vol_60s` | median $0.01658, p99 $0.32218 |
| `trades_60s` | median **0**, max 13 |
| book churn per 60s | median **290** |
| rows with any trade in 60s | 26,376 (**31.9%**) |

Two facts that shape M5. The median minute contains **290 book updates and zero
trades**, so trade-flow features are undefined on 68% of rows while book
features are always available — any model using both must handle that asymmetry
rather than imputing zeros. And `imbalance_1` has a median of **+0.419**, a
persistent bid-side lean rather than a symmetric distribution around zero, so a
naive "positive imbalance predicts up" rule would be long almost always and
needs a baseline that accounts for it.
