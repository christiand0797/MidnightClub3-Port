#!/usr/bin/env python3
"""A signed-in game tunnel races as its account: points land on the account and stay after it disconnects.

  py -3.12 tools/multiplayer/tests/test_account_tunnel.py ws://127.0.0.1:8787/ws
  py -3.12 tools/multiplayer/tests/test_account_tunnel.py wss://mc3.top/ws     (makes a throwaway account)

Uses a temporary accounts file, never the one in this Windows user's app data.
"""
import asyncio
import json
import secrets
import sys
import tempfile
from pathlib import Path

import websockets

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tools" / "multiplayer" / "proxy"))
import account  # noqa: E402
from proxy import Proxy  # noqa: E402

URL = sys.argv[1] if len(sys.argv) > 1 else "ws://127.0.0.1:8787/ws"
GAME_PORT = 3074
START = {"type": "race_event", "event": {"type": "RACE_STATE", "body": "05000000", "state": 5}}


async def board():
    ws = await websockets.connect(URL)
    try:
        await ws.send(json.dumps({"type": "join", "clientType": "browser", "name": "checker"}))
        while json.loads(await ws.recv())["type"] != "auth_ok":
            pass
        await ws.send(json.dumps({"type": "leaderboard"}))
        while True:
            m = json.loads(await asyncio.wait_for(ws.recv(), 5))
            if m.get("type") == "leaderboard":
                return {r["name"]: r for r in m["rows"]}
    finally:
        await ws.close()


async def main():
    results = []

    def check(name, ok, detail=""):
        results.append(bool(ok))
        print(f"  [{'PASS' if ok else 'FAIL'}] {name} {detail}", flush=True)

    account.ACCOUNT_FILE = Path(tempfile.mkdtemp()) / "accounts.json"
    base = account.http_base(URL)
    user = "zt" + secrets.token_hex(4)
    password = secrets.token_urlsafe(12)
    signed = await asyncio.to_thread(account.sign_in, base, user, password, True)
    check("register through the tunnel's sign-in saves a device", signed == user and account.saved(base) is not None)
    check("the password is not stored", "password" not in account.ACCOUNT_FILE.read_text())

    racer = Proxy(URL, 30891, GAME_PORT, name="ignored", game_ip="127.0.0.31", use_account=True)
    guest = Proxy(URL, 30892, GAME_PORT, name="zt-guest-" + user[2:], game_ip="127.0.0.32")
    tasks = [asyncio.create_task(p.run()) for p in (racer, guest)]
    try:
        await asyncio.gather(*(asyncio.wait_for(p.game_id_event.wait(), 20) for p in (racer, guest)))
        check("signed-in tunnel joins as the account", racer.client_name == user, racer.client_name)
        for p in (racer, guest):
            await p.ws.send(json.dumps(START))
        await asyncio.sleep(1.5)
        rows = await board()
        check("race points go to the account", rows.get(user, {}).get("points", 0) >= 10, str(rows.get(user)))
    finally:
        for p in (racer, guest):
            p.stop()
        for t in tasks:
            t.cancel()
    await asyncio.sleep(1.5)
    rows = await board()
    check("the account keeps its points after the tunnel leaves", rows.get(user, {}).get("points", 0) >= 10, str(rows.get(user)))

    await asyncio.to_thread(account.sign_out, base)
    check("sign-out forgets the device", account.saved(base) is None)
    after = Proxy(URL, 30893, GAME_PORT, name="zt-after-" + user[2:], game_ip="127.0.0.33", use_account=True)
    task = asyncio.create_task(after.run())
    try:
        await asyncio.wait_for(after.game_id_event.wait(), 20)
        check("signed out, the tunnel still plays as a guest", after.client_name.startswith("zt-after-"), after.client_name)
    finally:
        after.stop()
        task.cancel()
    # Clean up: deleting the throwaway account takes it off the public rankings.
    await asyncio.to_thread(account.sign_in, base, user, password)
    try:
        await asyncio.to_thread(account.delete_account, base, "wrong-" + password)
        check("deleting needs the right password", False)
    except account.AccountError:
        check("deleting needs the right password", True)
    await asyncio.to_thread(account.delete_account, base, password)
    rows = await board()
    check("a deleted account leaves the rankings", user not in rows, str(rows.get(user)))
    n = len(results)
    print(f"Results: {sum(results)}/{n} -> {'PASS' if all(results) and n == 9 else 'FAIL'}")
    return 0 if all(results) and n == 9 else 1


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
