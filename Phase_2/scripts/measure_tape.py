import json
import statistics
import subprocess
import sys
from pathlib import Path

binary, tape, output = sys.argv[1:]
with open(tape) as f:
    p = subprocess.run([binary, '--replay', '--profile'], stdin=f,
                       stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
                       text=True, check=True)
values = [int(line.split()[1]) for line in p.stderr.splitlines() if line.startswith('HOT_NS ')]
values = values[50:]
if not values:
    raise SystemExit('Not enough valid snapshots. Supply a JSONL tape with more than 50 snapshots.')
values.sort()
result = dict(snapshots=len(values), warmup=50, p50_ns=statistics.median(values),
              p99_ns=values[min(len(values)-1, int(.99*len(values)))],
              scope='on_depth + on_book + local order serialization; excludes parsing and network latency')
Path(output).write_text(json.dumps(result, indent=2)+'\n')
print(json.dumps(result, indent=2))
