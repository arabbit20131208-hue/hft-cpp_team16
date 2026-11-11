# Phase 3 — Threaded Pipeline and Lock-Free SPSC Rings (Team 16)

Measurements in §4 were taken on macOS 26.6.2, Apple Silicon (arm64, 14 logical
CPUs), with `python3 Phase_3/scripts/phase3_bench.py`. Correctness and TSan were
checked on both that Mac and a 2-vCPU Linux box.

## 1. Pipeline design

```
 rx thread                    strategy thread                 send thread
 (socket callback /           (bot hooks: local book,         (socket write / replay
  stdin reader in replay)      signal, queue policy, quota)    line, LAT + PROF logs)
 stamp, decode ──ticks_──▶ pop, decide, encode ──sends_──▶ pop, send, log
        ▲                         │        ▲                      │
        └────────free_in_─────────┘        └──────free_out_───────┘
```

- **What travels on the rings.** The course `SPSCRing` carries `uint64_t`, so the
  rings carry **slot indices**. The payloads live in two pools that are
  allocated once at start-up:
  - 256 `Event` slots, each holding a decoded snapshot (with depth), or an
    ack, fill, error or session event, and its timestamps.
  - 256 `OutSlot`s, each holding the encoded wire messages of one event.
- **Four rings, each with one producer and one consumer.** Every pool has a
  forward ring (`ticks_`, `sends_`) and a free-list ring back (`free_in_`,
  `free_out_`). Pushing an index hands the whole slot to the other thread. The
  ring's release store and acquire load order the slot's contents, so the slots
  themselves need no atomics.
- **The strategy thread does all trading.** The network callback no longer
  trades. It only stamps the arrival time, decodes (SAX for snapshots, DOM for
  the rarer acks and fills), and pushes. Every bot hook runs on the strategy
  thread, so bot state needs no locks.
- **The decision path never blocks.** The strategy thread takes no lock, does no
  allocation (all strings and buffers are reserved up front), and makes no
  system call while work is queued. Encoding happens there; socket writes and
  `LAT`/`PROF` logging moved to the send thread.
- **Idle policy.** An idle thread spins with `pause`/`yield` instructions, then
  `std::this_thread::yield()` after `IDLE_SPINS` empty polls (default 4,096).
  That keeps an idle pipeline from starving other processes and is never reached
  while work is queued.
- **Ordering.** Every inbound frame becomes one event in arrival order.
  In replay mode every input line produces exactly one output line, in order
  (tested on 3,000 mixed messages). A stop event at EOF drains the pipeline.
- **Overload policy.** If all 256 `Event` slots are in flight, a live book
  snapshot is dropped and counted (`[pipeline] dropped_books`), because a newer
  snapshot replaces it anyway. Acks, fills and errors wait for a slot and are
  never dropped. Replay never drops. The send stage applies back-pressure
  through `free_out_`.
- **Pinning.** `PIN_CORES=rx,strategy,send` pins each thread with
  `pthread_setaffinity_np` on Linux. macOS has no hard-affinity API, so on the
  Mac the threads float.
- **Inline send.** `SEND_THREAD=0` runs the send stage on the strategy thread,
  which gives a 2-thread variant for comparison.
- **`order_quota`.** `QuotaGuard` allows at most `ORDER_QUOTA` messages
  (default 6) per `ORDER_QUOTA_MS` window (default 1,000 ms). The last message of
  each window is reserved for cancels, so a risk-reducing cancel is never
  starved by new quotes. When the budget is spent, the strategy holds and tries
  again on the next tick. This is a client-side guard; the venue's exact window
  definition is an assumption.

## 2. SPSC ring (`include/spsc_ring.hpp`)

- Implements the exact course contract: `SPSCRing(capacity_pow2)`, `push`,
  `pop`, `empty`, `full`.
- **Indexing.** Monotonic `head_`/`tail_` counters, masked with `cap - 1`, so all
  `cap` slots are usable and the size is `tail - head`. A capacity that is not a
  power of two is rounded up.
- **Memory ordering.**
  - `push`: write the slot, then `tail_.store(release)`.
  - `pop`: `tail_.load(acquire)`, then read the slot, then
    `head_.store(release)`.
  - Each side loads its own counter `relaxed`, since it is the only writer.
- **Cached counters.** The producer keeps a private `head_cache_` and re-reads
  `head_` (acquire) only when the ring looks full. The consumer does the same
  with `tail_cache_` when the ring looks empty. In steady state each call
  touches only its own cache line.
- **Padding.** `alignas(64)` puts `tail_` + `head_cache_` and `head_` +
  `tail_cache_` on separate cache lines, so there is no false sharing.
- **Copies.** The bot uses an identical copy in
  `Phase_3/hft/cpp_client/include/`. CMake fails the build if the two copies
  differ, because CI grades the first `spsc_ring.hpp` it finds.

Course autograder, run locally with `tests/run_ci.py`: **Phase 3 section
100/100**. That covers compiles, basic, empty_pop, full, fifo, wrap,
concurrent, and TSan. Concurrency throughput on the development box varied
between runs, from 5.8 × 10⁷ to 1.8 × 10⁸ ops/s. On the Mac, median
throughput across five runs was 34,946,197 ops/s.

## 3. ThreadSanitizer-clean proof

| Run (dev box)                                                             | What it exercises                                                                 |  `ThreadSanitizer` warnings | Log                                    |
| ------------------------------------------------------------------------- | --------------------------------------------------------------------------------- | ----------------------------: | -------------------------------------- |
| Course `spsc_concurrency.cpp`, `-fsanitize=thread`, 200k items        | the ring alone, 1P/1C                                                             |                             0 | `results/tsan_spsc_course_linux.log` |
| `pipeline_test` (TSan build)                                            | slot-pool handoff pattern, 200k structs                                           |                             0 | ctest                                  |
| Whole bot, TSan build, `--replay` on a 40,000-snapshot busy tape        | rx → strategy → send, all hooks                                                 |                             0 | `results/tsan_replay_linux.log`      |
| Whole bot, TSan build, live WebSocket stress (`tests/socket_stress.py`) | IXWebSocket callback as rx; 5,000 snapshots plus acks and fills; SIGTERM shutdown |                             0 | `results/tsan_live_linux.log`        |
| `replay_test.py` under the TSan build (10 scenarios)                    | quota, ordering, queue logic                                                      | 0 (a race would exit with 66) | ctest                                  |

On the Mac, the live WebSocket TSan run (5,000 snapshots) also reported 0 warnings
and dropped 0 books.

In the Linux dev-box live TSan run, TSan slowed every thread roughly 5–15×. The strategy
thread fell behind the burst, and 2,283 of the 5,000 snapshots were dropped by
the overload policy (acks and fills are never dropped). That run checks for
races, not speed.

To reproduce: `cmake -S Phase_3/hft/cpp_client -B build_tsan -DHFT_TSAN=ON`,
build it, then run `ctest --test-dir build_tsan`. The TSan build also
instruments IXWebSocket.

## 4. Before/after percentiles

**Method.** The course harness (`scripts/latency_replay.py`) is lock-step: it sends
one line and waits for the answer, so at most one message is in flight and a pipeline
has nothing to overlap. The main measurement uses `tools/tape_driver`. It writes a
busy tape (40,000 snapshots, 8 symbols, 10 levels, 2,000 warm-up snapshots) at a fixed
offered rate and reads answers on another thread. Latency is measured from each
message's **scheduled** send time, so falling behind the tape shows up as latency
(no coordinated omission). The paced benchmark passed `ORDER_QUOTA=6` to both bots.
Phase 3 applies the quota guard. Phase 2 uses its existing order behavior.

| Lock-step harness (µs) |   p50 |   p90 |   p99 | p99.9 |
| ----------------------- | ----: | ----: | ----: | ----: |
| Phase 2 (single thread) | 27.83 | 31.83 | 38.83 | 55.17 |
| Phase 3 (pipeline)      | 28.88 | 35.96 | 43.42 | 63.26 |

| Paced busy tape | Offered/s | Achieved/s |  p50 µs |   p99 µs | p99.9 µs |   Max µs | Final lag µs |
| --------------- | --------: | ---------: | -------: | --------: | --------: | --------: | ------------: |
| Phase 2         |     5,000 |      5,000 |    31.67 |     51.33 |    137.29 |    798.71 |         34.17 |
| Phase 3         |     5,000 |      5,000 |    35.08 |     56.00 |  2,710.12 |  7,029.88 |         43.67 |
| Phase 2         |    10,000 |     10,000 |    31.17 |     60.38 |    128.50 |    347.21 |         34.92 |
| Phase 3         |    10,000 |     10,000 |    36.12 |     52.58 |     94.54 |    834.71 |         33.42 |
| Phase 2         |    20,000 |     20,000 |    35.67 |     48.54 |    119.17 |    222.58 |         39.54 |
| Phase 3         |    20,000 |     20,000 |    36.62 |     53.92 |     83.38 |    356.88 |         30.08 |
| Phase 2         |    40,000 |     39,996 |   172.79 | 19,189.71 | 19,733.08 | 19,760.75 |        131.33 |
| Phase 3         |    40,000 |     39,999 |    46.29 |  4,274.17 |  4,299.29 |  4,333.83 |         43.62 |
| Phase 2         |  flat out |     39,985 | 6,167.04 |  8,419.29 |  8,536.67 |  8,637.79 |      4,629.38 |
| Phase 3         |  flat out |     46,420 | 5,319.42 |  7,221.29 |  7,675.96 |  7,948.00 |      3,943.79 |

**Speedup vs Phase 2.**

- **Throughput.** Flat-out throughput rose 16.1%, from 39,985 to 46,420
  snapshots/s (1.16×).
- **Busy tape (40,000/s).** Phase 2 was near its flat-out ceiling. Its p50 was
  173 µs and its p99 was 19.19 ms. Phase 3 reached 46 µs at p50 and 4.27 ms
  at p99, a 4.5× reduction in p99.
- **Moderate load (10,000–20,000/s).** Phase 3 reduced p99.9 from 128.50 to
  94.54 µs at 10,000/s and from 119.17 to 83.38 µs at 20,000/s.

**Where Phase 3 is slower.**

- **Lock-step.** Phase 3 added 1.05 µs at p50 and 4.59 µs at p99. With one
  message in flight, the stages cannot overlap and each event crosses two
  thread handoffs.
- **Light load (5,000/s).** Phase 3 p99.9 was 2.71 ms. One possible cause is
  that idle threads yield after `IDLE_SPINS` empty polls and then wait to be
  rescheduled. The Mac run used unpinned threads. A larger `IDLE_SPINS` may
  reduce wake-up latency at the cost of more idle CPU use.

On the 2-vCPU Linux development box, Phase 3 was slower. Oversubscription is a
likely contributor. Those results are in `results/dev_box_2vcpu_summary.md`.

## 5. Order quota

On the Linux development box, the 40,000-snapshot tape used
`ORDER_QUOTA=6` and `ORDER_QUOTA_MS=1000`:
`[quota] max_used_in_window=5 skipped=14221`, with `dropped_books=0`. New
orders never used more than the 5 non-reserved messages, and the 6th stayed
available for cancels. `replay_test.py` case 9 checks that a cancel still goes
out when new orders are blocked.

The Mac run with the same quota settings recorded maximum window usage of 5,
14,235 skipped actions, maximum backlog of 3, and zero dropped books.

## 6. Limits

- The order-state timeouts from Phase 2 (2-snapshot cancel grace, 5-snapshot ack
  timeout) are unchanged. They are assumptions, not venue confirmations.
- Dropping live snapshots under overload keeps the newest information but means
  `on_depth` does not see every book. The count is printed so it can be checked.
- Spinning threads use CPU while the session is quiet. `IDLE_SPINS` trades idle
  CPU for wake-up latency.
- No pinning on macOS.
