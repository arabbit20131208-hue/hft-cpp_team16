# Phase 2 — Team 16

Run commands from the repository root. The phase report is `Phase_2/Phase2_Report.md`.

## Build and test

```bash
cmake -S Phase_2/hft/cpp_client -B build_phase2 -DCMAKE_BUILD_TYPE=Release -DHFT_USE_TLS=ON
cmake --build build_phase2 -j 4
ctest --test-dir build_phase2 --output-on-failure
./build_phase2/book_bench
```

## Local comparison

```bash
cmake -S Phase_1/phase1/cpp_client -B build_p1 -DCMAKE_BUILD_TYPE=Release -DPHASE1_FAST=ON
cmake --build build_p1 -j 4
python3 Phase_2/scripts/make_tape.py normal_tape.jsonl
python3 Phase_2/scripts/compare_phases.py normal_tape.jsonl Phase_2/results/compare.json "Phase 1=build_p1/hft_bot" "Phase 2 final=build_phase2/hft_bot"
python3 Phase_2/scripts/queue_sim.py build_phase2/hft_bot --json Phase_2/results/queue_sim.json
```

`results/book_bench.txt`, `compare.json` and `queue_sim.json` contain summaries transcribed from the user's Mac terminal output. They replace the Linux summaries for the submitted report. The per-round raw measurements were not supplied. The comparison tape has no venue acknowledgements; the toy CLOB provides simulated acknowledgements and fills.

## Live venue

Load the team's existing credentials in the same terminal. Use the registered bot ID from `.env`.

```bash
set -a
source .env
set +a
TICK_SIZE=0.01 ./build_phase2/hft_bot 2> lat_team16_clean.log
```

Run one bot only. Stop it with Control+C, not Control+Z. After it exits:

```bash
python3 scripts/latency_report.py lat_team16_clean.log
grep -aE '^\[(book|book-reject|book-mismatch|queue)\]' lat_team16_clean.log
```

The `[book]` lines now split the snapshots into checked / rejected by reason (`off_grid`, `span`, `duplicate`, `locked`, `crossed`, `too_deep`, `bad_size`) / unchecked (`one_sided`, `no_mid`, `no_slot`, `decoder_dropped`). The first three examples of each rejection reason and of any mismatch are printed as `[book-reject]` / `[book-mismatch]` lines with the offending price.

Do not commit `.env` or tokens. Start from a fresh venue session with no existing orders or positions. Stopping the client is not proof that venue-side orders were cancelled.

The latest v4 Mac run passed both tests. The book benchmark measured map/flat p50 of 436.2/56.0 ns per operation and throughput of 2,240,618/17,829,369 snapshots/s. The fresh-log live run processed 2,310 snapshots with zero rejections; 2,100 checked snapshots had zero mid/spread mismatches, and 210 were one-sided. It clipped 5,033 deep levels across 630 snapshots. Queue counters recorded 27 orders, zero fills, one near-front hold, 20 behind-touch cancellations, zero acknowledgement timeouts and zero errors. Live LAT: p50 81.25, p99 195.27, p99.9 233.65, max 237.92 µs across only 47 samples. These are small-sample live observations. The earlier offline comparison was not rerun for v4. Full pasted summaries and limitations are in `results/venue_summary.txt` and the report; the previous shared-log result is retained in `results/venue_summary_previous.txt`.

Known limits: common tick size for all symbols, 64 symbols, 256 levels per side, a 4,096-tick band re-anchored at the touch (levels more than 2,048 ticks away are clipped and counted in `[book] clipped_*`), unconfirmed order timeout assumptions. Reconnect reconciliation remains unverified.

## Submission

Place this directory in the team's private repository. Include source, this README, the report and results. Exclude build directories and credentials. Commit the final files, create or verify the team's Phase 2 tag, and submit the URL for that tag with `Phase2_Report.md` attached. The tag and push must be confirmed in the team's actual repository.
