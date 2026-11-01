# Phase 2 — Local Order Book

## Design

We use fixed arrays indexed by price ticks. Each snapshot replaces the visible book. We clear the slots used by the previous snapshot. Best bid and ask are cached during the update. Reading either takes O(1). Updating takes O(depth). The local book update allocates no memory.

The signal combines OBI with the microprice offset from mid, scaled by half the spread. We update an EWMA with `s += 0.2 * (x - s)`. This takes O(1).

## Measurements

Machine: Mac, macOS, AppleClang 21.0.0, Release build.
We used 2,000 generated snapshots with ten levels per side. Warm-up was 5,000 updates. Results are medians across seven runs. Timing covers book updates, best-price reads, and signal updates. Throughput uses a separate batch timer.

| Implementation | p50 (ns) | p99 (ns) | Snapshots/s |
|---|---:|---:|---:|
| Map reference | 500 | 625 | 2,072,440 |
| Flat arrays | 42 | 84 | 18,972,700 |

The flat version reduced p50 by 91.6%. Throughput increased by 9.15 times. These are local book results from generated data. They do not measure the full client on a normal venue tape.

The project built successfully. The local-book test and queue replay checks passed. Live AlgoArena book agreement remains unverified.

## Queue behavior

We track `queue_ahead` and `level_qty` from acknowledgements and queue updates. Queue updates must match the order ID. We treat `queue_ahead <= level_qty / 4` as near the front.

We retain resting orders. The threshold for quoting the opposite side rises from 0.15 to 0.50 when the existing order is near the front. With identical replay messages, a position of 10/100 kept only the buy order. A position of 80/100 allowed an additional sell order after the signal turned negative.
