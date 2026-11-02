# Phase 2 — Local Order Book and Queue-Aware Quoting
Team 16

## Local book and signal

Each symbol has fixed bid and ask arrays indexed by integer price ticks. We clear only the slots used by the previous snapshot. Best prices and quantities are cached during each update. Updating takes O(depth); reading the touch takes O(1). The local book update allocates no memory. Cache misses are not ruled out.

Prices are converted to ticks once. The book rejects off-grid prices, invalid sizes, duplicate levels, crossed or locked books, and unsupported depth or spread. It supports 64 symbols, 256 levels per side, and a 4,096-tick band. Wide snapshots are re-anchored at the touch and deep levels outside the band are clipped and counted. All symbols use one configured tick size.

The signal combines OBI and the microprice offset scaled by half the spread. An EWMA updates it with `s += 0.2 * (x - s)`. This takes O(1), without a window recompute.

The client now caches scalar book fields instead of copying the full 8 KB snapshot. LAT logging runs after the tick's messages have been handed to the WebSocket interface. A mutex remains in the scalar cache.

## Local measurements

Machine: MacBook Pro M4 Pro, macOS, AppleClang 21.0.0, CMake Release. The local-book and queue-replay tests passed.

### Book benchmark

64,000 generated snapshots, up to ten levels per side, 20,000 warm-up operations, median of seven runs. Each sample times 32 operations and divides by 32. An operation copies compact depth into a reused buffer, updates the book, reads the touch, and updates the signal. It does not parse JSON.

| Book | p50 (ns/op) | p99 of 32-op means (ns/op) | Snapshots/s |
|---|---:|---:|---:|
| Map | 436.2 | 527.3 | 2,240,618 |
| Flat | 56.0 | 70.3 | 17,829,369 |

In the latest v4 Mac run, the flat book reduced p50 by 87.2%. Throughput was 7.96 times the map reference. Both local_book and queue_replay passed (2/2 tests). Full benchmark output is in `results/book_bench.txt`.

### Phase comparison

The earlier phase-comparison run, performed before the v4 clipping change, used the same Mac with 20,000 generated snapshots, 200 warm-up snapshots, and five alternating rounds. These are pipe round-trip times from the course replay harness.

| Build (µs) | p50 | p90 | p99 | p99.9 |
|---|---:|---:|---:|---:|
| Phase 1 | 28.71 | 36.21 | 45.00 | 88.25 |
| Phase 2 | 28.92 | 34.83 | 41.92 | 62.24 |

These comparison and profile numbers were not rerun for v4. Median latency was similar. Phase 2 had lower tail values in this experiment. We do not claim a general tail improvement from this run.

| Phase 2 profiled stage | p50 (µs) | p99 (µs) |
|---|---:|---:|
| Decode plus scalar-cache update | 2.334 | 3.292 |
| Book, signal, strategy, encode, send and LAT logging | 0.083 | 3.000 |
| Encode, ticks with messages | 0.375 | 1.208 |
| Arrival to send-interface return, 2,276 LAT samples | 2.875 | 4.958 |

The generated replay contains snapshots without venue acknowledgements. It recorded 2,273 acknowledgement timeouts, no fills and no resting-order cancellations. It measures this replay path, not the full acknowledged-order lifecycle. All 20,000 generated snapshots passed the mid/spread check. The generator uses the same levels to produce these fields, so this is limited correctness evidence.

## Queue-aware behaviour

Each side tracks PendingNew, Resting and PendingCancel states. A matching acknowledgement or queue update supplies `queue_ahead` and `level_qty`. Near-front means `queue_ahead <= level_qty / 4`.

We cancel when a better same-side touch exists or the signal is strongly adverse. With a mildly adverse signal, we hold near-front orders and cancel orders farther back. Rejoining loses FIFO priority. Replay tests cover these decisions and order-ID matching.

In the toy CLOB simulation, queue-aware runs had 833, 914 and 791 fills across seeds 1–3. Queue-blind runs had 868, 877 and 787. Near-front holds occurred 1, 4 and 2 times. These results do not show a consistent fill-rate or markout advantage. Full summaries are in `results/queue_sim.json`.

## Live AlgoArena observations and limitations

The v4 bot was run on the hosted venue after existing local hft_bot processes were stopped. It used a fresh log file, `lat_team16_v4_20261101_234745.log`, and was stopped with Control+C. The post-run process check reported that the bot had exited. The full raw log was not supplied; these results are transcribed from the user's Mac terminal output.

```text
[book] snapshots=2310 rejected=0 checked=2100 mid_mismatch=0 spread_mismatch=0
[book] unchecked: one_sided=210 no_mid=0 no_slot=0 decoder_dropped=0
[book] clipped_snapshots=630 clipped_levels=5033
[book] rejected by reason: too_deep=0 bad_size=0 off_grid=0 span=0 duplicate=0 locked=0 crossed=0
[queue] aware=1 orders=27 fills=0 held_near_front=1 cancel_behind_touch=20 cancel_strong_adverse=0 cancel_adverse_far_back=0 ack_timeouts=0 errors=0
```

All 2,310 snapshots were accepted. The 2,100 two-sided snapshots checked against the venue had zero mid/spread mismatches. The remaining 210 were one-sided and were not checked by this two-sided test. There were no decoder drops or rejected snapshots. This validates the checked touch fields, not every depth quantity.

The book clipped 5,033 deep levels across 630 snapshots (27.3% of snapshots). Zero rejections does not mean that full venue depth was retained. The price band is re-anchored at the touch; levels outside the retained band are omitted, and `quantity()` returns zero for those prices. The earlier report attributed span rejections to deep levels outside the 4,096-tick band. This v4 run confirms that clipping occurred without span rejections in the observed sample.

The queue counters recorded 27 orders, no fills, one near-front hold and 20 behind-touch cancellations. There were no acknowledgement timeouts or reported errors. The hold counter records orders entering the near-front hold branch, not every hold tick. With no fills, this run provides no evidence of a fill-rate or profitability advantage.

| Live LAT (µs) | p50 | p99 | p99.9 | Max | Samples |
|---|---:|---:|---:|---:|---:|
| v4, fresh log | 81.25 | 195.27 | 233.65 | 237.92 | 47 |

Only 47 LAT samples were recorded, so the upper percentiles are descriptive values from this small sample, not stable tail estimates. The LAT sample count differs from the snapshot count. The exact run duration was not recorded. Live LAT is not comparable to the replay figures above: on the venue, the timed window includes the WebSocket send interface; replay uses an in-memory output path. These live figures do not replace the course's offline replay measurements.

Earlier live output from two processes sharing a log is retained in `results/venue_summary_previous.txt` for provenance. It is not used as the current v4 result.

The protocol handling assumes cancellation completion after two snapshots and new-order rejection after five snapshots without an acknowledgement. These are timeout assumptions, not venue confirmations. Delayed replies can cause order-state errors or position-limit breaches. Reconnect reconciliation and venue-side order cleanup after shutdown remain unverified.
