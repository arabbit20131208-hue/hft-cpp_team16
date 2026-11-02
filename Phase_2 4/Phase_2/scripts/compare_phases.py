"""Tick-to-order comparison on one tape with the course harness.

Runs scripts/latency_replay.py's benchmark() (pipe round trip per snapshot,
the course's offline grading tool) on each binary, several rounds in
alternating order, and reports the median round of each percentile. Then runs
the last binary once with --profile for an in-process stage breakdown and its
[book]/[queue] summaries.

Usage:
  python3 compare_phases.py TAPE.jsonl out.json LABEL=BIN [LABEL=BIN ...]
"""
import json
import pathlib
import platform
import statistics
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts'))
from latency_replay import benchmark, iter_snapshots, percentile  # noqa: E402

ROUNDS, WARMUP = 5, 200
tape, out_path, *pairs = sys.argv[1:]
bots = [p.split('=', 1) for p in pairs]
snaps = list(iter_snapshots(pathlib.Path(tape)))

rounds = {label: [] for label, _ in bots}
for r in range(ROUNDS):
    order = bots if r % 2 == 0 else bots[::-1]
    for label, exe in order:
        res = benchmark(iter(snaps), [exe, '--replay'], warmup=WARMUP)
        rounds[label].append(res['summary'])

result = dict(machine=f'{platform.platform()} {platform.processor() or platform.machine()}',
              tape=pathlib.Path(tape).name, snapshots=len(snaps), warmup=WARMUP, rounds=ROUNDS,
              harness_us={})
print(f"{len(snaps)} snapshots, warm-up {WARMUP}, median of {ROUNDS} rounds (us, pipe round trip)")
print('| Build | p50 | p90 | p99 | p99.9 |')
print('|---|---:|---:|---:|---:|')
for label, _ in bots:
    row = {k: statistics.median(s[k] for s in rounds[label]) for k in ('p50', 'p90', 'p99', 'p999')}
    result['harness_us'][label] = row
    print(f"| {label} | {row['p50']:.2f} | {row['p90']:.2f} | {row['p99']:.2f} | {row['p999']:.2f} |")

# In-process breakdown for the last binary (Phase 2 final).
label, exe = bots[-1]
data = ''.join(json.dumps(s, separators=(',', ':')) + '\n' for s in snaps)
p = subprocess.run([exe, '--replay', '--profile'], input=data, capture_output=True, text=True,
                   check=True)
prof = [list(map(int, l.split()[1:])) for l in p.stderr.splitlines() if l.startswith('PROF ')]
prof = prof[WARMUP:]
lat = sorted(float(l.split()[2]) for l in p.stderr.splitlines() if l.startswith('LAT '))
def pct(xs, q):
    xs = sorted(xs)
    return percentile(xs, q)
stages = {
    'decode_ns': [r[0] for r in prof],
    'hot_ns (book+signal+strategy+encode+send)': [r[1] for r in prof],
    'encode_ns (ticks with orders)': [r[2] for r in prof if r[2]],
}
result['in_process'] = {k: dict(p50=pct(v, 50), p99=pct(v, 99), n=len(v)) for k, v in stages.items()}
result['tick_to_order_us'] = dict(p50=pct(lat, 50), p99=pct(lat, 99), n=len(lat))
result['summaries'] = [l for l in p.stderr.splitlines() if l.startswith(('[book]', '[queue]'))]
print(f'\n{label} in-process (profiling on):')
for k, v in result['in_process'].items():
    print(f"  {k}: p50={v['p50']:.0f} p99={v['p99']:.0f} (n={v['n']})")
t = result['tick_to_order_us']
print(f"  tick-to-order LAT: p50={t['p50']:.3f} us p99={t['p99']:.3f} us (n={t['n']})")
for l in result['summaries']:
    print(' ', l)
pathlib.Path(out_path).write_text(json.dumps(result, indent=2) + '\n')
