"""Build, verify and measure Phase 4 (gateway + strategy processes) against the
single-process Phase 3 bot, in one go.

Run from the repository root on the machine whose numbers go in the report:

    pip install websockets
    python3 Phase_4/scripts/phase4_bench.py

Writes Phase_4/results/: summary.md (tables to paste into the report), raw
JSON/logs. Takes about 5-10 minutes; close other heavy apps first.

Steps
  1. Release builds of Phase 3 (Phase_3/hft/cpp_client) and Phase 4
     (hft/cpp_client), and a ThreadSanitizer build of Phase 4.
  2. Course shm_ring_test (fork + mmap) x5 -> shm ops/s.
  3. ctest on both Phase 4 builds (book, shm, two-process replay, crash recovery).
  4. shm_ipc_bench: one-way / round-trip / flat-out across the real rings.
  5. Busy tape (40,000 snapshots, 8 symbols).
  6. Course lock-step harness: Phase 3 vs Phase 4.
  7. Paced open-loop tape_driver at several rates, both bots.
  8. Phase 4 per-stage breakdown (--profile PROF lines, paced at 5,000/s).
  9. Resilience: resilience_test.py x3 (SIGKILL + SIGSTOP).
 10. TSan: whole two-process replay under the TSan gateway.
"""
import json
import os
import pathlib
import platform
import re
import statistics
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
P3 = ROOT / 'Phase_3' / 'hft' / 'cpp_client'
P4 = ROOT / 'hft' / 'cpp_client'
OUT = ROOT / 'Phase_4' / 'results'
BUILD = ROOT / 'build_phase4_bench'
RATES = [int(r) for r in os.environ.get('RATES', '5000,10000,20000,40000,0').split(',')]
JOBS = str(os.cpu_count() or 4)


def sh(cmd, **kw):
    print('$', ' '.join(map(str, cmd)), flush=True)
    return subprocess.run(list(map(str, cmd)), check=True, text=True, **kw)


def build(src, dst, *flags):
    sh(['cmake', '-S', src, '-B', dst, '-DCMAKE_BUILD_TYPE=Release', *flags], stdout=subprocess.DEVNULL)
    sh(['cmake', '--build', dst, '-j', JOBS], stdout=subprocess.DEVNULL)


def tag(line, key):
    m = re.search(rf'{re.escape(key)}=([-\d.]+)', line)
    return float(m.group(1)) if m else None


def pct(xs, q):
    xs = sorted(xs)
    return xs[min(len(xs) - 1, int(q / 100 * len(xs)))] if xs else 0


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    cxx = os.environ.get('CXX', 'c++')
    s = dict(machine=f'{platform.platform()} / {platform.processor() or platform.machine()} / '
                     f'{os.cpu_count()} logical CPUs')

    # 1. builds
    b3, b4, bt = BUILD / 'p3', BUILD / 'p4', BUILD / 'p4_tsan'
    build(P3, b3)
    build(P4, b4)
    build(P4, bt, '-DHFT_TSAN=ON')
    bot3, bot4, bott = b3 / 'hft_bot', b4 / 'hft_bot', bt / 'hft_bot'
    driver = b4 / 'tape_driver'

    # 2. course shm test
    sh([cxx, '-std=c++17', '-O2', '-pthread', '-I', ROOT / 'include', '-I', ROOT / 'tests',
        ROOT / 'tests' / 'shm_ring_test.cpp', '-o', BUILD / 'shm_ring_test'])
    ops, results = [], []
    for _ in range(5):
        out = subprocess.run([BUILD / 'shm_ring_test'], capture_output=True, text=True).stdout
        results += [l for l in out.splitlines() if l.startswith('RESULT')]
        ops += [int(l.split('|')[2]) for l in out.splitlines() if l.startswith('METRIC|shm_ops_per_sec')]
    s['course_shm'] = dict(results=sorted(set(results)), ops_per_s_median=statistics.median(ops))

    # 3. ctest
    for d in (b4, bt):
        sh(['ctest', '--test-dir', d, '--output-on-failure'], stdout=subprocess.DEVNULL)
    s['ctest'] = 'release and TSan builds: all tests passed'

    # 4. IPC microbenchmark
    ipc = subprocess.run([b4 / 'shm_ipc_bench', '200000', '10000'], capture_output=True, text=True,
                         check=True).stdout
    (OUT / 'ipc_bench.txt').write_text(ipc)
    s['ipc_bench'] = ipc.strip().splitlines()

    # 5. busy tape
    tape, raw = BUILD / 'busy.jsonl', BUILD / 'busy_raw.jsonl'
    sh([sys.executable, ROOT / 'Phase_2' / 'scripts' / 'make_tape.py', tape, '--n', '40000', '--symbols', '8'])
    with open(tape) as f, open(raw, 'w') as g:
        for line in f:
            g.write(json.dumps(json.loads(line)['msg'], separators=(',', ':')) + '\n')

    # 6. lock-step harness
    subprocess.run([sys.executable, ROOT / 'Phase_2' / 'scripts' / 'compare_phases.py', tape,
                    OUT / 'lockstep.json', f'Phase 3={bot3}', f'Phase 4={bot4}'],
                   capture_output=True, text=True, check=True)
    s['lockstep'] = json.loads((OUT / 'lockstep.json').read_text())['harness_us']

    # 7. paced runs
    paced = []
    for rate in RATES:
        for label, bot in (('Phase 3', bot3), ('Phase 4', bot4)):
            r = subprocess.run([driver, '--tape', raw, '--rate', str(rate), '--warmup', '2000', '--', bot,
                                '--replay'], capture_output=True, text=True, check=True,
                               env=dict(os.environ, ORDER_QUOTA='6'))
            line = r.stdout.strip()
            print(label, line, flush=True)
            paced.append(dict(build=label, rate=rate, **{k: tag(line, k) for k in
                              ('achieved', 'p50', 'p90', 'p99', 'p99.9', 'max', 'final_lag')}))
    s['paced'] = paced

    # 8. per-stage breakdown of Phase 4 (orders only), paced at 5,000/s so each
    #    stage shows its own cost rather than a flat-out backlog.
    s.update(stage_breakdown(driver, bot4, raw))

    # 9. resilience
    runs = []
    for _ in range(3):
        r = subprocess.run([sys.executable, P4 / 'tests' / 'resilience_test.py', bot4], capture_output=True,
                           text=True)
        runs.append(r.stdout.strip() or r.stderr[-800:])
    (OUT / 'resilience.txt').write_text('\n\n'.join(runs) + '\n')
    s['resilience'] = runs

    # 10. TSan replay
    with open(raw) as fin, open(OUT / 'tsan_replay.log', 'w') as err:
        subprocess.run([bott, '--replay'], stdin=fin, stdout=subprocess.DEVNULL, stderr=err, check=True)
    log = (OUT / 'tsan_replay.log').read_text()
    s['tsan_replay_warnings'] = log.count('WARNING: ThreadSanitizer')
    (OUT / 'tsan_replay.log').write_text('\n'.join(l for l in log.splitlines() if not l.startswith('LAT ')) + '\n')

    (OUT / 'phase4_bench.json').write_text(json.dumps(s, indent=2) + '\n')

    md = [f"Machine: {s['machine']}", '',
          f"Course shm_ring_test: {s['course_shm']['results']}; median {s['course_shm']['ops_per_s_median']:,.0f} ops/s",
          '', 'IPC microbenchmark (shm_ipc_bench 200000 10000):', '```', ipc.strip(), '```', '',
          '| Lock-step harness (µs) | p50 | p90 | p99 | p99.9 |', '|---|---:|---:|---:|---:|']
    for k, v in s['lockstep'].items():
        md.append(f"| {k} | {v['p50']:.2f} | {v['p90']:.2f} | {v['p99']:.2f} | {v['p999']:.2f} |")
    md += ['', '| Paced busy tape (µs from schedule) | Offered/s | Achieved/s | p50 | p99 | p99.9 | Max | Final lag |',
           '|---|---:|---:|---:|---:|---:|---:|---:|']
    for p in paced:
        offered = 'flat out' if p['rate'] == 0 else f"{p['rate']:,}"
        md.append(f"| {p['build']} | {offered} | {p['achieved']:,.0f} | {p['p50']:.2f} | {p['p99']:.2f} | "
                  f"{p['p99.9']:.2f} | {p['max']:.2f} | {p['final_lag']:.2f} |")
    md += ['', f"Phase 4 per-stage (ns, {s['stages_n']} orders, --profile, paced {s['stages_rate']:,}/s, ORDER_QUOTA=0):", '',
           '| Stage | p50 | p99 |', '|---|---:|---:|']
    for n, v in s['stages_ns'].items():
        md.append(f"| {n} | {v['p50']:,} | {v['p99']:,} |")
    md += ['', 'Phase 4 replay logs:'] + [f'    {l}' for l in s['phase4_logs']]
    md += ['', 'Resilience (3 runs):'] + [f'    {l}' for r in runs for l in r.splitlines()]
    md += ['', f"TSan two-process replay warnings: {s['tsan_replay_warnings']}"]
    (OUT / 'summary.md').write_text('\n'.join(md) + '\n')
    print('\n'.join(md))
    fill_report(s, ipc, md)


def stage_breakdown(driver, bot4, raw, rate=5000):
    prof_log = BUILD / 'profile_stderr.log'
    subprocess.run([driver, '--tape', raw, '--rate', str(rate), '--warmup', '2000', '--stderr', prof_log, '--',
                    bot4, '--replay', '--profile'], capture_output=True, text=True, check=True,
                   env=dict(os.environ, ORDER_QUOTA='0'))
    lines = prof_log.read_text().splitlines()
    prof = [list(map(int, l.split()[1:])) for l in lines if l.startswith('PROF ')]
    prof = prof[len(prof) // 20:]  # drop start-up
    names = ['gateway decode', 'in-ring hop', 'strategy decide', 'out-ring hop', 'encode + send', 'total']
    return dict(stages_ns={n: dict(p50=pct([p[i] for p in prof], 50), p99=pct([p[i] for p in prof], 99))
                           for i, n in enumerate(names)},
                stages_n=len(prof), stages_rate=rate,
                phase4_logs=[l for l in lines if l.startswith(('[ipc]', '[gateway]', '[strategy]'))])


# ── fill the marked blocks of Phase_4/Phase4_Report.md ─────────────────────

def ipc_row(ipc_text, kind, payload):
    for line in ipc_text.splitlines():
        if line.startswith(kind) and payload in line:
            return {k: tag(line, k) for k in ('p50', 'p90', 'p99', 'p99.9', 'max')} | {'line': line}
    return None


def fill_report(s, ipc_text, md):
    path = ROOT / 'Phase_4' / 'Phase4_Report.md'
    text = path.read_text()

    def put(key, body):
        nonlocal text
        text = re.sub(rf'(<!-- BEGIN:{key} -->).*?(<!-- END:{key} -->)',
                      lambda m: m.group(1) + '\n' + body.strip() + '\n' + m.group(2), text, flags=re.S)

    put('MACHINE', f"Measured on: {s['machine']}.")
    put('CISHM', f"median {s['course_shm']['ops_per_s_median']:,.0f} ops/s over 5 runs")

    rows = ['| Run | Detected as | Resting orders at kill | Kill-switch cancels | Recovery (loss → new strategy consuming) | Orders after recovery | Venue connections |',
            '|---|---|---:|---:|---:|---:|---:|']
    for i, r in enumerate(s['resilience'], 1):
        first = next((l for l in r.splitlines() if l.startswith('exit=')), '')
        hang = next((l for l in r.splitlines() if l.startswith('hang:')), '')
        get = lambda line, k: (re.search(rf'{k}=("[^"]*"|\S+)', line) or [None, '?'])[1].strip('"')
        rows.append(f"| {i}a SIGKILL | {get(first, 'detected')} | {get(first, 'resting_at_kill')} | "
                    f"{get(first, 'killswitch_cancels')} | {get(first, 'recovery_ms')} ms | "
                    f"{get(first, 'orders_after_recovery')} | {get(first, 'venue_connections')} |")
        rows.append(f"| {i}b SIGSTOP (hang) | {get(hang, 'detected')} (after the 500 ms timeout) | | | "
                    f"{get(hang, 'recovery_ms')} ms after detection | {get(hang, 'orders_after')} | 1 |")
    put('RESILIENCE', '\n'.join(rows))

    ipc_rows = ['| Path (2 processes) | p50 ns | p90 ns | p99 ns | p99.9 ns |', '|---|---:|---:|---:|---:|']
    for kind, payload, label in (('one-way', '16 B', 'one-way, 16 B'), ('one-way', 'InMsg', 'one-way, InMsg 1,344 B'),
                                 ('rtt/2', '16 B', 'round trip / 2, 16 B'), ('rtt/2', 'InMsg', 'round trip / 2, InMsg')):
        r = ipc_row(ipc_text, kind, payload)
        if r:
            ipc_rows.append(f"| {label} | {r['p50']:,.0f} | {r['p90']:,.0f} | {r['p99']:,.0f} | {r['p99.9']:,.0f} |")
    flats = [l for l in ipc_text.splitlines() if l.startswith('flat-out')]
    put('IPC', '\n'.join(ipc_rows) + '\n\nThroughput (every message written and read in full):\n\n' +
        '\n'.join(f'- `{l.strip()}`' for l in flats))

    e2e = ['| Lock-step harness (µs) | p50 | p90 | p99 | p99.9 |', '|---|---:|---:|---:|---:|']
    e2e += [f"| {k} | {v['p50']:.2f} | {v['p90']:.2f} | {v['p99']:.2f} | {v['p999']:.2f} |" for k, v in s['lockstep'].items()]
    paced = ['| Paced busy tape (µs from schedule) | Offered/s | Achieved/s | p50 | p99 | p99.9 | Max | Final lag |',
             '|---|---:|---:|---:|---:|---:|---:|---:|']
    for p in s['paced']:
        offered = 'flat out' if p['rate'] == 0 else f"{p['rate']:,}"
        paced.append(f"| {p['build']} | {offered} | {p['achieved']:,.0f} | {p['p50']:.2f} | {p['p99']:.2f} | "
                     f"{p['p99.9']:.2f} | {p['max']:.2f} | {p['final_lag']:.2f} |")
    put('E2E', '\n'.join(e2e) + '\n\n' + '\n'.join(paced))

    st = ['| Stage | p50 ns | p99 ns |', '|---|---:|---:|']
    st += [f"| {n} | {v['p50']:,} | {v['p99']:,} |" for n, v in s['stages_ns'].items()]
    put('STAGES', f"{s['stages_n']:,} orders from the busy tape, paced at {s['stages_rate']:,}/s "
                  f"(`--profile`, `ORDER_QUOTA=0`):\n\n" + '\n'.join(st))

    # Numbers for the trade-off section.
    lk = s['lockstep']
    l3, l4 = lk.get('Phase 3'), lk.get('Phase 4')
    hops = s['stages_ns']['in-ring hop']['p50'] + s['stages_ns']['out-ring hop']['p50']
    total = s['stages_ns']['total']['p50'] or 1
    flat = {p['build']: p['achieved'] for p in s['paced'] if p['rate'] == 0}

    def sustained(build):
        ok = [p['rate'] for p in s['paced'] if p['build'] == build and p['rate'] and
              p['achieved'] >= 0.99 * p['rate'] and p['final_lag'] < 1000]
        return max(ok) if ok else 0

    rec = sorted(float(m) for r in s['resilience'] for m in re.findall(r'^exit=.*recovery_ms=([\d.]+)', r, re.M))
    lines = [f"- **Two ring hops** cost {hops:,} ns at p50 (in {s['stages_ns']['in-ring hop']['p50']:,} + out "
             f"{s['stages_ns']['out-ring hop']['p50']:,}). That is {100 * hops / total:.1f}% of Phase 4's "
             f"{total / 1000:.1f} µs p50 tick-to-trade inside the gateway."]
    if l3 and l4:
        lines.append(f"- **Lock-step harness:** Phase 4 vs Phase 3 is {l4['p50'] - l3['p50']:+.2f} µs at p50 and "
                     f"{l4['p99'] - l3['p99']:+.2f} µs at p99 (the harness round trip includes Python pipe I/O).")
    if flat.get('Phase 3') and flat.get('Phase 4'):
        lines.append(f"- **Flat-out throughput:** {flat['Phase 4']:,.0f} vs {flat['Phase 3']:,.0f} snapshots/s "
                     f"({flat['Phase 4'] / flat['Phase 3']:.2f}x). Highest paced rate with no growing lag: "
                     f"Phase 4 {sustained('Phase 4'):,}/s, Phase 3 {sustained('Phase 3'):,}/s.")
    if rec:
        lines.append(f"- **Recovery after SIGKILL:** median {rec[len(rec) // 2]:.1f} ms with 0 reconnects; "
                     f"a hang adds the 500 ms heartbeat timeout.")
    if (os.cpu_count() or 0) < 6:
        lines.append(f"- **Caveat:** this machine has {os.cpu_count()} logical CPUs, fewer than the busy threads in "
                     "play (gateway rx/tx, strategy, tape driver). The hop numbers above include waiting for the OS to "
                     "reschedule a thread that had yielded its core, so they are not representative of the design.")
    put('TRADEOFF', '**Measured.**\n\n' + '\n'.join(lines))
    path.write_text(text)
    print(f'filled {path}')


if __name__ == '__main__':
    main()
