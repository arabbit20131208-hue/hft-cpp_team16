# Phase 3 — Team 16

The Phase 2 bot split into three threads (rx/decode → strategy → send),
connected by lock-free SPSC rings. Design, the TSan proof and measurements are
in [`Phase3_Report.md`](Phase3_Report.md).

```
include/spsc_ring.hpp          (repo root) graded by the course CI
Phase_3/hft/cpp_client/
  include/spsc_ring.hpp        identical copy used by the bot (CMake checks they match)
  include/pipeline.hpp         Event / OutSlot pools, Backoff, pinning
  include/quota.hpp            order_quota guard
  include/arena_client.hpp     the three pipeline stages
  src/arena_client.cpp
  src/main.cpp                 QueueBot (Phase 2 strategy + quota)
  tests/                       book_test, pipeline_test, replay_test.py,
                               socket_test.py, socket_stress.py, book_bench
  tools/tape_driver.cpp        paced open-loop driver for --replay bots
Phase_3/scripts/phase3_bench.py  builds, verifies and measures everything
Phase_3/results/               TSan logs and measurements
```

## Build and test

```bash
cmake -S Phase_3/hft/cpp_client -B build_phase3 -DCMAKE_BUILD_TYPE=Release -DHFT_USE_TLS=ON
cmake --build build_phase3 -j 8 && ctest --test-dir build_phase3

# ThreadSanitizer build of the whole bot (IXWebSocket included)
cmake -S Phase_3/hft/cpp_client -B build_tsan -DHFT_TSAN=ON
cmake --build build_tsan -j 8 && ctest --test-dir build_tsan
```

Course SPSC grader locally: `python3 tests/run_ci.py`. The Phase 3 rows should
be 100/100.

## Produce the report numbers (on the Mac)

```bash
pip install websockets          # for the live TSan stress test
python3 Phase_3/scripts/phase3_bench.py
```

This benchmark takes about 5–10 minutes and writes the results to
`Phase_3/results/summary.md`. The Mac measurements used for submission
are documented in [`Phase3_Report.md`](Phase3_Report.md).

## Runtime settings

| Variable                             | Default       | Meaning                                                  |
| ------------------------------------ | ------------- | -------------------------------------------------------- |
| `ORDER_QUOTA` / `ORDER_QUOTA_MS` | 6 / 1000      | messages per window, last one kept for cancels (0 = off) |
| `SEND_THREAD`                      | 1             | 0 = send inline on the strategy thread                   |
| `PIN_CORES`                        | unset         | `rx,strategy,send` core ids (Linux only)               |
| `IDLE_SPINS`                       | 4096          | empty polls before an idle thread yields                 |
| `TICK_SIZE`, `QUEUE_AWARE`       | as in Phase 2 |                                                          |

Live run: same as Phase 2 (`source .env`, run one bot, `2> lat.log`). On exit the bot
prints `[pipeline]` (events, max backlog, dropped books) and `[quota]` lines in
addition to `[book]`/`[queue]`.

## Submission checklist

- `.github/workflows/grade.yml` (the course-provided workflow) must be in the repo;
  CI grades `include/spsc_ring.hpp` on every push.
- Commit, tag `phase3`, push; submit the tag URL and attach `Phase3_Report.md`.
