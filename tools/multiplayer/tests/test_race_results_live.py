#!/usr/bin/env python3
"""Replays a real recorded Track Race (relay/fixtures_track_race.json: what both game tunnels reported) into a
running world server over two game-tunnel WebSockets and checks the finishing places it awards.

  py -3.12 tools/multiplayer/tests/test_race_results_live.py wss://mc3.top/ws
  py -3.12 tools/multiplayer/tests/test_race_results_live.py ws://127.0.0.1:8787/ws

Guests in a private throwaway room: nothing persists on the public rankings.
"""
import asyncio
import json
import secrets
import sys
from pathlib import Path

import websockets

URL = sys.argv[1] if len(sys.argv) > 1 else "ws://127.0.0.1:8787/ws"
EVENTS = json.loads((Path(__file__).resolve().parents[1] / "relay" / "fixtures_track_race.json").read_text())


async def tunnel(name, room):
    ws = await websockets.connect(URL)
    await ws.send(json.dumps({"type": "join", "clientType": "game", "name": name}))
    while json.loads(await ws.recv())["type"] != "auth_ok":
        pass
    await ws.send(json.dumps({"type": "enter_room", "name": room}))
    while json.loads(await ws.recv())["type"] != "game_joined":
        pass
    return ws


async def collect(ws, into):
    try:
        async for raw in ws:
            m = json.loads(raw)
            if m.get("type") in ("race_result", "points"):
                into.append(m)
    except websockets.ConnectionClosed:
        pass


async def main():
    room = "zt-results-" + secrets.token_hex(4)
    socks = {"host": await tunnel("zt-host", room), "guest": await tunnel("zt-guest", room)}
    got = {"host": [], "guest": []}
    readers = [asyncio.create_task(collect(socks[k], got[k])) for k in socks]
    for item in EVENTS:
        await socks[item["by"]].send(json.dumps({"type": "race_event", "event": item["event"]}))
        await asyncio.sleep(0.05)
    await asyncio.sleep(2)
    for s in socks.values():
        await s.close()
    await asyncio.gather(*readers, return_exceptions=True)
    results = []

    def check(name, ok, detail=""):
        results.append(bool(ok))
        print(f"  [{'PASS' if ok else 'FAIL'}] {name} {detail}", flush=True)

    g = [m for m in got["guest"] if m["type"] == "race_result"]
    h = [m for m in got["host"] if m["type"] == "race_result"]
    check("both racers' starts counted", any(m["type"] == "points" for m in got["guest"]) and any(m["type"] == "points" for m in got["host"]))
    check("the guest, furthest along, finishes 1st of 2", g and g[0]["place"] == 1 and g[0]["of"] == 2, str(g))
    check("the host finishes 2nd of 2", h and h[0]["place"] == 2 and h[0]["of"] == 2, str(h))
    check("1st earns more than 2nd", g and h and g[0]["earned"] > h[0]["earned"])
    print(f"Results: {sum(results)}/{len(results)} -> {'PASS' if all(results) else 'FAIL'}")
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
