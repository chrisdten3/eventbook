# Experiment protocol

Status: **M0**. Written before any data exists, which is the point: the rules
below are committed to Git *first* so that later choices cannot be quietly
tuned to flatter a result.

## Research questions

1. **Price formation.** Do order-book imbalance, microprice, and signed trade
   flow predict the direction and magnitude of short-horizon price changes in
   electronic prediction markets?
2. **Execution.** Can a simple signal- and liquidity-aware execution policy
   improve implementation shortfall or completion quality relative to immediate
   execution, aggressive TWAP, and passive-then-cross baselines?

The first study exists to serve the second.

## Primary hypothesis

Order-book imbalance and microprice displacement carry incremental information
about the next midpoint move beyond the current midpoint and spread.

## Universe selection

1. Run a seven-day scout using predefined standard-binary filters.
2. Exclude multivariate/combo markets and unsupported price structures only when
   technically necessary, and record the reason for each exclusion.
3. Compare recurring series on contract count, update rate, trade rate, spread,
   visible depth, and fraction of time with a valid two-sided book.
4. Choose **one** recurring series and **freeze the inclusion rule** before any
   final analysis.
5. Capture every eligible market in that series. Do not select favorable periods.

### Data gate

A study is not run until the frozen universe contains at least:

- 10 trading days,
- 20 completed contracts or market events,
- several hundred thousand valid order-book updates,
- enough book and trade activity to define meaningful execution tasks.

## Splits

Chronological, at the **event** level:

| Split | Share | Use |
|---|---|---|
| Train | earliest 60% | Model fitting |
| Validation | next 20% | Threshold and hyperparameter selection |
| Test | final 20% | Untouched until the experiment is frozen |

Rules:

- Never randomly split individual one-second rows; adjacent rows are strongly
  dependent and random splitting leaks the answer across the boundary.
- Sibling threshold contracts belonging to one underlying event stay in the
  **same** partition. The independent unit is the event/date, not the ticker.

## Model sequence

Interpretable baselines first, in this order:

1. No-information / base-rate predictor.
2. Sign of L1 imbalance.
3. Microprice displacement alone.
4. Logistic regression on imbalance, microprice, trade flow, spread,
   volatility, price bucket, and time-to-close.

Gradient boosting and neural networks are **not** permitted until the baselines,
the validation scheme, and the leakage checks are demonstrably correct.

## Reporting requirements

Every result reports:

- Sample counts and class balance.
- Accuracy **conditional on a midpoint move** (unconditional accuracy is
  dominated by the no-move class and is close to meaningless here).
- Log loss, and ROC-AUC where appropriate.
- Calibration.
- 1-, 5-, and 30-second markout.
- Confidence intervals clustered or bootstrapped **at the market-event level**.
- Breakdowns by spread, volatility, price, liquidity, and time-to-close bucket.

Each result records the experiment configuration and the code revision that
produced it.

## Execution study

Buy and sell tasks at eligible historical starting points.

- Horizons: 60 s and 300 s.
- Target sizes: 1%, 5%, 10% of visible opposing top-five depth.
- Configurable decision interval and simulated latency.
- A cap preventing a simulated child order from dominating displayed depth.

Policies: immediate aggressive, aggressive TWAP, passive-then-cross, and
signal/urgency aware. Every decision uses only information available at that
simulated timestamp.

Queue assumptions: conservative, proportional, and optimistic. **Headline
results must hold under conservative and proportional assumptions.** An
optimistic-only result is reported as not robust.

For a buy program:

```text
implementation_shortfall = execution_vwap - arrival_midpoint + fees / filled_quantity
```

with the price sign reversed for a sell program.

## Research integrity rules

Do not:

- Tune on the test set.
- Treat millions of correlated one-second rows as independent observations.
- Remove difficult periods without a **predefined** data-quality rule.
- Describe association as causal market impact.
- Report only the best hyperparameter or the best market subset.
- Claim alpha from classification accuracy alone.

Null and negative findings are reported with the same prominence as positive
ones.
