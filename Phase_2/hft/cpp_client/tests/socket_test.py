import asyncio
import json
import os
import signal
import sys
import websockets

async def main():
    received = asyncio.Event()
    async def venue(ws):
        hs = json.loads(await ws.recv())
        assert hs['type'] == 'handshake' and hs['team_id'] == 'team_16'
        await ws.send(json.dumps(dict(type='session_event', event='SESSION_OPEN', message='test')))
        await ws.send(json.dumps(dict(type='book_snapshot', symbol='ABC', mid_price=100.01,
            microprice=100.018, obi=.8, bids=[[100,100]], asks=[[100.02,100]])))
        order = json.loads(await ws.recv())
        assert order['type']=='place_order' and order['side']=='buy' and order['price']==100
        received.set()
        await ws.wait_closed()
    async with websockets.serve(venue, '127.0.0.1', 0) as server:
        port = server.sockets[0].getsockname()[1]
        env = dict(os.environ, TEAM_ID='team_16', EXCHANGE_URL=f'ws://127.0.0.1:{port}')
        p = await asyncio.create_subprocess_exec(sys.argv[1], env=env,
            stdout=asyncio.subprocess.DEVNULL, stderr=asyncio.subprocess.DEVNULL)
        try:
            await asyncio.wait_for(received.wait(), 10)
        finally:
            p.send_signal(signal.SIGTERM)
            await asyncio.wait_for(p.wait(), 10)
    print('Local WebSocket handshake, snapshot, and order passed. This is not an AlgoArena venue test.')
asyncio.run(main())
