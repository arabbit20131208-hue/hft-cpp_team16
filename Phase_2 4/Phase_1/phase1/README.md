Team 16

# Phase 1

Only files inside `phase1` are changed. The original Phase 0 source stays unchanged.

## Run

Run from the team repository root:

```bash
cmake -S phase1/cpp_client -B phase1/build_before -DPHASE1_FAST=OFF -DHFT_USE_TLS=ON
cmake --build phase1/build_before -j 4
cmake -S phase1/cpp_client -B phase1/build -DPHASE1_FAST=ON -DHFT_USE_TLS=ON
cmake --build phase1/build -j 4
python3 phase1/bench.py --before phase1/build_before/hft_bot --after phase1/build/hft_bot
```

The benchmark imports the original `scripts/latency_replay.py`. Both versions run the same 2,000 snapshots, with 50 warm-up ticks discarded. It checks their orders, prints p50/p99, and saves `results.json` and `tail_hist.png` next to this file. To use a session recording, add `--tape sessions/session_NAME.jsonl`.

## Implementation

The strategy keeps the Phase 0 mid-price momentum rule. Its symbol states and message slots are allocated before receiving ticks. The message arena resets after each tick. A SAX parser reads only the symbol, best bid, best ask and mid; it does not build a JSON DOM. A variant visitor selects the PlaceOrder or CancelOrder serializer, which writes into reusable buffers. `on_book` only decides and fills the preallocated message slots. Serialization and socket sending run immediately after it returns. LAT timing still includes both operations.

The control build uses the old DOM and map logic with timing added in this separate client. The original client is unchanged. Stage timings use a separate profiling pass. Replay timings use the course harness without profiling and include pipe IO. The storage supports 64 symbols, 16 messages per tick and names/IDs up to 128 bytes.

Measurements from the Mac run are in `report.md`. The included `scripts/latency_replay.py` is an unchanged copy of the course harness.
