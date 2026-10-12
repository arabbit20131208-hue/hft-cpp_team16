import argparse
import sys
import json
import platform
import subprocess
import tempfile
from pathlib import Path
import matplotlib.pyplot as plt
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/"scripts"))
from latency_replay import benchmark, percentile, synthetic_snapshots, iter_snapshots

parser=argparse.ArgumentParser()
parser.add_argument("--before",required=True)
parser.add_argument("--after",required=True)
parser.add_argument("--tape")
parser.add_argument("--n",type=int,default=2000)
parser.add_argument("--warmup",type=int,default=50)
args=parser.parse_args()
if args.tape:
    snapshots=list(iter_snapshots(Path(args.tape)))[:args.n]
else:
    snapshots=list(synthetic_snapshots(args.n))
if len(snapshots)<=args.warmup:
    raise ValueError("Need more snapshots than warm-up")
output=Path(__file__).resolve().parent
output.mkdir(exist_ok=True)
data="".join(json.dumps(s,separators=(",",":"))+"\n" for s in snapshots)
results={}
orders=[]
for label,exe in [("Phase 0",args.before),("Phase 1",args.after)]:
    replay=benchmark(iter(snapshots),[exe,"--replay"],warmup=args.warmup)
    with tempfile.TemporaryFile(mode="w+") as log:
        run=subprocess.run([exe,"--replay","--profile"],input=data,
                           stdout=subprocess.PIPE,stderr=log,text=True,check=True)
        log.seek(0)
        lines=log.read().splitlines()
    orders.append([json.loads(line) for line in run.stdout.splitlines()])
    if not any(line.startswith("LAT ") for line in lines):
        raise ValueError("No tick-to-order latency samples")
    stages=[list(map(int,line.split()[1:])) for line in lines if line.startswith("STAGE ")]
    if len(stages)!=len(snapshots):
        raise ValueError("A snapshot was rejected by the codec")
    stages=stages[args.warmup:]
    parse=[row[0]/1000 for row in stages]
    serialize=[row[1]/1000 for row in stages if row[2]]
    combined=[(row[0]+row[1])/1000 for row in stages if row[2]]
    results[label]={"parse":parse,"serialize":serialize,"parse+serialize":combined,
                    "replay":replay["latencies_us"]}
if orders[0]!=orders[1]:
    raise ValueError("Phase 0 and Phase 1 produced different orders")
print("Orders match. Latency stamping is enabled in both versions.")
print("Machine:",platform.platform(),platform.machine())
print("Snapshots:",len(snapshots),"Warm-up:",args.warmup)
print("| Metric (us) | Phase 0 p50 | Phase 0 p99 | Phase 1 p50 | Phase 1 p99 |")
print("|---|---:|---:|---:|---:|")
summary={}
for metric in ["parse","serialize","parse+serialize","replay"]:
    values=[]
    for label in results:
        samples=results[label][metric]
        if not samples:
            raise ValueError("No orders after warm-up; choose a tape that triggers the strategy")
        values.extend([percentile(sorted(samples),50),percentile(sorted(samples),99)])
    print(f"| {metric} | "+" | ".join(f"{value:.3f}" for value in values)+" |")
    summary[metric]=dict(zip(["before_p50","before_p99","after_p50","after_p99"],values))
(output/"results.json").write_text(json.dumps(summary,indent=2))
lo=min(percentile(sorted(results[label]["replay"]),90) for label in results)
hi=max(max(results[label]["replay"]) for label in results)
if hi==lo: hi=lo+1
bins=[lo+(hi-lo)*i/30 for i in range(31)]
for label in results:
    plt.hist(results[label]["replay"],bins=bins,histtype="step",label=label)
plt.yscale("log")
plt.xlabel("Replay round-trip (us)")
plt.ylabel("Count")
plt.title("Tail: at or above the smaller p90")
plt.legend()
plt.tight_layout()
plt.savefig(output/"tail_hist.png",dpi=180)
print("Saved",output/"results.json","and",output/"tail_hist.png")
