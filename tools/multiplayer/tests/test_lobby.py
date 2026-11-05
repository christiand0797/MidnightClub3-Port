#!/usr/bin/env python3
"""The online world: every game joins the relay's Main Lobby unless it names a room.

Starts the relay (node) on a test port and real proxies, then checks:
  1. proxies with no room name all land in the Main Lobby, more than the old 8-player cap;
  2. the Main Lobby is listed, a named room is not (private by name);
  3. room names are case-insensitive, so friends typing "Friday" and "friday" meet;
  4. a broadcast in the Main Lobby reaches every lobby player and nobody in a named room;
  5. the Main Lobby survives everyone leaving.
Run:
  py -3.12 tools/multiplayer/tests/test_lobby.py
"""
import asyncio
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

import websockets

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tools" / "multiplayer" / "proxy"))
sys.path.insert(0, str(Path(__file__).resolve().parent))
from proxy import Proxy  # noqa: E402
from test_relay_peers import frame, recv, nothing, wait_port, GAME_PORT  # noqa: E402

RELAY_PORT = 8195
RELAY_URL = f"ws://127.0.0.1:{RELAY_PORT}/ws"
LOBBY_PLAYERS = 10  # more than the old per-room cap of 8
BASE_UDP = 30800


async def game_list():
    async with websockets.connect(RELAY_URL) as ws:
        await ws.send(json.dumps({"type": "join", "clientType": "browser", "name": "lister"}))
        while True:
            msg = json.loads(await asyncio.wait_for(ws.recv(), 3))
            if msg.get("type") == "auth_ok":
                await ws.send(json.dumps({"type": "list"}))
            elif msg.get("type") == "gameList":
                return msg["games"]


async def main():
    results = []

    def check(name, ok, detail=""):
        results.append(ok)
        print(f"  [{'PASS' if ok else 'FAIL'}] {name} {detail}", flush=True)

    data_dir = tempfile.mkdtemp(prefix="mc3-lobby-test-")
    env = dict(os.environ, RELAY_PORT=str(RELAY_PORT), DATA_DIR=data_dir)
    relay = subprocess.Popen(["node", "server.js"], cwd=ROOT / "tools/multiplayer/relay", env=env,
                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    proxies, tasks, games = [], [], []
    try:
        await wait_port(RELAY_PORT)
        # the relay admits 8 quick connections per address, then one per 2 s; every proxy here shares 127.0.0.1,
        # so the later ones get in through the proxy's own reconnect
        rooms = [None] * LOBBY_PLAYERS + ["Friday", "friday"]
        for n, room in enumerate(rooms):
            ip = f"127.0.1.{100 + n}"  # each fake game on its own loopback address, like xnet instances
            p = Proxy(RELAY_URL, BASE_UDP + n, GAME_PORT, room=room, name=f"p{n}", game_ip=ip)
            proxies.append(p)
            tasks.append(asyncio.create_task(p.run()))
            g = __import__("socket").socket(2, 2)
            g.bind((ip, GAME_PORT))
            g.setblocking(False)
            games.append(g)
        await asyncio.gather(*(asyncio.wait_for(p.game_id_event.wait(), 45) for p in proxies))
        lobby_ids = {p.game_id for p in proxies[:LOBBY_PLAYERS]}
        check(f"{LOBBY_PLAYERS} games with no room name share the Main Lobby", lobby_ids == {"main"}, str(lobby_ids))
        friday = proxies[LOBBY_PLAYERS:]
        check("room names are case-insensitive", friday[0].game_id == friday[1].game_id != "main",
              f"{friday[0].game_id} {friday[1].game_id}")

        listed = await game_list()
        main = [g for g in listed if g["id"] == "main"]
        check("Main Lobby is listed with no cap", len(main) == 1 and main[0]["max_players"] == 0
              and main[0]["player_count"] == LOBBY_PLAYERS, str(main))
        check("a named room is not listed", all(g["id"] != friday[0].game_id for g in listed), str(listed))

        # every game sends a datagram first so its proxy learns the game socket (as xnet does)
        for n, g in enumerate(games):
            g.sendto(frame(GAME_PORT, GAME_PORT, 0, b"HELLO"), ("127.0.0.1", BASE_UDP + n))
        await asyncio.sleep(1.0)
        for g in games:
            while await nothing(g, 0.1):
                pass
        games[0].sendto(frame(GAME_PORT, GAME_PORT, 0, b"MATCH-AD"), ("127.0.0.1", BASE_UDP))
        got = []
        for g in games[1:LOBBY_PLAYERS]:
            try:
                data, _ = await recv(g, 3)
                got.append(data.endswith(b"MATCH-AD"))
            except asyncio.TimeoutError:
                got.append(False)
        check("a lobby broadcast reaches every other lobby player", all(got), f"{sum(got)}/{len(got)}")
        leaked = [await nothing(g) for g in games[LOBBY_PLAYERS:]]
        check("a lobby broadcast does not reach a named room", leaked == [None, None])

        for p in proxies[:LOBBY_PLAYERS]:
            p.stop()
            if p.ws:
                await p.ws.close()
        await asyncio.sleep(1.0)
        listed = await game_list()
        main = [g for g in listed if g["id"] == "main"]
        check("Main Lobby stays open when empty", len(main) == 1 and main[0]["player_count"] == 0, str(main))
    finally:
        for p in proxies:
            p.stop()
        for t in tasks:
            t.cancel()
        for g in games:
            g.close()
        relay.terminate()
        try:
            relay.wait(5)
        except subprocess.TimeoutExpired:
            relay.kill()
    ok = all(results) and len(results) == 7
    print(f"Results: {sum(results)}/{len(results)} -> {'PASS' if ok else 'FAIL'}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
