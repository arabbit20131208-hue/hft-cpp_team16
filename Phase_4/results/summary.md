Machine: macOS-26.6.2-arm64-arm-64bit / arm / 14 logical CPUs

Course shm_ring_test: ['RESULT|shm_spsc|pass|moved 500000 items across processes, FIFO']; median 38,250,059 ops/s

IPC microbenchmark (shm_ipc_bench 200000 10000):
```
shm_ipc_bench: n=200000 pace=10000 ns, InMsg=1344 B
one-way    16 B           n=190000 p50=83 p90=292 p99=5435791 p99.9=9536041 max=10006041 ns
one-way    InMsg 1344 B   n=190000 p50=84 p90=84 p99=2750 p99.9=8116500 max=10002625 ns
rtt/2      16 B           n=190000 p50=41 p90=42 p99=62 p99.9=83 max=3937 ns
rtt/2      InMsg 1344 B   n=190000 p50=62 p90=62 p99=83 p99.9=104 max=7145 ns
flat-out   16 B           n=2000000 53184063 msgs/s (851 MB/s)
flat-out   InMsg 1344 B   n=200000 29819774 msgs/s (40078 MB/s)
```

| Lock-step harness (µs) | p50 | p90 | p99 | p99.9 |
|---|---:|---:|---:|---:|
| Phase 3 | 29.88 | 37.62 | 45.46 | 73.88 |
| Phase 4 | 30.00 | 37.00 | 48.62 | 83.15 |

| Paced busy tape (µs from schedule) | Offered/s | Achieved/s | p50 | p99 | p99.9 | Max | Final lag |
|---|---:|---:|---:|---:|---:|---:|---:|
| Phase 3 | 5,000 | 5,000 | 34.04 | 50.75 | 157.71 | 3195.54 | 33.92 |
| Phase 4 | 5,000 | 5,000 | 33.62 | 49.38 | 99.88 | 290.00 | 36.96 |
| Phase 3 | 10,000 | 10,000 | 33.67 | 46.79 | 90.67 | 143.29 | 43.21 |
| Phase 4 | 10,000 | 10,000 | 32.29 | 48.33 | 103.54 | 294.67 | 37.46 |
| Phase 3 | 20,000 | 20,000 | 33.42 | 47.38 | 102.08 | 130.62 | 32.33 |
| Phase 4 | 20,000 | 20,000 | 33.54 | 44.71 | 78.96 | 117.88 | 34.25 |
| Phase 3 | 40,000 | 39,943 | 301.12 | 7755.25 | 8933.54 | 9088.12 | 1369.08 |
| Phase 4 | 40,000 | 40,000 | 225.54 | 4130.83 | 4300.21 | 4330.83 | 29.96 |
| Phase 3 | flat out | 47,367 | 5201.62 | 7304.62 | 7763.25 | 8095.08 | 4166.42 |
| Phase 4 | flat out | 46,946 | 5265.79 | 6996.62 | 7176.42 | 7506.21 | 4077.25 |

Phase 4 per-stage (ns, 4383 orders, --profile, paced 5,000/s, ORDER_QUOTA=0):

| Stage | p50 | p99 |
|---|---:|---:|
| gateway decode | 2,750 | 3,500 |
| in-ring hop | 83 | 1,584 |
| strategy decide | 208 | 541 |
| out-ring hop | 84 | 1,208 |
| encode + send | 542 | 1,583 |
| total | 3,834 | 10,375 |

Phase 4 replay logs:
    [gateway] shm /hft16_89286 (1472 KB); waiting for strategy
    [strategy] pid 89287 attached to /hft16_89286 (epoch 1)
    [strategy] messages=40000 gaps=0 stale_books_skipped=0
    [ipc] in-ring one-way n=40000 p50=96 p90=768 p99=1600 p99.9=26944 ns
    [gateway] frames=40000 books_pushed=40000 dropped_ring_full=0 dropped_strategy_down=0 orders_sent=4613 lost_lines=0
    [gateway] strategy_restarts=0 killswitch_cancels=0 last_recovery_ms=0.0
    [ipc] out-ring one-way n=4613 p50=96 p90=192 p99=1216 p99.9=19008 ns
    [gateway] tick-to-trade (decode -> order handed to socket/output) n=4613 p50=3904 p90=4928 p99=10592 p99.9=34688 ns

Resilience (3 runs):
    exit=0 venue_connections=1 killed_pid=89292 new_pid=89294 detected="killed by signal" recovery_ms=57.7 resting_at_kill=3 killswitch_cancels=3 venue_cancels_within_500ms=23 orders_before_kill=97 orders_after_recovery=96 shm_left=False
    hang: hung_pid=89293 detected="heartbeat timeout" recovery_ms=55.1 orders_after=85
    resilience_test: strategy SIGKILL and hang (SIGSTOP) both survived; the gateway kept its connection, cancelled resting orders, restarted the strategy and resumed trading.
    exit=0 venue_connections=1 killed_pid=89302 new_pid=89304 detected="killed by signal" recovery_ms=52.9 resting_at_kill=3 killswitch_cancels=3 venue_cancels_within_500ms=25 orders_before_kill=97 orders_after_recovery=97 shm_left=False
    hang: hung_pid=89303 detected="heartbeat timeout" recovery_ms=54.0 orders_after=85
    resilience_test: strategy SIGKILL and hang (SIGSTOP) both survived; the gateway kept its connection, cancelled resting orders, restarted the strategy and resumed trading.
    exit=0 venue_connections=1 killed_pid=89307 new_pid=89309 detected="killed by signal" recovery_ms=52.9 resting_at_kill=3 killswitch_cancels=3 venue_cancels_within_500ms=24 orders_before_kill=97 orders_after_recovery=97 shm_left=False
    hang: hung_pid=89308 detected="heartbeat timeout" recovery_ms=56.0 orders_after=85
    resilience_test: strategy SIGKILL and hang (SIGSTOP) both survived; the gateway kept its connection, cancelled resting orders, restarted the strategy and resumed trading.

TSan two-process replay warnings: 0
