"""Closed-loop FIFO queue simulator for the bot's --replay mode.

A toy single-symbol CLOB, NOT the AlgoArena engine. It exists to show how
queue_ahead changes the bot's decisions with everything else held fixed:
the same seed gives the same market for QUEUE_AWARE=1 and QUEUE_AWARE=0.

Market model (all in ticks of 0.01):
  * latent imbalance I follows AR(1); the snapshot's obi is I plus noise, and
    the touch sizes are set so the touch imbalance equals obi;
  * the bid moves up with probability 0.12*(1+I) and down with 0.12*(1-I),
    so the signal is predictive but noisy;
  * each step sell flow hits the bid and buy flow lifts the ask (0..30 lots,
    skewed by I); flow consumes the FIFO queue ahead of our order first;
  * new orders keep joining the level behind ours (0..15 lots per step), so
    level_qty = ahead + 1 + behind and our share of the level shrinks over time;
  * our order fills when the queue ahead is exhausted, or when the touch moves
    through our price; a touch moving away leaves it resting, unfilled.
The venue side sends order_ack (queue_ahead = size already at that price),
queue_update when it changes, trade_execution on fill, and no message for a
cancel (matching the real protocol, which has no cancel confirmation).

Markout: mid 20 steps after a fill minus the fill price, signed in our
favour, in ticks. Negative = adverse selection.

Usage: python3 queue_sim.py path/to/hft_bot [--steps 20000] [--seeds 1 2 3]
"""
import argparse
import json
import os
import random
import subprocess

TEAM = 'hft_alpha'
MARKOUT = 20


def run(bot, seed, steps, aware):
    rng = random.Random(seed)
    env = dict(os.environ, QUEUE_AWARE='1' if aware else '0')
    p = subprocess.Popen([bot, '--replay'], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                         stderr=subprocess.PIPE, text=True, bufsize=1, env=env)
    state = dict(bid=10000, imb=0.0)
    orders = {}  # id -> dict(side, price, ahead)
    next_id = [0]
    fills, mids = [], []
    stats = dict(orders=0, cancels=0, fills=0, messages=0)
    pending = []  # venue messages to deliver before the next snapshot

    def send(msg):
        p.stdin.write(json.dumps(msg, separators=(',', ':')) + '\n')
        p.stdin.flush()
        for o in json.loads(p.stdout.readline()):
            handle(o)

    def handle(o):
        stats['messages'] += 1
        if o['type'] == 'place_order':
            stats['orders'] += 1
            next_id[0] += 1
            oid = f'o{next_id[0]}'
            price = round(o['price'] * 100)
            side = o['side']
            touch = state['bid'] if side == 'buy' else state['bid'] + state['gap']
            ahead = state['bq'] if side == 'buy' else state['aq']
            if price != touch:
                ahead = 0
            orders[oid] = dict(side=side, price=price, ahead=ahead, behind=0)
            pending.append(dict(type='order_ack', symbol='SIM', side=side, order_id=oid,
                                price=price / 100, quantity=1, queue_ahead=ahead,
                                level_qty=ahead + 1))
        elif o['type'] == 'cancel_order':
            if orders.pop(o['order_id'], None) is not None:
                stats['cancels'] += 1

    def fill(oid, step):
        o = orders.pop(oid)
        stats['fills'] += 1
        fills.append((step, o['side'], o['price']))
        buyer, seller = (TEAM, 'mm') if o['side'] == 'buy' else ('mm', TEAM)
        pending.append(dict(type='trade_execution', symbol='SIM', quantity=1,
                            price=o['price'] / 100, buyer_id=buyer, seller_id=seller,
                            maker_id=TEAM))

    for step in range(steps):
        # 1. market moves
        state['imb'] = 0.9 * state['imb'] + rng.gauss(0, 0.25)
        imb = max(-0.95, min(0.95, state['imb']))
        r = rng.random()
        if r < 0.12 * (1 + imb):
            state['bid'] += 1
        elif r < 0.12 * (1 + imb) + 0.12 * (1 - imb):
            state['bid'] -= 1
        state['gap'] = 1 if rng.random() < 0.8 else 2
        bid, ask = state['bid'], state['bid'] + state['gap']
        obi = max(-0.9, min(0.9, imb + rng.gauss(0, 0.2)))
        state['bq'] = max(1, round(100 * (1 + obi)))
        state['aq'] = max(1, round(100 * (1 - obi)))
        mids.append((bid + ask) / 2)

        # 2. our resting orders: touch moved through us, or flow ate the queue
        sell_flow = rng.randint(0, 30) if rng.random() < 0.5 * (1 - imb) else 0
        buy_flow = rng.randint(0, 30) if rng.random() < 0.5 * (1 + imb) else 0
        for oid, o in list(orders.items()):
            if o['side'] == 'buy':
                if bid < o['price']:
                    fill(oid, step)
                    continue
                if bid == o['price'] and sell_flow:
                    o['ahead'] -= sell_flow
            else:
                if ask > o['price']:
                    fill(oid, step)
                    continue
                if ask == o['price'] and buy_flow:
                    o['ahead'] -= buy_flow
            if o['ahead'] < 0:
                fill(oid, step)
                continue
            at_touch = (bid if o['side'] == 'buy' else ask) == o['price']
            if at_touch:
                o['behind'] += rng.randint(0, 15)
                pending.append(dict(type='queue_update', symbol='SIM', side=o['side'],
                                    order_id=oid, price=o['price'] / 100,
                                    queue_ahead=o['ahead'],
                                    level_qty=o['ahead'] + 1 + o['behind']))

        # 3. deliver venue messages, then the snapshot
        while pending:
            send(pending.pop(0))
        bids = [[(bid - i) / 100, state['bq'] if i == 0 else 100] for i in range(5)]
        asks = [[(ask + i) / 100, state['aq'] if i == 0 else 100] for i in range(5)]
        bq, aq = state['bq'], state['aq']
        micro = (bid * aq + ask * bq) / (bq + aq) / 100
        send(dict(type='book_snapshot', symbol='SIM', bids=bids, asks=asks,
                  mid_price=(bid + ask) / 200, spread=(ask - bid) / 100,
                  microprice=micro, obi=(bq - aq) / (bq + aq)))

    p.stdin.close()
    summary = [l for l in p.stderr.read().splitlines() if l.startswith('[queue]')]
    p.wait()
    marks = [((mids[s + MARKOUT] - px) if side == 'buy' else (px - mids[s + MARKOUT]))
             for s, side, px in fills if s + MARKOUT < len(mids)]
    stats['markout_ticks'] = sum(marks) / len(marks) if marks else 0.0
    stats['fill_rate'] = stats['fills'] / stats['orders'] if stats['orders'] else 0.0
    stats['bot'] = summary[0] if summary else ''
    return stats


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('bot')
    ap.add_argument('--steps', type=int, default=20000)
    ap.add_argument('--seeds', type=int, nargs='+', default=[1, 2, 3])
    ap.add_argument('--json')
    args = ap.parse_args()
    out = {}
    print('| Policy | Seed | Orders | Cancels | Fills | Fill rate | Markout (ticks/fill) | Msgs/fill |')
    print('|---|---:|---:|---:|---:|---:|---:|---:|')
    for aware in (True, False):
        label = 'queue-aware' if aware else 'queue-blind'
        for seed in args.seeds:
            s = run(args.bot, seed, args.steps, aware)
            out[f'{label}/{seed}'] = s
            print(f"| {label} | {seed} | {s['orders']} | {s['cancels']} | {s['fills']} | "
                  f"{s['fill_rate']:.3f} | {s['markout_ticks']:+.3f} | "
                  f"{s['messages'] / max(1, s['fills']):.2f} |")
    for k, s in out.items():
        print(k, s['bot'])
    if args.json:
        with open(args.json, 'w') as f:
            json.dump(out, f, indent=2)


if __name__ == '__main__':
    main()
