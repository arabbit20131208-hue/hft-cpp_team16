# Phase 2

Based on Phase1. Copy `hft/cpp_client` into the team repository.

```bash
cmake -S hft/cpp_client -B build -DCMAKE_BUILD_TYPE=Release -DHFT_USE_TLS=ON
cmake --build build -j 4
ctest --test-dir build --output-on-failure
./build/book_bench
python3 tests/replay_test.py build/hft_bot
```

`book_bench` uses generated snapshots. It compares a map reference with the flat book. It is not a Phase 1 versus Phase 2 venue benchmark.

Use the team's usual connection settings. Set `TICK_SIZE` to the instrument's tick size. The default is 0.01. This version uses one tick size for all symbols. It holds up to 64 symbols, 256 levels per side, and a 4,096-tick range per snapshot. It rejects snapshots outside those limits. The local book represents the depth in the snapshot, not undisclosed venue orders.

The strategy keeps at most one buy and one sell per symbol. Order size is one. It tracks each side by its acknowledged order ID. It retains resting orders. Queue position changes the threshold for quoting the opposite side. Position limit is five per side, including the one pending order on that side. Run in a fresh session with no existing orders or positions. Check current venue event formats before live use.

Optional local connection check (`websockets` Python package required):

```bash
python3 tests/socket_test.py build/hft_bot
```

For a recorded normal tape, use JSONL containing the original snapshot, acknowledgement, queue, and fill messages:

```bash
python3 scripts/measure_tape.py build/hft_bot normal_tape.jsonl normal_latency.json
```

This measures callback time after parsing. It includes local serialization when an order is sent. Profile logging can affect the result. Record a real venue run separately for connectivity and book validation.

Before submission: run the current venue, add the normal-tape numbers to `Phase2_Report.md`, commit the code, and create a tag such as `phase2`. Submit the team's private repository URL at that tag and attach the report. No repository was pushed or tagged here.
