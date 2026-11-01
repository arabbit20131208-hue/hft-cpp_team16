import json
import subprocess
import sys

binary = sys.argv[1]

def snapshot(value):
    return dict(type='book_snapshot', symbol='ABC', mid_price=100.01,
                microprice=100.01 + .01*value, obi=value,
                bids=[[100, 100], [99.99, 80]], asks=[[100.02, 100]])

def run(messages):
    p = subprocess.run([binary, '--replay'], input=''.join(json.dumps(m)+'\n' for m in messages),
                       capture_output=True, text=True, check=True)
    return [json.loads(line) for line in p.stdout.splitlines()]

def ack(ahead):
    return dict(type='order_ack', symbol='ABC', side='buy', order_id='b1',
                price=100, quantity=1, queue_ahead=ahead, level_qty=100)

# Same market data. Only the FIFO position differs.
near = run([snapshot(.8), ack(10)] + [snapshot(-.25)]*20)
far = run([snapshot(.8), ack(80)] + [snapshot(-.25)]*20)
assert len(near[0]) == 1 and near[0][0]['price'] == 100
assert sum(len(row) for row in near) == 1
assert sum(len(row) for row in far) == 2
assert any(o['side']=='sell' and o['price']==100.02 for row in far for o in row)

# Wrong order ID must not overwrite a known queue position.
queue = dict(type='queue_update', symbol='ABC', side='buy', order_id='wrong',
             price=100, queue_ahead=80, level_qty=100)
messages = [snapshot(.8), ack(10)] + [snapshot(-.25)]*20 + [queue, snapshot(-.25)]
rows = run(messages)
assert not rows[-1]
queue = dict(queue, order_id='b1')
rows = run(messages + [queue, snapshot(-.25)])
assert rows[-1][0]['side'] == 'sell'

# One-unit fills release the one outstanding slot per side. Position caps new buys at five.
messages = []
for i in range(6):
    messages += [snapshot(.8), ack(10), dict(type='trade_execution', symbol='ABC',
        quantity=1, price=100, buyer_id='hft_alpha', seller_id='other', maker_id='hft_alpha')]
rows = run(messages)
assert sum(len(row) for row in rows) == 5
print('Queue decisions, order IDs, duplicate suppression, and position limit passed.')
