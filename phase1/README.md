Team 16

# Run Phase 1

Copy only this `phase1` folder into the existing Team 16 repository. Original files under `hft`, `scripts` and `tests` stay unchanged.

```bash
cmake -S phase1/cpp_client -B phase1/build_before -DPHASE1_FAST=OFF -DHFT_USE_TLS=ON
cmake --build phase1/build_before -j 4
cmake -S phase1/cpp_client -B phase1/build -DPHASE1_FAST=ON -DHFT_USE_TLS=ON
cmake --build phase1/build -j 4
python3 phase1/bench.py --before phase1/build_before/hft_bot --after phase1/build/hft_bot
```

The control build copies the Phase 0 DOM and map logic and adds stage timing inside the separate client. The original Phase 0 client is not modified. The benchmark imports the original `scripts/latency_replay.py`.

For live play, use `phase1/build/hft_bot` with the same team environment settings. Submit the tagged repository link, `phase1/Phase1_Report.md` and `phase1/tail_hist.png`.
