# Backtester

C++20 event-driven market backtester with a paper-trading path and an optional **live** path that links against a separate ITCH feed-handler / order-book engine.

## Features

- Deterministic event queue ordered by `(timestamp, feed seq, det_id)`
- L2/L3-style simulated book (`SimBook`) with queue-aware paper fills
- Strategy interface + example strategies (`mid_cross`, `warmup_take`)
- Risk gate (position / hard PnL floor)
- Markdown run reports (PnL, fill quality, equity samples)
- Optional `live_trader` that consumes ITCH via an SPSC ring from the feed-handler project

## Layout

```
include/backtester/   public headers (engine, book, adapter, risk, …)
src/                  library implementation
strategies/           strategy implementations
tools/                paper_trade, live_main, synth_gen
tests/                GoogleTest suite
```

## Requirements

- CMake ≥ 3.20
- C++20 compiler (Apple Clang / GCC / Clang)
- GoogleTest (fetched automatically by CMake)

For `live_trader` only: a sibling (or configured) [feed handler](https://github.com/) project that provides the `feedhandler` CMake target and `include/feedhandler/` headers. On this machine that is typically the Desktop folder `order book engine`.

## Build (core)

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
# or: ./build/backtester_tests
```

Binaries from the core build:

| Target | Purpose |
|--------|---------|
| `backtester` | Main CLI runner |
| `paper_trade` | Paper-trading tool |
| `synth_gen` | Synthetic event generator |
| `backtester_tests` | Unit / parity tests |

## Build (live trader)

Enable with `-DBT_BUILD_LIVE=ON`. CMake auto-detects a sibling directory named `order book engine`; otherwise pass paths explicitly.

```bash
# Auto-detect ../order book engine
cmake -DBT_BUILD_LIVE=ON -S . -B build-live
cmake --build build-live --target live_trader

# Or point at the feed-handler source tree
cmake -DBT_BUILD_LIVE=ON \
  -DFH_DIR="/path/to/order book engine" \
  -S . -B build-live
cmake --build build-live --target live_trader

# Or link a prebuilt static library
cmake -DBT_BUILD_LIVE=ON \
  -DFH_INCLUDE_DIR="/path/to/order book engine/include" \
  -DFH_LIB="/path/to/order book engine/build/libfeedhandler.a" \
  -S . -B build-live
```

You do **not** need to run the feed-handler process side by side: `live_trader` links `libfeedhandler` into the same binary (ingest thread + engine thread).

### Run live on an ITCH file

Generate or reuse a sample (symbol defaults to `SYNTH` in the feed-handler’s `itch_gen`):

```bash
./build-live/live_trader \
  --symbol SYNTH \
  --file "/path/to/order book engine/data/sample.itch"
```

Useful flags:

```text
--strategy warmup_take|mid_cross   # default: warmup_take
--latency <ns>                     # default 50000
--max-pos <N>                      # default 100
--hard-floor <dollars>             # default -1000
--output report.md
--verbose
```

Notes:

- `warmup_take` — smoke-test strategy: after a short warmup, takes 1 lot at the ask (works on flat synthetic tape).
- `mid_cross` — rolling-mid cross; needs mid to move (flat `itch_gen` tape often produces **0** orders).

## Paper trade

```bash
cmake --build build --target paper_trade
./build/paper_trade --help   # see tool flags
```

## Design notes

See [DESIGN_NOTES.md](DESIGN_NOTES.md) for architecture details (clocks, look-ahead safety, parity harness, live vs sim).

## License

See repository metadata / project owner for licensing.
