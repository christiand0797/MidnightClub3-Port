#!/usr/bin/env python3
"""Race points through the real relay: two game tunnels report the host's RACE_STATE 5, both earn points,
and the leaderboard shows them. Also a car club and its chat over the same connection.

  py -3.12 tools/multiplayer/tests/test_race_points.py
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
sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_relay_peers import wait_port  # noqa: E402

PORT = 8198
URL = f"ws://127.0.0.1:{PORT}/ws"


async def connect(name, kind="game"):
    ws = await websockets.connect(URL)
    await ws.send(json.dumps({"type": "join", "clientType": kind, "name": name}))
    await expect(ws, "auth_ok")
    return ws


async def expect(ws, kind, timeout=3):
    while True:
        m = json.loads(await asyncio.wait_for(ws.recv(), timeout))
        if m.get("type") == kind:
            return m


async def main():
    results = []

    def check(name, ok, detail=""):
        results.append(bool(ok))
        print(f"  [{'PASS' if ok else 'FAIL'}] {name} {detail}", flush=True)

    env = dict(os.environ, RELAY_PORT=str(PORT), DATA_DIR=tempfile.mkdtemp(prefix="mc3-points-"))
    relay = subprocess.Popen(["node", "server.js"], cwd=ROOT / "tools/multiplayer/relay", env=env,
                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        await wait_port(PORT)
        host, guest = await connect("host"), await connect("guest")
        for ws in (host, guest):
            await ws.send(json.dumps({"type": "enter_room"}))
            await expect(ws, "game_joined")
        start = {"type": "race_event", "event": {"type": "RACE_STATE", "body": "05000000", "state": 5}}
        await host.send(json.dumps({**start, "event": {**start["event"], "dir": "out"}}))
        try:
            early = await expect(host, "points", 0.5)
        except asyncio.TimeoutError:
            early = None
        check("one tunnel alone earns nothing", early is None)
        await guest.send(json.dumps({**start, "event": {**start["event"], "dir": "in"}}))
        p_host, p_guest = await expect(host, "points"), await expect(guest, "points")
        check("a start both racers report earns both points", p_host["points"] == p_guest["points"] == 10,
              f"{p_host} {p_guest}")
        viewer = await connect("viewer", "browser")
        await viewer.send(json.dumps({"type": "leaderboard"}))
        rows = (await expect(viewer, "leaderboard"))["rows"]
        check("the leaderboard shows both racers", sorted(r["name"] for r in rows) == ["guest", "host"], str(rows))
        await viewer.send(json.dumps({"type": "presence"}))
        pres = await expect(viewer, "presence")
        check("presence lists the Main Lobby's players", pres["rooms"][0]["id"] == "main" and
              len(pres["rooms"][0]["players"]) == 2, str(pres))
        await viewer.send(json.dumps({"type": "club_create", "name": "Guests", "tag": "GST"}))
        err = await expect(viewer, "error")
        check("a guest cannot found a club", "Sign in" in err["error"], err["error"])
        await viewer.send(json.dumps({"type": "nonsense"}))
        try:
            await asyncio.wait_for(viewer.recv(), 2)
            closed = False
        except websockets.exceptions.ConnectionClosed:
            closed = True
        check("unknown message types are still refused", closed)
        for ws in (host, guest):
            await ws.close()
    finally:
        relay.terminate()
        try:
            relay.wait(5)
        except subprocess.TimeoutExpired:
            relay.kill()
    ok = all(results) and len(results) == 6
    print(f"Results: {sum(results)}/{len(results)} -> {'PASS' if ok else 'FAIL'}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
