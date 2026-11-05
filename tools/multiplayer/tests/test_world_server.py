#!/usr/bin/env python3
"""Checks a running online-world server (the Node relay or the Cloudflare worker) over its real WebSocket.

  py -3.12 tools/multiplayer/tests/test_world_server.py ws://127.0.0.1:8787/ws
  py -3.12 tools/multiplayer/tests/test_world_server.py wss://mc3.top/ws

Two game tunnels (proxy.py, as players run them) join the Main Lobby with no room name and exchange System Link
datagrams; a third joins a named room and must not hear them; a race start reported by both earns points; presence
and the leaderboard show them.
"""
import asyncio
import json
import socket
import struct
import sys
from pathlib import Path

import websockets

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tools" / "multiplayer" / "proxy"))
from proxy import Proxy, RELAY_MAGIC  # noqa: E402

URL = sys.argv[1] if len(sys.argv) > 1 else "ws://127.0.0.1:8787/ws"
GAME_PORT = 3074


def frame(payload, peer=0):
    return RELAY_MAGIC + struct.pack("<HHI", GAME_PORT, GAME_PORT, peer) + payload


async def recv(sock, timeout=4.0):
    loop = asyncio.get_running_loop()
    return (await asyncio.wait_for(loop.sock_recvfrom(sock, 65535), timeout))[0]


async def quiet(sock, timeout=1.0):
    try:
        return await recv(sock, timeout)
    except asyncio.TimeoutError:
        return None


async def viewer():
    ws = await websockets.connect(URL)
    await ws.send(json.dumps({"type": "join", "clientType": "browser", "name": "checker"}))
    while json.loads(await ws.recv())["type"] != "auth_ok":
        pass
    return ws


async def expect(ws, kind, timeout=5):
    while True:
        m = json.loads(await asyncio.wait_for(ws.recv(), timeout))
        if m.get("type") == kind:
            return m


async def main():
    results = []

    def check(name, ok, detail=""):
        results.append(bool(ok))
        print(f"  [{'PASS' if ok else 'FAIL'}] {name} {detail}", flush=True)

    specs = [("alpha", None, "127.0.0.21", 30881), ("bravo", None, "127.0.0.22", 30882), ("hidden", "Secret Garage", "127.0.0.23", 30883)]
    proxies, games, tasks = [], [], []
    for name, room, ip, port in specs:
        p = Proxy(URL, port, GAME_PORT, room=room, name=name, game_ip=ip)
        proxies.append(p)
        tasks.append(asyncio.create_task(p.run()))
        g = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        g.bind((ip, GAME_PORT))
        g.setblocking(False)
        games.append(g)
    try:
        await asyncio.gather(*(asyncio.wait_for(p.game_id_event.wait(), 20) for p in proxies))
        check("no room name -> Main Lobby", proxies[0].game_id == proxies[1].game_id == "main",
              f"{proxies[0].game_id} {proxies[1].game_id}")
        check("a typed room name -> its own room", proxies[2].game_id not in (None, "main"), str(proxies[2].game_id))
        for (name, room, ip, port), g in zip(specs, games):
            g.sendto(frame(b"HELLO-" + name.encode()), ("127.0.0.1", port))
        await asyncio.sleep(1.5)
        for g in games:
            while await quiet(g, 0.2):
                pass
        games[0].sendto(frame(b"MATCH-AD"), ("127.0.0.1", specs[0][3]))
        got = await quiet(games[1], 4)
        check("a System Link broadcast reaches the other lobby player", bool(got) and got.endswith(b"MATCH-AD"))
        check("it does not reach a private room", await quiet(games[2], 1.5) is None)

        v = await viewer()
        await v.send(json.dumps({"type": "list"}))
        lst = await expect(v, "gameList")
        main = [g for g in lst["games"] if g["id"] == "main"]
        check("Main Lobby listed, no cap, 2 players", main and main[0]["max_players"] == 0 and main[0]["player_count"] == 2, str(main))
        check("private room not listed", all(g["id"] != proxies[2].game_id for g in lst["games"]))
        start = {"type": "race_event", "event": {"type": "RACE_STATE", "body": "05000000", "state": 5}}
        for p in proxies[:2]:
            await p.ws.send(json.dumps(start))
        await asyncio.sleep(1)
        await v.send(json.dumps({"type": "leaderboard"}))
        rows = (await expect(v, "leaderboard"))["rows"]
        names = {r["name"]: r["points"] for r in rows}
        check("a confirmed race start earns both racers points", names.get("alpha", 0) >= 10 and names.get("bravo", 0) >= 10, str(rows))
        await v.send(json.dumps({"type": "presence"}))
        pres = await expect(v, "presence")
        check("presence shows the lobby's players", any(r["id"] == "main" and len(r["players"]) >= 2 for r in pres["rooms"]))
        await v.close()
    finally:
        for p in proxies:
            p.stop()
        for t in tasks:
            t.cancel()
        for g in games:
            g.close()
    print(f"Results: {sum(results)}/{len(results)} -> {'PASS' if all(results) and len(results) == 8 else 'FAIL'}")
    return 0 if all(results) and len(results) == 8 else 1


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
