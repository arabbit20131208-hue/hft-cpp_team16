"""Generate a 'normal' synthetic session tape in the course recording format.

Each line is {"msg": <book_snapshot>}, which scripts/latency_replay.py reads.
Random walk per symbol, 10 levels per side, one-tick moves on ~20% of
snapshots, venue-style mid_price/spread/microprice/obi included.

Usage: python3 make_tape.py out.jsonl [--n 20000] [--symbols 5] [--seed 7]
"""
import argparse
import json
import random

ap = argparse.ArgumentParser()
ap.add_argument('out')
ap.add_argument('--n', type=int, default=20000)
ap.add_argument('--symbols', type=int, default=5)
ap.add_argument('--seed', type=int, default=7)
args = ap.parse_args()

rng = random.Random(args.seed)
names = ['NVDA', 'AMD', 'AAPL', 'MSFT', 'TSLA', 'META', 'AMZN', 'GOOG'][:args.symbols]
bid = {s: 10000 + 2500 * i for i, s in enumerate(names)}  # in ticks of 0.01
with open(args.out, 'w') as f:
    for k in range(args.n):
        s = names[k % len(names)]
        r = rng.random()
        bid[s] += -1 if r < 0.1 else 1 if r < 0.2 else 0
        b = bid[s]
        a = b + (1 if rng.random() < 0.7 else 2)
        bids = [[round((b - i) / 100, 2), rng.randint(1, 200)] for i in range(10) if i == 0 or rng.random() > 0.1]
        asks = [[round((a + i) / 100, 2), rng.randint(1, 200)] for i in range(10) if i == 0 or rng.random() > 0.1]
        bq, aq = bids[0][1], asks[0][1]
        bp, ap_ = bids[0][0], asks[0][0]
        msg = dict(type='book_snapshot', symbol=s, bids=bids, asks=asks,
                   mid_price=round((bp + ap_) / 2, 4), spread=round(ap_ - bp, 4),
                   ref_price=round((bp + ap_) / 2, 4),
                   microprice=round((bp * aq + ap_ * bq) / (bq + aq), 6),
                   obi=round((bq - aq) / (bq + aq), 6), asset_type='equity')
        f.write(json.dumps({'msg': msg}, separators=(',', ':')) + '\n')
