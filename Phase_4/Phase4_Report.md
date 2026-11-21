# Phase 4 — Gateway + Strategy Processes over Shared Memory (Team 16)

**Where the code is.** The Phase 4 bot is the repo-root client,
`hft/cpp_client/` (`hft_bot` = gateway, `strategy`). The ring is
`include/shm_ring.hpp`. Build, run and test commands are in
`hft/cpp_client/README.md`. The single-process Phase 3 bot used as the
baseline is unchanged in `Phase_3/hft/cpp_client/`.

**How the numbers were produced.** One command produces everything in §4 and
§5: `python3 Phase_4/scripts/phase4_bench.py`. Raw outputs are in
`Phase_4/results/`.

<!-- BEGIN:MACHINE -->
Measured on: macOS-26.6.2-arm64-arm-64bit / arm / 14 logical CPUs.
<!-- END:MACHINE -->

## 1. Architecture

```
            arena (WebSocket, JSON)
                    │  ▲
     book_snapshot  │  │ place_order / cancel_order
                    ▼  │
 ┌──────────────── hft_bot = Gateway process ───────────────────┐
 │ rx thread    stamp t_recv, SAX-decode, write one InMsg        │
 │ tx thread    read OutMsg, encode JSON, send; stamp LAT/PROF   │
 │ supervisor   start strategy (posix_spawn), watch it (waitpid  │
 │              + heartbeat), kill switch, restart, resync       │
 └──────┬──────────────────────────────────────────────▲────────┘
        │ in ring: ShmSpsc<InMsg,1024>                 │ out ring: ShmSpsc<OutMsg,1024>
        │ book updates, acks, queue updates, fills,    │ order intents (place / cancel),
        │ errors, position resync                      │ replay "done" marks
        │            ── one POSIX shm region (shm_open + mmap), 1.47 MB ──
 ┌──────▼──────────────── strategy process ─────────────┴────────┐
 │ QueueStrategy (Phases 2-3): flat local book, EWMA signal,      │
 │ queue-aware hold/cancel, order_quota. Heartbeat every 256 turns│
 └────────────────────────────────────────────────────────────────┘
```

- **The arena sees one client.** The gateway owns the only socket. The
  strategy has no network access, so a strategy bug cannot disconnect us.
- **Latency stamps.** Both processes read the same system-wide monotonic clock
  (`steady_clock`), so the gateway stamps tick-to-trade from `t_recv` at
  decode to the `place_order` send. The shared-memory round trip is inside
  that window. Each `OutMsg` carries the origin's four timestamps, so
  `--profile` splits every order into stages (§4.3).
- **Replay.** `hft_bot --replay` starts the strategy itself, so the course
  harness and `tape_driver` drive the whole two-process system.

### Region layout (`hft/cpp_client/include/shm_layout.hpp`)

| Part | Contents |
|---|---|
| `Control` | `magic`/`version`, which the creator writes last with release. Gateway line: heartbeat, pid, `shutdown`, `session_open`. Strategy line: heartbeat, pid (owner of the consumer side), `epoch`, `ready`. Each process writes only its own 64-byte line. |
| `in` | `ShmSpsc<InMsg, 1024>`. `InMsg` is 1,344 B, `alignas(64)`: seq, timestamps, type, symbol/order-id/error text as fixed `char[]`, scalars, and the 32 best levels per side. |
| `out` | `ShmSpsc<OutMsg, 1024>`. `OutMsg` is 128 B: seq, the 4 origin timestamps, type, side, qty, price, symbol, order id. |

Both message types are `static_assert`ed trivially copyable. They contain no
pointers or `std::string`, because the region is mapped at a different
address in each process.

## 2. The shared-memory ring (`include/shm_ring.hpp`)

- **`ShmSpsc<T, N>`** is our Phase 3 SPSC algorithm made position-independent.
  - Monotonic 64-bit `head_`/`tail_` counters with slot `= counter & (N-1)`.
  - Release on publish and acquire on observe.
  - Each side caches the other side's counter and re-reads it only when the
    ring looks full or empty.
  - Producer line, consumer line and the slot array each start on their own
    64-byte line.
  - `static_assert`s check the power-of-two capacity, trivially copyable `T`,
    and `std::atomic<uint64_t>::is_always_lock_free` (needed for atomics to
    work across processes).
- **No copies through the ring.** The gateway SAX-decodes into a reused
  `BookView`, then writes the fixed layout directly into the claimed slot
  (`claim()` → fill → `commit()`). The strategy reads the slot in place
  (`peek()` → use → `release()`). Copying `push`/`pop` exist too.
- **Crash safety.**
  - A producer that dies after writing a slot but before `commit()` publishes
    nothing.
  - A consumer that dies before `release()` leaves the message for its
    successor.
  - The cached counters only ever lag the real ones, so caches left in shared
    memory by a dead process are stale but safe. `shm_test` checks this: it
    SIGKILLs a consumer after about 34% of 100k messages, starts a new one, and
    sees every message exactly once.
- **`ShmRing`** is the CI contract (`CAPACITY = 1024`, `init`, `push`, `pop`):
  an `ShmSpsc<uint64_t, 1024>`.

### Protocol: handshake, back-pressure, sequence numbers, heartbeat

- **Start.** The gateway creates the region: it unlinks any stale one,
  `shm_open(O_CREAT|O_EXCL)`, `ftruncate`, initializes, and writes `magic`
  last. Then it starts the strategy and waits for `ready` before it lets any
  market data flow.
  - The strategy attaches: it checks `magic` and `version`, then claims the
    consumer side by CAS on `st_pid`. A second strategy is refused while the
    first is alive.
  - It bumps `epoch`, sets `ready`, and starts consuming.
- **Stop.** The gateway sets `shutdown`. The strategy prints its stats and
  exits. The gateway reaps it (SIGKILL after 2 s), then unmaps and
  `shm_unlink`s the region.
- **Back-pressure, inbound.**
  - If the in-ring is full, a live book update is dropped and counted. A newer
    snapshot replaces it, and the gateway's rx thread never waits for the
    strategy on market data.
  - Acks and fills wait for space.
  - Replay never drops.
- **Back-pressure, outbound.** If the out-ring is full, the strategy waits for
  the gateway.
- **Sequence numbers.** Every inbound frame gets the next `seq`, including
  dropped ones. The strategy counts gaps (`[strategy] gaps=`). In replay,
  `seq` is the input line number and the strategy answers each one with
  `kDone`. The gateway's tx thread turns those into output lines in order. A
  line lost with a crashed strategy is answered `[]` (`lost_lines`).
- **Heartbeats.**
  - The strategy writes its heartbeat every 256 loop turns, busy or idle.
  - The gateway supervisor writes its own every 2 ms.
  - A strategy whose gateway has been silent for 3 s, and whose pid is gone,
    exits by itself.

## 3. Robustness: the strategy can crash and restart

The gateway supervisor polls every 2 ms and treats the strategy as lost in
three cases:
- `waitpid` reports it exited or was killed;
- its heartbeat is older than 500 ms, meaning it is alive but hung (the
  gateway then SIGKILLs it);
- its pid is gone, for a strategy started by hand with `--no-spawn`.

Then it does four things:
1. **Stops feeding it.** Book updates are dropped (`dropped_strategy_down`).
2. **Kill switch.** It cancels every order it knows is resting. The gateway
   tracks our open orders from `order_ack`, fills and its own cancels, so it
   needs nothing from the dead process.
3. **Restarts** the strategy after 50 ms, at most 20 times a minute.
4. **Resyncs.** When the new strategy's `epoch` appears, it pushes one
   `kResync` message per symbol with the position it tracked from fills, before
   any new market data. Books that queued during the restart and are older
   than 200 ms are skipped.

The WebSocket, the gateway's threads and its venue session are untouched by
any of this. A strategy that exits with code 3 (it asked to stop, e.g. after
an error flood) stops the whole system instead of restarting.

`tests/resilience_test.py` checks this against a local fake venue that acks
every order:
1. SIGKILL the strategy while orders are resting.
2. Freeze its replacement with SIGSTOP, which only the heartbeat can detect.

<!-- BEGIN:RESILIENCE -->
| Run | Detected as | Resting orders at kill | Kill-switch cancels | Recovery (loss → new strategy consuming) | Orders after recovery | Venue connections |
|---|---|---:|---:|---:|---:|---:|
| 1a SIGKILL | killed by signal | 3 | 3 | 57.7 ms | 96 | 1 |
| 1b SIGSTOP (hang) | heartbeat timeout (after the 500 ms timeout) | | | 55.1 ms after detection | 85 | 1 |
| 2a SIGKILL | killed by signal | 3 | 3 | 52.9 ms | 97 | 1 |
| 2b SIGSTOP (hang) | heartbeat timeout (after the 500 ms timeout) | | | 54.0 ms after detection | 85 | 1 |
| 3a SIGKILL | killed by signal | 3 | 3 | 52.9 ms | 97 | 1 |
| 3b SIGSTOP (hang) | heartbeat timeout (after the 500 ms timeout) | | | 56.0 ms after detection | 85 | 1 |
<!-- END:RESILIENCE -->

## 4. Latency

### 4.1 One-way IPC across the shared-memory ring (`tools/shm_ipc_bench`)

Two processes (fork) share the real `ShmSpsc` rings. The producer stamps
`now_ns()` just before `commit()` and the consumer subtracts right after
`peek()`. Messages are paced at 1 per 10 µs, so this is the cost of the hop,
not of queueing. The round-trip / 2 rows avoid comparing clock reads across
cores. The flat-out rows copy every message in full and checksum it.

<!-- BEGIN:IPC -->
| Path (2 processes) | p50 ns | p90 ns | p99 ns | p99.9 ns |
|---|---:|---:|---:|---:|
| one-way, 16 B | 83 | 292 | 5,435,791 | 9,536,041 |
| one-way, InMsg 1,344 B | 84 | 84 | 2,750 | 8,116,500 |
| round trip / 2, 16 B | 41 | 42 | 62 | 83 |
| round trip / 2, InMsg | 62 | 62 | 83 | 104 |

Throughput (every message written and read in full):

- `flat-out   16 B           n=2000000 53184063 msgs/s (851 MB/s)`
- `flat-out   InMsg 1344 B   n=200000 29819774 msgs/s (40078 MB/s)`
<!-- END:IPC -->

Note on the one-way tails. The one-way runs show millisecond tails (5.44 ms at p99 for 16 B) despite medians of about 83 ns. The round-trip / 2 rows on the same Mac do not: there both processes spin continuously, and p99.9 stays at 83–104 ns. The difference is the waiting policy. The one-way consumer uses our idle policy (spin, then std::this_thread::yield() after IDLE_SPINS empty polls), because messages arrive only every 10 µs. So the tails most likely come from macOS rescheduling a thread that had yielded its core, not from the ring itself. The same effect explains Phase 3's tail at light load. A larger IDLE_SPINS, or pinning (not available on macOS), trades idle CPU for lower wake-up latency.

### 4.2 End-to-end tick-to-trade vs the single-process Phase 3 bot; throughput

Both bots answer the same busy tape (40,000 snapshots, 8 symbols, 10 levels;
2,000 warm-up; `ORDER_QUOTA=6`).
- **Lock-step:** the course harness sends one line and waits for the answer.
- **Paced:** `tape_driver` writes at a fixed rate and measures from each
  message's scheduled time, so falling behind shows up as latency.

<!-- BEGIN:E2E -->
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
<!-- END:E2E -->

### 4.3 Where Phase 4's time goes (per order, `--profile`)

<!-- BEGIN:STAGES -->
4,383 orders from the busy tape, paced at 5,000/s (`--profile`, `ORDER_QUOTA=0`):

| Stage | p50 ns | p99 ns |
|---|---:|---:|
| gateway decode | 2,750 | 3,500 |
| in-ring hop | 83 | 1,584 |
| strategy decide | 208 | 541 |
| out-ring hop | 84 | 1,208 |
| encode + send | 542 | 1,583 |
| total | 3,834 | 10,375 |
<!-- END:STAGES -->

## 5. Isolation vs latency

<!-- BEGIN:TRADEOFF -->
**Measured.**

- **Two ring hops** cost 167 ns at p50 (in 83 + out 84). That is 4.4% of Phase 4's 3.8 µs p50 tick-to-trade inside the gateway.
- **Lock-step harness:** Phase 4 vs Phase 3 is +0.12 µs at p50 and +3.17 µs at p99 (the harness round trip includes Python pipe I/O).
- **Flat-out throughput:** 46,946 vs 47,367 snapshots/s (0.99x). At the highest tested paced rate of 40,000 snapshots/s, Phase 4 sustained the full offered rate with 29.96 µs final lag, while Phase 3 achieved 39,943 snapshots/s with 1,369.08 µs final lag.
- **Recovery after SIGKILL:** median 52.9 ms with 0 reconnects; a hang adds the 500 ms heartbeat timeout.
<!-- END:TRADEOFF -->

**What the process boundary buys.**
- **Fault containment.**
  - A strategy crash, hang or bad deploy no longer touches the venue session.
    In the crash test the venue saw one connection throughout.
  - Our resting orders were pulled within milliseconds, by the process that
    owns the socket, without any help from the process that failed.
  - Trading resumed in about the restart delay (50 ms) plus process start-up.
  - In Phase 3 the same bug would have taken the socket, the session and every
    resting order's owner down together.
- **Independent restarts.** A new strategy build can be started (or killed with
  `--no-spawn`) while the gateway stays logged in and keeps its order and
  position state.
- **A narrow, checkable interface.** The only thing the processes share is a
  1.47 MB region of fixed-size PODs. The strategy cannot corrupt the gateway's
  heap, socket or JSON buffers. Everything it says arrives as an `OutMsg` that
  the gateway encodes itself.

**What it costs.**
- **Two ring hops per order,** in and out (§4.1, §4.3), each a
  release/acquire handoff between cores. A 1,344-byte book update moves about
  21 cache lines from the gateway's core to the strategy's core.
- **One copy** of the decoded book into the fixed layout (at most 32 levels a
  side).
- **One more busy core.** Each process spins when idle.
- **Extra machinery:** handshake, heartbeats, sequence numbers, kill switch and
  resync. The single-process bot needs none of it.
- **In lock-step replay,** nothing overlaps, so the full round trip appears
  in every answer.

**Why we keep it.**
- When each spinning thread has a core of its own, a hop costs under 100 ns at p50 on our Mac (§4.1). That is small next to a tick-to-trade dominated by
  microseconds of JSON decoding and WebSocket I/O. The "Measured" list above
  gives the share on our machine.
- The hop becomes expensive when it is not a hop but a wake-up. If the
  receiving thread has yielded its core (idle policy, `IDLE_SPINS`) or shares
  one with other busy threads, the message waits for the OS scheduler: tens of
  µs to ms instead of ns. A process boundary adds two such waiting points; a
  single process has none. So this design needs enough cores, and pinning where
  the OS allows it.
- The gap that remains is detection, not transport: a hung strategy costs up
  to the 500 ms heartbeat timeout. A tighter timeout trades false alarms
  (e.g. a strategy descheduled by the OS) for faster recovery.
- If decoding became sub-microsecond (binary feeds, kernel bypass), the two
  hops would become a large share of the total. Then a single process with
  threads (Phase 3) or core-pinned processes on one L2 would be the better
  trade.
- We rejected Unix sockets and pipes for the hop: each message would cost a
  system call in each process, microseconds rather than nanoseconds.

## 6. Correctness checks

| Check | Result |
|---|---|
| Course CI `shm_ring_test` (fork + mmap, 500k items, FIFO) | pass, <!-- BEGIN:CISHM -->
median 38,250,059 ops/s over 5 runs
<!-- END:CISHM --> |
| Course CI SPSC (Phase 3 header, unchanged) | 100/100 locally |
| `shm_test`: 200k full `InMsg` across fork, zero-copy, byte-checked; consumer SIGKILL + restart; shm_open create/attach | pass |
| `replay_test.py`: the 10 Phase 3 replay scenarios through both processes (queue logic, quota, ordering over 3,000 mixed lines) | pass |
| `resilience_test.py`: SIGKILL and SIGSTOP the strategy live | pass, 3 of 3 runs |
| ThreadSanitizer build of both processes and IXWebSocket: ctest + 40,000-line two-process replay | 0 warnings within each process (TSan cannot see synchronization across processes; that is what `shm_test` checks) |

## 7. Limits

- **Detection time.** A hung strategy is detected only after the 500 ms
  heartbeat timeout; a crash is caught by `waitpid` within the 2 ms poll.
- **Kill-switch cancels.** They are not confirmed (the protocol has none). A
  fill racing with a cancel is still applied to the tracked position.
- **Order matching.** The gateway matches fills to open orders by symbol and
  side, which is exact for our one-lot orders.
- **Depth.** Book updates carry the 32 best levels per side; deeper levels are
  dropped at the gateway. The strategy only uses levels near the touch, and
  Phase 2's band clipped beyond ±2,048 ticks anyway.
- **Pinning.** No thread or process pinning on macOS. `STRATEGY_CORE` pins the
  strategy on Linux.
