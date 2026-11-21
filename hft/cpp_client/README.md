# Team 16 HFT client — Phase 4: gateway + strategy processes

This directory is our current bot. It is two processes that talk over lock-free
rings in POSIX shared memory:

```
            arena (WebSocket, JSON)
                    │  ▲
     book_snapshot  │  │ place_order / cancel_order
                    ▼  │
 ┌──────────────── hft_bot = Gateway process ───────────────────┐
 │ rx thread: decode → InMsg   tx thread: OutMsg → encode, send  │
 │ supervisor: start / watch / restart strategy, kill switch     │
 └──────┬──────────────────────────────────────────────▲────────┘
        │ in  ring  ShmSpsc<InMsg,1024>    (shm_open    │ out ring  ShmSpsc<OutMsg,1024>
        │ book updates, acks, fills, resync  + mmap)    │ order intents (+ replay "done")
 ┌──────▼──────────────── strategy process ─────────────┴────────┐
 │ QueueStrategy: local book, EWMA signal, queue-aware quoting,   │
 │ order_quota                                                    │
 └────────────────────────────────────────────────────────────────┘
```

The arena still sees one client (the gateway). The strategy has no socket. If
the strategy crashes or hangs, the gateway does three things:
1. keeps the connection,
2. cancels our resting orders,
3. starts a new strategy and resyncs its positions.

Design, measurements and the isolation-vs-latency analysis are in
[`Phase_4/Phase4_Report.md`](../../Phase_4/Phase4_Report.md). Earlier phases are
kept in `Phase_1/`…`Phase_3/`.

## Files

| Path | What |
|---|---|
| `../../include/shm_ring.hpp` | `ShmSpsc<T,N>` cross-process ring (claim/commit, peek/release) + the CI `ShmRing` |
| `include/shm_layout.hpp` | `InMsg` / `OutMsg` (fixed-size POD), `Control` block, `Region`, `Mapping` (shm_open + mmap) |
| `src/gateway.cpp` | Gateway process → `hft_bot` |
| `src/strategy.cpp` | Strategy process → `strategy` |
| `include/queue_strategy.hpp` | the trading logic (Phases 2–3), transport-independent |
| `include/local_book.hpp`, `quote_policy.hpp`, `quota.hpp`, `fast_codec.hpp`, `spin.hpp` | book, queue policy, order quota, JSON codec, spin/clock/histogram |
| `launch.sh` | build if needed, then start (live, `--replay`, `--no-spawn`, `strategy`) |
| `tests/` | `book_test`, `shm_test` (cross-process, consumer kill/restart), `replay_test.py` (two-process replay), `resilience_test.py` (SIGKILL + SIGSTOP the strategy live) |
| `tools/` | `shm_ipc_bench` (one-way / round-trip / throughput), `tape_driver` (paced open-loop replay) |

## Build and test

```bash
cmake -S hft/cpp_client -B build_phase4 -DCMAKE_BUILD_TYPE=Release -DHFT_USE_TLS=ON
cmake --build build_phase4 -j
pip install websockets                       # for the live crash test
ctest --test-dir build_phase4 --output-on-failure

# ThreadSanitizer build (both processes and IXWebSocket instrumented)
cmake -S hft/cpp_client -B build_tsan -DHFT_TSAN=ON && cmake --build build_tsan -j
ctest --test-dir build_tsan
```

## Run

```bash
./hft/cpp_client/launch.sh                       # live: gateway starts the strategy
./hft/cpp_client/launch.sh --replay < tape.jsonl # offline, one output line per input line
```

Kill demo (two terminals):

```bash
./hft/cpp_client/launch.sh --no-spawn            # terminal 1: gateway only, waits for a strategy
./hft/cpp_client/launch.sh strategy              # terminal 2: start, Ctrl-C / kill -9, start again
```

In the default mode the gateway does this by itself: `kill -9 $(pgrep -f build_phase4/strategy)`.
It logs `strategy lost (killed by signal)` and then `strategy back (epoch N) … ms after the loss`.

Settings (environment):

| Variable | Default | Meaning |
|---|---|---|
| `TEAM_ID`, `EXCHANGE_URL`, `ARENA_TOKEN`, `ARENA_LEVEL` | | venue (also read from `.env`) |
| `ORDER_QUOTA` / `ORDER_QUOTA_MS` | 6 / 1000 | messages per window; the last one is kept for cancels (0 = off) |
| `TICK_SIZE`, `QUEUE_AWARE` | 0.01 / 1 | as in Phase 2 |
| `HFT_SHM` | `/hft16_<gateway pid>` | shared-memory name (`/hft_team16` with `--no-spawn`) |
| `STRATEGY_BIN` | `strategy` next to `hft_bot` | strategy executable the gateway starts |
| `STRATEGY_CORE`, `IDLE_SPINS` | unset / 4096 | pin the strategy (Linux); spins before an idle thread yields |

On exit the gateway prints `[gateway]`, `[ipc] out-ring` and tick-to-trade lines.
The strategy prints `[strategy]`, `[ipc] in-ring`, `[quota]`, `[book]` and `[queue]` lines.
`--profile` adds one `PROF` line per order with six fields: gateway decode,
in-ring hop, strategy, out-ring hop, encode+send, and total (ns).
