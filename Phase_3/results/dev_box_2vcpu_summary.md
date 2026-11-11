Machine: Linux-6.18.44-fc-v310-x86_64-with-glibc2.39 / x86_64 / 2 logical CPUs

SPSC course tests: 5 correctness lines, concurrency ['RESULT|concurrent|pass|moved 2000000 items in FIFO order across threads'], throughput 177,366,854 ops/s, TSan warnings 0

Whole-bot TSan (replay): warnings 0; live: exit=0 snapshots_sent=5000 events=2766 dropped_books=2283 orders=401 acks=401 fills=133 tsan_warnings=0

| Lock-step harness (µs) | p50 | p90 | p99 | p99.9 |
|---|---:|---:|---:|---:|
| Phase 2 | 48.76 | 59.61 | 102.44 | 224.32 |
| Phase 3 | 75.65 | 156.26 | 218.22 | 859.49 |

| Paced (µs from schedule) | offered/s | achieved/s | p50 | p99 | p99.9 | max | final lag |
|---|---:|---:|---:|---:|---:|---:|---:|
| Phase 2 | 5000 | 5000 | 41.35 | 534.57 | 3239.26 | 5812.61 | 660.61 |
| Phase 3 | 5000 | 5000 | 130.63 | 4635.99 | 6600.17 | 10425.93 | 174.42 |
| Phase 2 | flat out | 37666 | 4218.30 | 5803.61 | 6018.20 | 6134.83 | 4135.60 |
| Phase 3 | flat out | 28847 | 6764.08 | 10907.15 | 12756.50 | 13885.19 | 5254.71 |

Quota run:
    [pipeline] events=40000 max_backlog=145 dropped_books=0 send_thread=1 cores=-1,-1,-1
    [quota] order_quota=6 window_ms=1000 max_used_in_window=5 skipped=14221
