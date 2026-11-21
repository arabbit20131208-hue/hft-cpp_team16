"""Kill the strategy process live and check the gateway survives and recovers.

A local fake venue (not AlgoArena) streams book snapshots and acks every order.
The test starts `hft_bot` (the gateway, which starts `strategy`), lets it
trade, sends SIGKILL to the strategy process, and checks:

  * the gateway's WebSocket stays up (the venue sees exactly one connection
    until the test itself starts shutting down),
  * the gateway cancels the orders that were resting when the strategy died
    (kill switch) and logs the loss,
  * a new strategy process is started and trading resumes,
  * the gateway exits cleanly on SIGTERM and removes its shared memory.

Prints the recovery time and counts used in the Phase 4 report.

A second fault follows: the restarted strategy is frozen with SIGSTOP (alive
but silent), which only the heartbeat can detect.

Usage: python3 resilience_test.py path/to/hft_bot [run_seconds=6]
Needs the `websockets` package; without it the test is skipped.
"""
import asyncio
import json
import os
import re
import signal
import sys
import tempfile
import time

try:
    import websockets
except ImportError:
    print('resilience_test: SKIPPED (pip install websockets)')
    sys.exit(0)

BOT = sys.argv[1]
RUN = float(sys.argv[2]) if len(sys.argv) > 2 else 6.0
KILL_AT = RUN / 3


def snap(i):
    # Alternating strong signal so both sides keep quoting and requoting.
    v = 0.8 if (i // 40) % 2 == 0 else -0.8
    bid = round(100 + (i // 25 % 5) * 0.01, 2)
    return dict(type='book_snapshot', symbol=f'S{i % 3}', mid_price=round(bid + 0.01, 3),
                spread=0.02, microprice=bid + 0.01 + 0.01 * v, obi=v,
                bids=[[bid, 100], [round(bid - 0.01, 2), 80]], asks=[[round(bid + 0.02, 2), 100]])


async def main():
    connections = 0
    places, cancels = [], []   # (time, msg)
    acked = {}                 # order_id -> time acked
    stop = asyncio.Event()

    async def venue(ws):
        nonlocal connections
        connections += 1
        await ws.recv()  # handshake
        await ws.send(json.dumps(dict(type='session_event', event='SESSION_OPEN', message='go')))

        async def answer():
            n = 0
            async for raw in ws:
                m = json.loads(raw)
                t = time.monotonic()
                if m['type'] == 'place_order':
                    n += 1
                    oid = f'o{n}'
                    places.append((t, m))
                    acked[oid] = t
                    await ws.send(json.dumps(dict(type='order_ack', order_id=oid, symbol=m['symbol'],
                                                  side=m['side'], price=m['price'], quantity=1,
                                                  queue_ahead=5, level_qty=100)))  # near front: held
                elif m['type'] == 'cancel_order':
                    cancels.append((t, m))

        reader = asyncio.create_task(answer())
        i = 0
        while not stop.is_set():
            await ws.send(json.dumps(snap(i)))
            i += 1
            await asyncio.sleep(0.002)
        reader.cancel()

    log = tempfile.NamedTemporaryFile('w+', suffix='.log', delete=False)
    async with websockets.serve(venue, '127.0.0.1', 0) as server:
        port = server.sockets[0].getsockname()[1]
        env = dict(os.environ, TEAM_ID='team_16', EXCHANGE_URL=f'ws://127.0.0.1:{port}', ORDER_QUOTA='0')
        p = await asyncio.create_subprocess_exec(BOT, env=env, stdout=asyncio.subprocess.DEVNULL, stderr=log)
        t0 = time.monotonic()
        await asyncio.sleep(KILL_AT)
        # Kill while some of our orders are resting, so the kill switch has work.
        while len(acked) - len(cancels) <= 0 and time.monotonic() - t0 < RUN:
            await asyncio.sleep(0.001)

        text = open(log.name).read()
        pids = re.findall(r'\[strategy\] pid (\d+) attached', text)
        assert pids, 'strategy never attached:\n' + text[-2000:]
        victim = int(pids[-1])
        open_at_kill = len(acked) - len(cancels)
        os.kill(victim, signal.SIGKILL)
        t_kill = time.monotonic()
        await asyncio.sleep(KILL_AT)

        # Second fault: the new strategy hangs (SIGSTOP) instead of dying. Only the
        # heartbeat can tell; the gateway must SIGKILL it and start another.
        text = open(log.name).read()
        hung = int(re.findall(r'\[strategy\] pid (\d+) attached', text)[-1])
        os.kill(hung, signal.SIGSTOP)
        t_hang = time.monotonic()
        await asyncio.sleep(RUN - 2 * KILL_AT)

        connections_during_run = connections  # before the venue starts closing sockets
        cancels_after_kill = sum(1 for t, _ in cancels if t_kill <= t < t_kill + 0.5)
        stop.set()
        p.send_signal(signal.SIGTERM)
        code = await asyncio.wait_for(p.wait(), 20)

    text = open(log.name).read()
    pids_after = [int(x) for x in re.findall(r'\[strategy\] pid (\d+) attached', text)]
    backs = re.findall(r'strategy back \(epoch (\d+)\) ([\d.]+) ms after the loss', text)
    losses = re.findall(r'strategy lost \(([^)]+)\); cancelling (\d+) resting orders', text)
    m = backs[0] if backs else None
    lost = losses[0] if losses else None
    before = sum(1 for t, _ in places if t < t_kill)
    after = sum(1 for t, _ in places if m and t_kill + float(m[1]) / 1000 < t < t_hang)
    after_hang = sum(1 for t, _ in places if len(backs) > 1 and t > t_hang + float(backs[1][1]) / 1000)
    killswitch = int(lost[1]) if lost else 0
    shm = re.search(r'\[gateway\] shm (\S+)', text).group(1)
    leftover = os.path.exists('/dev/shm' + shm) if os.path.isdir('/dev/shm') else False

    print(f'exit={code} venue_connections={connections_during_run} killed_pid={victim} new_pid={pids_after[-1]} '
          f'detected="{lost[0] if lost else None}" recovery_ms={m[1] if m else None} '
          f'resting_at_kill={open_at_kill} killswitch_cancels={killswitch} '
          f'venue_cancels_within_500ms={cancels_after_kill} orders_before_kill={before} '
          f'orders_after_recovery={after} shm_left={leftover}')
    print(f'hang: hung_pid={hung} detected="{losses[1][0] if len(losses) > 1 else None}" '
          f'recovery_ms={backs[1][1] if len(backs) > 1 else None} orders_after={after_hang}')
    assert code == 0, text[-2000:]
    assert connections_during_run == 1, 'gateway reconnected: the strategy crash reached the socket'
    assert killswitch > 0 and cancels_after_kill >= killswitch, 'resting orders were not cancelled'
    assert lost and m, 'loss or recovery not logged:\n' + text[-2000:]
    assert pids_after[-1] != victim
    assert after > 0, 'no orders after the restart'
    assert len(losses) > 1 and losses[1][0] == 'heartbeat timeout' and len(backs) > 1, \
        'hung strategy not detected:\n' + text[-2000:]
    assert after_hang > 0, 'no orders after the hang recovery'
    assert not leftover
    os.unlink(log.name)
    print('resilience_test: strategy SIGKILL and hang (SIGSTOP) both survived; the gateway kept its '
          'connection, cancelled resting orders, restarted the strategy and resumed trading.')


asyncio.run(main())
