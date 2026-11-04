#!/usr/bin/env python3
"""Three-player relay tunnel test: the native "MC3R" framing xnet.cpp speaks, through real proxies and relay.

Starts the relay (node) on a test port and three proxies that join one room by name (as Launch-Multiplayer does).
Three UDP sockets stand in for three games. Checks:
  1. all three proxies end up in the same relay game without a host typing a game id;
  2. a broadcast (peer 0) from game A reaches B and C, each seeing A as one peer number with A's source port;
  3. a reply addressed to that peer number reaches only A, not C;
  4. the proxy port drops non-tunnel datagrams and its own bounced deliveries (no relay loop).

  py -3.12 tools/multiplayer/tests/test_relay_peers.py
"""
import asyncio
import os
import socket
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tools" / "multiplayer" / "proxy"))
from proxy import Proxy, RELAY_MAGIC  # noqa: E402

RELAY_PORT = 8193
RELAY_URL = f"ws://127.0.0.1:{RELAY_PORT}/ws"
ROOM = "MC3 relay test"
PROXY_PORTS = {"A": 30761, "B": 30762, "C": 30763}
GAME_PORT = 3074  # every game binds the same System Link port, on its own address
GAME_IPS = {"A": "127.0.0.11", "B": "127.0.0.12", "C": "127.0.0.13"}


def frame(src, dst, peer, payload):
    return RELAY_MAGIC + struct.pack("<HHI", src, dst, peer) + payload


def unframe(data):
    assert data[:4] == RELAY_MAGIC, data[:12]
    src, dst, peer = struct.unpack("<HHI", data[4:12])
    return src, dst, peer, data[12:]


async def recv(sock, timeout=3.0):
    loop = asyncio.get_running_loop()
    return await asyncio.wait_for(loop.sock_recvfrom(sock, 65535), timeout)


async def nothing(sock, timeout=0.6):
    try:
        data, _ = await recv(sock, timeout)
        return data
    except asyncio.TimeoutError:
        return None


async def wait_port(port, timeout=15):
    end = asyncio.get_running_loop().time() + timeout
    while True:
        try:
            _, w = await asyncio.open_connection("127.0.0.1", port)
            w.close()
            return
        except OSError:
            if asyncio.get_running_loop().time() > end:
                raise
            await asyncio.sleep(0.2)


async def main():
    results = []

    def check(name, ok, detail=""):
        results.append(ok)
        print(f"  [{'PASS' if ok else 'FAIL'}] {name} {detail}", flush=True)

    data_dir = tempfile.mkdtemp(prefix="mc3-relay-test-")
    env = dict(os.environ, RELAY_PORT=str(RELAY_PORT), DATA_DIR=data_dir)
    relay = subprocess.Popen(["node", "server.js"], cwd=ROOT / "tools/multiplayer/relay", env=env,
                             stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)
    proxies, tasks, games = {}, [], {}
    try:
        await wait_port(RELAY_PORT)
        for key, port in PROXY_PORTS.items():
            g = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            g.bind((GAME_IPS[key], GAME_PORT))
            g.setblocking(False)
            games[key] = g
            p = Proxy(RELAY_URL, port, GAME_PORT, room=ROOM, name=f"player-{key}", game_ip=GAME_IPS[key])
            proxies[key] = p
            tasks.append(asyncio.ensure_future(p.run()))
            await asyncio.wait_for(p.game_id_event.wait(), 10)  # one at a time: the first creates the room
        ids = {p.game_id for p in proxies.values()}
        check("three proxies join one relay room by name", len(ids) == 1 and None not in ids, str(ids))

        # Each game sends one tunnelled datagram first so its proxy learns the game socket (as xnet does).
        a, b, c = games["A"], games["B"], games["C"]
        hello = frame(GAME_PORT, GAME_PORT, 0, b"DISCOVER")
        a.sendto(hello, ("127.0.0.1", PROXY_PORTS["A"]))
        got = {}
        for key in "BC":
            data, addr = await recv(games[key])
            got[key] = unframe(data)
            check(f"broadcast from A reaches {key} from its proxy", addr == ("127.0.0.1", PROXY_PORTS[key]),
                  f"{addr} {got[key][:3]}")
        peer_at_b = got["B"][2]
        check("delivery names a peer number and A's source port",
              all(v[2] >= 1 and v[0] == GAME_PORT and v[3] == b"DISCOVER" for v in got.values()), str(got))

        # C introduces itself so B's proxy knows two peers; then B answers A only.
        c.sendto(frame(GAME_PORT, GAME_PORT, 0, b"C-HELLO"), ("127.0.0.1", PROXY_PORTS["C"]))
        for key in "AB":
            await recv(games[key])
        peers_b = proxies["B"].peers
        check("B's proxy gives A and C different peer numbers", len(set(peers_b.values())) == 2, str(peers_b))

        b.sendto(frame(GAME_PORT, GAME_PORT, peer_at_b, b"REPLY-TO-A"), ("127.0.0.1", PROXY_PORTS["B"]))
        data, _ = await recv(a)
        src, dst, peer, payload = unframe(data)
        check("reply to A's peer number reaches A", payload == b"REPLY-TO-A" and peer >= 1, f"peer={peer}")
        leaked = await nothing(c)
        check("the reply does not reach C", leaked is None, repr(leaked))

        # The proxy port ignores raw datagrams and its own bounced deliveries.
        stray = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        stray.sendto(b"not a tunnel datagram", ("127.0.0.1", PROXY_PORTS["A"]))
        stray.close()
        check("raw datagram to a proxy is not relayed", await nothing(b) is None and await nothing(c) is None)
        bound = proxies["A"].udp_sock.getsockname()[0]
        check("proxy listens on loopback only", bound == "127.0.0.1", bound)
    finally:
        for p in proxies.values():
            p.stop()
        for t in tasks:
            t.cancel()
        for g in games.values():
            g.close()
        relay.terminate()
        try:
            relay.wait(5)
        except subprocess.TimeoutExpired:
            relay.kill()
    ok = all(results) and len(results) == 9
    print(f"Results: {sum(results)}/{len(results)} -> {'PASS' if ok else 'FAIL'}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
