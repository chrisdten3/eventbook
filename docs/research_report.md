# EventBook research report

> **Status: no results exist yet.** This is a skeleton created at M0. Every
> quantitative claim below is a placeholder written as `[TBD]` and must not be
> filled in until a saved experiment configuration and output support it.

## Abstract

`[TBD]`

## 1. Introduction

Kalshi binary event contracts settle at $1 for the winning outcome and $0 for the
losing one. That bounded payoff changes the microstructure relative to equities:
price is a probability, the tick grid is defined per market by `price_ranges`,
and behaviour near $0.01 and $0.99 is not comparable to behaviour near $0.50.

This report asks two connected questions:

1. Do order-book imbalance, microprice, and signed trade flow predict
   short-horizon midpoint changes?
2. Does a signal- and liquidity-aware execution policy improve implementation
   shortfall relative to immediate, TWAP, and passive-then-cross baselines?

## 2. Data

- Universe and inclusion rule: `[TBD]`
- Collection window: `[TBD]`
- Contracts / events: `[TBD]`
- Valid order-book updates: `[TBD]`
- Public trades: `[TBD]`

### 2.1 Data quality

| Metric | Value |
|---|---|
| Connections / reconnects | `[TBD]` |
| Sequence gaps | `[TBD]` |
| Snapshot refreshes | `[TBD]` |
| Parse failures | `[TBD]` |
| Invalid deltas | `[TBD]` |
| Journal write failures | `[TBD]` |
| Dropped messages | `[TBD]` |
| Time with invalid book | `[TBD]` |
| Observed exchange-to-local timestamp difference (median / p99) | `[TBD]` |

Any session containing an unhandled drop is excluded from research and the
exclusion is recorded here.

## 3. Method

See [experiment_protocol.md](experiment_protocol.md), which was frozen before
collection began.

## 4. Descriptive microstructure

`[TBD]` - spread, depth, volatility, and activity by time-to-close and price
bucket.

## 5. Price formation results

`[TBD]` - baselines, logistic regression, calibration, markout, and
event-clustered confidence intervals. Null findings reported alongside positive
ones.

## 6. Execution results

`[TBD]` - implementation shortfall in ticks and cents per contract, VWAP, fill
rate, completion rate, time to completion, fees, markout, and cost tails, broken
out by target size, horizon, spread, volatility, and queue assumption.

## 7. Limitations

These apply regardless of what the results turn out to be:

- The order book is **aggregated L2**. It does not expose order ids or exact
  time priority, so queue position is *estimated*, never observed.
- Simulated orders replay against a historical path that **does not react** to
  them. This is displayed-depth consumption, not true market-impact simulation.
- Fills are **simulated**. No order was ever sent to the exchange.
- Overlapping simulated tasks are correlated; inference clusters at the
  market-event or market-day level rather than treating tasks as independent.
- Results describe an **observed association** over one recurring series in one
  time window. They are not a profitability claim.

## 8. Reproduction

`[TBD]` - exact commands from raw fixture to final table.

## References

- Kalshi API documentation: https://docs.kalshi.com/
