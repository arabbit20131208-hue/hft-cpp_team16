"""Build, verify and measure Phase 3 against Phase 2 in one go.

Run from the repository root on the machine whose numbers go in the report
(needs >= 4 free cores for a fair pipeline measurement):

    python3 Phase_3/scripts/phase3_bench.py

Steps (outputs land in Phase_3/results/):
  1. Release builds of Phase 2 and Phase 3, a ThreadSanitizer build of Phase 3.
  2. Course SPSC tests (correctness, concurrency, TSan) against include/spsc_ring.hpp.
  3. ctest for both TSan and Release Phase 3 builds.
  4. TSan proof: the whole bot under TSan on a busy tape (replay) and over a
     local WebSocket stress test (live path) -> tsan_replay.log, tsan_live.log.
  5. Busy tape (make_tape.py, 40,000 snapshots, 8 symbols).
  6. Course lock-step harness (latency_replay.py) Phase 2 vs Phase 3.
  7. Paced open-loop runs (tape_driver) at several rates and flat-out, both bots.
  8. order_quota=6 run: messages per window must stay <= 6.
Writes summary.md (tables to paste into the report) and raw JSON.
"""
import json
import os
import pathlib
import platform
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
P2 = ROOT / 'Phase_2' / 'hft' / 'cpp_client'
P3 = ROOT / 'Phase_3' / 'hft' / 'cpp_client'
OUT = ROOT / 'Phase_3' / 'results'
BUILD = ROOT / 'build_phase3_bench'
RATES = [int(r) for r in os.environ.get('RATES', '5000,10000,20000,40000,0').split(',')]  # 0 = flat out
JOBS = str(os.cpu_count() or 4)


def sh(cmd, **kw):
    print('$', ' '.join(map(str, cmd)), flush=True)
    return subprocess.run(list(map(str, cmd)), check=True, text=True, **kw)


def build(src, dst, *flags):
    sh(['cmake', '-S', src, '-B', dst, '-DCMAKE_BUILD_TYPE=Release', *flags], stdout=subprocess.DEVNULL)
    sh(['cmake', '--build', dst, '-j', JOBS], stdout=subprocess.DEVNULL)


def tag(line, key):
    m = re.search(rf'{key}=([-\d.]+)', line)
    return float(m.group(1)) if m else None


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    cxx = os.environ.get('CXX', 'c++')
    summary = dict(machine=f'{platform.platform()} / {platform.processor() or platform.machine()} / '
                           f'{os.cpu_count()} logical CPUs')

    # 1. builds
    b2, b3, bt = BUILD / 'p2', BUILD / 'p3', BUILD / 'p3_tsan'
    build(P2, b2)
    build(P3, b3)
    build(P3, bt, '-DHFT_TSAN=ON')
    bot2, bot3, bott, driver = b2 / 'hft_bot', b3 / 'hft_bot', bt / 'hft_bot', b3 / 'tape_driver'

    # 2. course SPSC tests
    inc, tdir = ROOT / 'include', ROOT / 'tests'
    spsc = {}
    sh([cxx, '-std=c++17', '-O2', '-pthread', '-I', inc, tdir / 'spsc_correctness.cpp', '-o', BUILD / 'spsc_corr'])
    sh([cxx, '-std=c++17', '-O2', '-pthread', '-I', inc, tdir / 'spsc_concurrency.cpp', '-o', BUILD / 'spsc_conc'])
    sh([cxx, '-std=c++17', '-O1', '-g', '-pthread', '-fsanitize=thread', '-I', inc,
        tdir / 'spsc_concurrency.cpp', '-o', BUILD / 'spsc_tsan'])
    r = subprocess.run([BUILD / 'spsc_corr'], capture_output=True, text=True)
    spsc['correctness'] = [l for l in r.stdout.splitlines() if l.startswith('RESULT')]
    thr = []
    for _ in range(5):
        r = subprocess.run([BUILD / 'spsc_conc'], capture_output=True, text=True)
        spsc.setdefault('concurrency', [l for l in r.stdout.splitlines() if l.startswith('RESULT')])
        thr += [int(l.split()[1]) for l in r.stdout.splitlines() if l.startswith('THROUGHPUT')]
    spsc['throughput_ops_per_s_median_of_5'] = sorted(thr)[len(thr) // 2]
    r = subprocess.run([BUILD / 'spsc_tsan'], capture_output=True, text=True,
                       env=dict(os.environ, SPSC_N='200000'))
    (OUT / 'tsan_spsc_course.log').write_text(r.stdout + r.stderr)
    spsc['tsan_warnings'] = (r.stdout + r.stderr).count('ThreadSanitizer')
    summary['spsc'] = spsc

    # 3. ctest
    for d in (bt, b3):
        sh(['ctest', '--test-dir', d, '--output-on-failure'], stdout=subprocess.DEVNULL)
    summary['ctest'] = 'release and tsan builds: all tests passed'

    # 5. tapes
    tape = BUILD / 'busy.jsonl'
    raw = BUILD / 'busy_raw.jsonl'
    sh([sys.executable, ROOT / 'Phase_2' / 'scripts' / 'make_tape.py', tape, '--n', '40000', '--symbols', '8'])
    with open(tape) as f, open(raw, 'w') as g:
        for line in f:
            g.write(json.dumps(json.loads(line)['msg'], separators=(',', ':')) + '\n')

    # 4. TSan proof on the whole bot
    with open(raw) as fin, open(OUT / 'tsan_replay.log', 'w') as err:
        subprocess.run([bott, '--replay'], stdin=fin, stdout=subprocess.DEVNULL, stderr=err, check=True)
    log = (OUT / 'tsan_replay.log').read_text()
    summary['tsan_bot_replay'] = dict(warnings=log.count('ThreadSanitizer'),
                                      pipeline=[l for l in log.splitlines() if l.startswith('[pipeline]')])
    try:
        import websockets  # noqa: F401
        r = subprocess.run([sys.executable, P3 / 'tests' / 'socket_stress.py', bott, OUT / 'tsan_live.log', '5000'],
                           capture_output=True, text=True)
        summary['tsan_bot_live'] = r.stdout.strip() or r.stderr[-500:]
    except ImportError:
        summary['tsan_bot_live'] = 'skipped: pip install websockets'

    # 6. course lock-step harness
    r = subprocess.run([sys.executable, ROOT / 'Phase_2' / 'scripts' / 'compare_phases.py', tape,
                        OUT / 'lockstep.json', f'Phase 2={bot2}', f'Phase 3={bot3}'],
                       capture_output=True, text=True, check=True)
    summary['lockstep'] = json.loads((OUT / 'lockstep.json').read_text())['harness_us']

    # 7. paced runs
    paced = []
    for rate in RATES:
        for label, bot in (('Phase 2', bot2), ('Phase 3', bot3)):
            r = subprocess.run([driver, '--tape', raw, '--rate', str(rate), '--warmup', '2000', '--', bot, '--replay'],
                               capture_output=True, text=True, check=True,
                               env=dict(os.environ, ORDER_QUOTA='6'))
            line = r.stdout.strip()
            print(label, line, flush=True)
            paced.append(dict(build=label, rate=rate, **{k: tag(line, k) for k in
                              ('achieved', 'p50', 'p90', 'p99', 'p99.9', 'max', 'final_lag')}))
    summary['paced'] = paced

    # 8. quota
    with open(raw) as fin:
        r = subprocess.run([bot3, '--replay'], stdin=fin, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
                           text=True, check=True, env=dict(os.environ, ORDER_QUOTA='6', ORDER_QUOTA_MS='1000'))
    summary['quota'] = [l for l in r.stderr.splitlines() if l.startswith(('[quota]', '[pipeline]'))]

    (OUT / 'phase3_bench.json').write_text(json.dumps(summary, indent=2) + '\n')

    # summary.md
    md = [f"Machine: {summary['machine']}", '',
          f"SPSC course tests: {len(spsc['correctness'])} correctness lines, concurrency "
          f"{spsc['concurrency']}, throughput {spsc['throughput_ops_per_s_median_of_5']:,} ops/s, "
          f"TSan warnings {spsc['tsan_warnings']}", '',
          f"Whole-bot TSan (replay): warnings {summary['tsan_bot_replay']['warnings']}; "
          f"live: {summary['tsan_bot_live']}", '',
          '| Lock-step harness (µs) | p50 | p90 | p99 | p99.9 |', '|---|---:|---:|---:|---:|']
    for k, v in summary['lockstep'].items():
        md.append(f"| {k} | {v['p50']:.2f} | {v['p90']:.2f} | {v['p99']:.2f} | {v['p999']:.2f} |")
    md += ['', '| Paced (µs from schedule) | offered/s | achieved/s | p50 | p99 | p99.9 | max | final lag |',
           '|---|---:|---:|---:|---:|---:|---:|---:|']
    for p in paced:
        md.append(f"| {p['build']} | {'flat out' if p['rate'] == 0 else p['rate']} | {p['achieved']:.0f} | "
                  f"{p['p50']:.2f} | {p['p99']:.2f} | {p['p99.9']:.2f} | {p['max']:.2f} | {p['final_lag']:.2f} |")
    md += ['', 'Quota run:'] + [f'    {l}' for l in summary['quota']]
    (OUT / 'summary.md').write_text('\n'.join(md) + '\n')
    print('\n'.join(md))


if __name__ == '__main__':
    main()
