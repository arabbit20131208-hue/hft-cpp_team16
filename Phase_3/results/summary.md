Machine: macOS-26.6.2-arm64-arm-64bit / arm / 14 logical CPUs

SPSC course tests: 5 correctness lines, concurrency ['RESULT|concurrent|pass|moved 2000000 items in FIFO order across threads'], throughput 34,946,197 ops/s, TSan warnings 0

Whole-bot TSan (replay): warnings 0; live: exit=0 snapshots_sent=5000 events=6099 dropped_books=0 orders=824 acks=824 fills=274 tsan_warnings=0

| Lock-step harness (µs) | p50 | p90 | p99 | p99.9 |
|---|---:|---:|---:|---:|
| Phase 2 | 27.83 | 31.83 | 38.83 | 55.17 |
| Phase 3 | 28.88 | 35.96 | 43.42 | 63.26 |

| Paced (µs from schedule) | offered/s | achieved/s | p50 | p99 | p99.9 | max | final lag |
|---|---:|---:|---:|---:|---:|---:|---:|
| Phase 2 | 5000 | 5000 | 31.67 | 51.33 | 137.29 | 798.71 | 34.17 |
| Phase 3 | 5000 | 5000 | 35.08 | 56.00 | 2710.12 | 7029.88 | 43.67 |
| Phase 2 | 10000 | 10000 | 31.17 | 60.38 | 128.50 | 347.21 | 34.92 |
| Phase 3 | 10000 | 10000 | 36.12 | 52.58 | 94.54 | 834.71 | 33.42 |
| Phase 2 | 20000 | 20000 | 35.67 | 48.54 | 119.17 | 222.58 | 39.54 |
| Phase 3 | 20000 | 20000 | 36.62 | 53.92 | 83.38 | 356.88 | 30.08 |
| Phase 2 | 40000 | 39996 | 172.79 | 19189.71 | 19733.08 | 19760.75 | 131.33 |
| Phase 3 | 40000 | 39999 | 46.29 | 4274.17 | 4299.29 | 4333.83 | 43.62 |
| Phase 2 | flat out | 39985 | 6167.04 | 8419.29 | 8536.67 | 8637.79 | 4629.38 |
| Phase 3 | flat out | 46420 | 5319.42 | 7221.29 | 7675.96 | 7948.00 | 3943.79 |

Quota run:
    [pipeline] events=40000 max_backlog=3 dropped_books=0 send_thread=1 cores=-1,-1,-1
    [quota] order_quota=6 window_ms=1000 max_used_in_window=5 skipped=14235
