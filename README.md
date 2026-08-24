# EventBook

A C++20 event-driven platform that records and reconstructs Kalshi L2 order
books, measures short-horizon price formation, and compares execution strategies
under explicit latency, fee, partial-fill, and queue-position assumptions.

> **Status: M0 - repository and C++ foundation.**
> The build, dependency, and test pipeline work end to end. No market data code
> exists yet. See [Milestones](#milestones).

**This project is read-only with respect to the exchange.** No code path
submits, amends, or cancels an order. The execution simulator produces
*simulated* fills replayed against recorded historical depth; they are never
real fills.

## Requirements

| Tool | Version used | Notes |
|---|---|---|
| CMake | 4.4.2 | 3.25+ required for the preset schema |
| Ninja | 1.13.2 | Generator for all presets |
| vcpkg | any recent | Dependency manager, manifest mode |
| C++ compiler | AppleClang 17 | Any C++20 compiler should work |

```bash
brew install cmake ninja
```

```bash
git clone https://github.com/microsoft/vcpkg.git ~/vcpkg && ~/vcpkg/bootstrap-vcpkg.sh -disableMetrics
```

`VCPKG_ROOT` must be set, because the presets read it to locate the toolchain
file. Add this to your shell profile:

```bash
echo 'export VCPKG_ROOT="$HOME/vcpkg"' >> ~/.zshrc
```

### macOS: stale Command Line Tools headers

If any C++ compile fails with `fatal error: 'algorithm' file not found`, your
Command Line Tools install has a leftover empty libc++ directory that shadows
the real SDK headers. Check it:

```bash
ls /Library/Developer/CommandLineTools/usr/include/c++/v1
```

If that lists only a handful of `__functional_03`-style files instead of ~185
headers, remove the stale directory so clang falls through to the SDK:

```bash
sudo mv /Library/Developer/CommandLineTools/usr/include/c++/v1 /Library/Developer/CommandLineTools/usr/include/c++/v1.stale
```

## Build and test

Dependencies are declared in [`vcpkg.json`](vcpkg.json) and installed
automatically on the first configure, so the first build takes a few minutes.

```bash
cmake --preset dev && cmake --build --preset dev && ctest --preset dev
```

Or as one workflow preset:

```bash
cmake --workflow --preset dev
```

### Presets

| Preset | Build type | What it is for |
|---|---|---|
| `dev` | Debug | Day-to-day work. ASan + UBSan, warnings as errors. **Use this by default.** |
| `release` | RelWithDebInfo | Benchmarks and any performance number that gets reported. |
| `coverage` | Debug | `llvm-cov` line coverage. |

Binaries land in `build/<preset>/bin/`.

```bash
./build/dev/bin/collect --version
```

### Running one test

`catch_discover_tests` registers each `TEST_CASE` with CTest individually:

```bash
ctest --preset dev -R "microprice" --output-on-failure
```

Or drive the Catch2 binary directly by tag:

```bash
./build/dev/bin/eventbook_tests "[book]"
```

## Layout

```text
include/eventbook/   public headers, one directory per subsystem
src/                 eventbook_core - all domain logic lives here
apps/                thin executables; argument parsing and wiring only
tests/               unit, property, and integration tests + small fixtures
docs/                architecture, data dictionary, protocol, report
config/              example.yaml; copy to config/local.yaml (git-ignored)
data/raw/            recorded journals (git-ignored)
data/derived/        generated feature datasets (git-ignored)
results/             generated tables and figures (git-ignored)
```

All logic lives in `eventbook_core` so that every code path is reachable from
tests. `apps/` stays thin on purpose.

## Configuration and credentials

Credentials are **never** committed and never read from a tracked file. They come
from the environment:

| Variable | Meaning |
|---|---|
| `EVENTBOOK_KALSHI_KEY_ID` | Kalshi API key id |
| `EVENTBOOK_KALSHI_KEY_PATH` | Path to the RSA private key used for request signing |

Everything else lives in a YAML file. Start from
[`config/example.yaml`](config/example.yaml):

```bash
cp config/example.yaml config/local.yaml
```

`config/local.yaml`, `.env`, `*.pem`, and `*.key` are all git-ignored.

## Dependencies

Added only when a milestone needs them, each with a stated reason.

| Package | Purpose | Added |
|---|---|---|
| `fmt` | Formatting | M0 |
| `spdlog` | Structured logging | M0 |
| `cli11` | Command-line parsing | M0 |
| `catch2` | Test framework | M0 |

Planned, deliberately **not** installed yet so the first build stays fast:
`openssl` (TLS + RSA-PSS request signing, M1), `boost-beast` (HTTP/WebSocket,
M1/M2), `simdjson` (high-volume parsing, M2), `zstd` (journal compression, M3),
and a YAML library (M1).

## Milestones

| # | Deliverable | Status |
|---|---|---|
| M0 | Repository and C++ foundation | **done** |
| M1 | Domain types and REST market discovery | next |
| M2 | One-market WebSocket vertical slice | |
| M3 | Journal and deterministic replay | |
| M4 | Feature dataset and descriptive research | |
| M5 | Price-formation experiment | |
| M6 | Execution simulator | |
| M7 | Adaptive execution and final study | |
| M8 | Portfolio hardening | |

The first vertical slice after setup: connect to one Kalshi market, record its
snapshot/delta/trade messages, reconstruct the current book, and replay the
captured session to the same final state.

## Documentation

- [Architecture](docs/architecture.md)
- [Data dictionary](docs/data_dictionary.md)
- [Experiment protocol](docs/experiment_protocol.md) - frozen before collection
- [Research report](docs/research_report.md) - skeleton, no results yet
- [Extensions](docs/extensions.md) - explicitly out of scope for v1
- [AGENTS.md](AGENTS.md) - working agreement for AI agents on this repository

## License

MIT. See [LICENSE](LICENSE).
