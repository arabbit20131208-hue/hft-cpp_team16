"""Live-path stress test against a local fake venue (not AlgoArena).

Sends a handshake reply, SESSION_OPEN, then a burst of book snapshots while
answering every place_order with an order_ack and every few with a fill, so
the rx thread (IXWebSocket callback), strategy thread and send thread all run
concurrently over the real socket path. Stops the bot with SIGTERM and checks
it exits cleanly, processed every frame, and (for a -DHFT_TSAN=ON build)
reported no ThreadSanitizer warnings.

Usage: python3 socket_stress.py path/to/hft_bot stderr.log [n_snapshots]
Needs the `websockets` package.
"""
import asyncio
import json
import os
import re
import signal
import sys

import websockets

BOT, LOG = sys.argv[1], sys.argv[2]
N = int(sys.argv[3]) if len(sys.argv) > 3 else 5000


def snap(i):
    v = 0.8 if (i // 50) % 2 == 0 else -0.8
    bid = 100 + (i % 7) * 0.01
    return dict(type='book_snapshot', symbol=f'S{i % 4}', mid_price=round(bid + 0.01, 2),
                spread=0.02, microprice=bid + 0.01 + 0.01 * v, obi=v,
                bids=[[round(bid, 2), 100], [round(bid - 0.01, 2), 80]],
                asks=[[round(bid + 0.02, 2), 100]])


async def main():
    stats = dict(orders=0, acks=0, fills=0)
    done = asyncio.Event()

    async def venue(ws):
        hs = json.loads(await ws.recv())
        assert hs['type'] == 'handshake'
        await ws.send(json.dumps(dict(type='session_event', event='SESSION_OPEN', message='go')))

        async def answer():
            async for raw in ws:
                m = json.loads(raw)
                if m['type'] != 'place_order':
                    continue
                stats['orders'] += 1
                oid = f"o{stats['orders']}"
                await ws.send(json.dumps(dict(type='order_ack', order_id=oid, symbol=m['symbol'],
                                              side=m['side'], price=m['price'], quantity=1,
                                              queue_ahead=10, level_qty=100)))
                stats['acks'] += 1
                if stats['orders'] % 3 == 0:
                    buyer, seller = ('team_16', 'x') if m['side'] == 'buy' else ('x', 'team_16')
                    await ws.send(json.dumps(dict(type='trade_execution', symbol=m['symbol'],
                                                  quantity=1, price=m['price'], buyer_id=buyer,
                                                  seller_id=seller, maker_id='team_16')))
                    stats['fills'] += 1

        reader = asyncio.create_task(answer())
        for i in range(N):
            await ws.send(json.dumps(snap(i)))
            if i % 200 == 0:
                await asyncio.sleep(0)  # let acks interleave with the burst
        await asyncio.sleep(1.0)
        done.set()
        await ws.wait_closed()
        reader.cancel()

    async with websockets.serve(venue, '127.0.0.1', 0, max_queue=None) as server:
        port = server.sockets[0].getsockname()[1]
        env = dict(os.environ, TEAM_ID='team_16', EXCHANGE_URL=f'ws://127.0.0.1:{port}',
                   ORDER_QUOTA='0')
        with open(LOG, 'w') as err:
            p = await asyncio.create_subprocess_exec(BOT, env=env, stdout=asyncio.subprocess.DEVNULL,
                                                     stderr=err)
            await asyncio.wait_for(done.wait(), 120)
            p.send_signal(signal.SIGTERM)
            code = await asyncio.wait_for(p.wait(), 30)

    log = open(LOG).read()
    events = int(re.search(r'\[pipeline\] events=(\d+)', log).group(1))
    dropped = int(re.search(r'dropped_books=(\d+)', log).group(1))
    races = log.count('ThreadSanitizer')
    print(f'exit={code} snapshots_sent={N} events={events} dropped_books={dropped} '
          f"orders={stats['orders']} acks={stats['acks']} fills={stats['fills']} "
          f'tsan_warnings={races}')
    assert code == 0, 'bot did not exit cleanly'
    assert races == 0, 'ThreadSanitizer reported a problem'
    assert events + dropped >= N, 'frames lost'
    assert stats['orders'] > 0


asyncio.run(main())
