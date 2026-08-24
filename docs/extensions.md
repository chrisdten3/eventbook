# Extensions

Ideas that are explicitly **out of scope for version one**. Nothing here may be
started before M8 is complete. The list exists so that good ideas can be
recorded without derailing the milestone actually in progress.

## Explicit non-goals for v1

Trading real money; profit claims or a production trading strategy; an
Avellaneda-Stoikov market maker; exact L3/order-level reconstruction;
cross-exchange arbitrage; multi-asset portfolio optimization; deep learning;
reinforcement learning; a graphical frontend; Kafka, Kubernetes, or distributed
microservices; nanosecond-latency claims; lock-free queues or custom allocators
before measurement shows a need.

## Candidate extensions

### Engineering

- Flat array-backed order book benchmarked against the `std::map` baseline.
  *Requires a working benchmark first.* The price domain is bounded, so an array
  is plausible, but "plausible" is not a measurement.
- Parquet output for derived rows once CSV becomes a bottleneck.
- Multi-market parallel replay, only after single-threaded determinism is proven.
- A binary journal format if JSONL compression ratio or parse throughput is shown
  to be the bottleneck.

### Research

- Additional label horizons and volatility-scaled labels.
- Hazard modelling of time-to-fill for resting orders.
- Regime segmentation by time-to-close.
- Cross-series generalization: does a rule fit on one series transfer to another?
- Non-linear models, once the interpretable baselines are trustworthy.

### Execution

- Richer adaptive policies with explicit completion-risk penalties.
- Sensitivity of conclusions to latency distribution rather than a fixed latency.
- Order-entry adapter. **Gated**: default disabled, distinct types for demo and
  production, unmistakable opt-in for production writes, tests proving the
  default configuration cannot send a write request, and explicit authorization
  from the repository owner.
