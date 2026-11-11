"""Queue-aware behaviour checks through the bot's --replay interface.

Usage: python3 replay_test.py path/to/hft_bot
Each scenario feeds identical market data and changes one thing (queue
position, signal strength, touch) to show the decision it drives.
"""
import json
import subprocess
import sys

BOT = sys.argv[1]


def snap(signal, bid=100.0):
    """Two-sided snapshot whose EWMA input equals `signal`."""
    ask = round(bid + 0.02, 2)
    return dict(type='book_snapshot', symbol='ABC', mid_price=round(bid + 0.01, 2), spread=0.02,
                microprice=bid + 0.01 + 0.01 * signal, obi=signal,
                bids=[[bid, 100], [round(bid - 0.01, 2), 80]], asks=[[ask, 100]])


def ack(ahead, side='buy', oid='b1', price=100.0):
    return dict(type='order_ack', symbol='ABC', side=side, order_id=oid, price=price,
                quantity=1, queue_ahead=ahead, level_qty=100)


def queue_update(ahead, oid='b1'):
    return dict(type='queue_update', symbol='ABC', side='buy', order_id=oid, price=100,
                queue_ahead=ahead, level_qty=100)


def fill(side='buy'):
    buyer, seller = ('hft_alpha', 'x') if side == 'buy' else ('x', 'hft_alpha')
    return dict(type='trade_execution', symbol='ABC', quantity=1, price=100,
                buyer_id=buyer, seller_id=seller, maker_id='hft_alpha')


def run(messages, env=None):
    p = subprocess.run([BOT, '--replay'], input=''.join(json.dumps(m) + '\n' for m in messages),
                       capture_output=True, text=True, check=True, env=env)
    return [json.loads(line) for line in p.stdout.splitlines()]


def orders(rows, kind=None, side=None):
    return [o for row in rows for o in row
            if (kind is None or o['type'] == kind) and (side is None or o.get('side') == side)]


def cancels_of(rows, oid):
    return [o for o in orders(rows, 'cancel_order') if o['order_id'] == oid]


# 1. Entry: a strong buy signal posts one buy at the bid.
rows = run([snap(.8)])
assert rows[0] == [dict(type='place_order', team_id='hft_alpha', symbol='ABC', side='buy',
                        order_type='limit', price=100, quantity=1)], rows[0]

# 2. Same tape, signal drifts mildly against the buy (EWMA -> -0.25).
#    Near the front (10/100) the order is held; far back (80/100) it is pulled.
drift = [snap(-.25)] * 20
near = run([snap(.8), ack(10)] + drift)
far = run([snap(.8), ack(80)] + drift)
assert not cancels_of(near, 'b1'), 'near-front order should be held'
assert len(cancels_of(far, 'b1')) == 1, 'far-back order should be cancelled once'

# 3. Strongly adverse signal: pulled even from the front.
strong = run([snap(.8), ack(0)] + [snap(-.9)] * 10)
assert len(cancels_of(strong, 'b1')) == 1

# 4. Touch moves up a tick: cancel, wait out the grace period, rejoin at 100.01.
up = run([snap(.8), ack(10)] + [snap(.8, bid=100.01)] * 4)
assert len(cancels_of(up, 'b1')) == 1
assert any(o['price'] == 100.01 for o in orders(up, 'place_order', 'buy'))

# 5. queue_update must match the order ID. Moving b1 from the front to the back
#    turns the held order into a cancel on the next mildly adverse tick.
base = [snap(.8), ack(10)] + drift
wrong = run(base + [queue_update(80, oid='other'), snap(-.25)])
right = run(base + [queue_update(80), snap(-.25)])
assert not cancels_of(wrong, 'b1')
assert cancels_of(right[-1:], 'b1')

# 6. Position limit: five one-lot buy fills, then no sixth buy.
msgs = []
for i in range(6):
    msgs += [snap(.8), ack(10, oid=f'b{i}'), fill('buy')]
assert len(orders(run(msgs), 'place_order', 'buy')) == 5

# 7. No ack within the timeout: the side returns to Idle and re-posts.
rows = run([snap(.8)] * 7)
assert len(orders(rows, 'place_order', 'buy')) == 2

# 8. Queue-blind build (QUEUE_AWARE=0) pulls the near-front order too.
import os
blind = run([snap(.8), ack(10)] + drift, env=dict(os.environ, QUEUE_AWARE='0'))
assert len(cancels_of(blind, 'b1')) == 1

print('replay_test: entry, near/far hold-vs-cancel, strong adverse, touch requote, '
      'order-ID matching, position limit, ack timeout, queue-blind control passed.')
