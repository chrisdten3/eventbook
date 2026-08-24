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

| Package | Purpose | Scope | Added |
|---|---|---|---|
| `fmt` | Formatting | runtime | M0 |
| `spdlog` | Structured logging | runtime | M0 |
| `cli11` | Command-line parsing | runtime | M0 |
| `catch2` | Test framework | test | M0 |
| `openssl` | TLS 1.2/1.3 client | runtime | M1 |
| `boost-beast` | HTTP/1.1 messages over Asio | runtime | M1 |
| `nlohmann-json` | Market metadata parsing | runtime | M1 |

`openssl` is what makes an HTTPS conversation possible at all: Kalshi is
TLS-only and sends HSTS. It replaces writing a TLS stack, which is not a thing
anyone should do. It will also supply RSA-PSS SHA-256 signing when M2 needs
authenticated WebSocket access — REST market data is public and needs none.

`boost-beast` provides HTTP/1.1 framing (headers, chunked transfer, keep-alive)
and, in M2, WebSocket framing. It replaces a hand-written HTTP parser, which is
a well-known source of security bugs. libcurl would serve REST alone more
simply, but has no WebSocket client, so the project would end up carrying two
network stacks; Beast covers both against one Asio model. Verified that Kalshi
negotiates HTTP/1.1 cleanly, since Beast does not speak HTTP/2.

Both are **runtime-only** and neither appears in a public header —
`BeastHttpTransport` hides them behind a pimpl, so they are linked `PRIVATE`
and nothing downstream pays their compile cost. They do make the first vcpkg
configure noticeably slower.

`nlohmann-json` handles market metadata: a few thousand objects parsed once at
startup, where ergonomics matter more than throughput. It is deliberately *not*
the parser for the M2 WebSocket feed, which is high-volume and gets `simdjson`.
Two JSON libraries is a considered choice, not an oversight — AGENTS.md sets
exactly this split.

Planned, deliberately **not** installed yet so the build stays fast:
`simdjson` (high-volume parsing, M2), `zstd` (journal compression, M3), and a
YAML library (M1).

### Live smoke tests

`ctest` never touches the network. The live tests are built but not registered
with CTest, so an offline machine or a venue outage cannot turn into a red
build. They are read-only, send no credentials, and cannot issue a write. Run
them by hand:

```bash
./build/dev/bin/eventbook_live_tests
```

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
