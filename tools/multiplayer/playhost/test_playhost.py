#!/usr/bin/env python3
"""playhost.py without launching the game: slots, saves, and the HTTP API's checks.

  py -3.12 tools/multiplayer/playhost/test_playhost.py
"""
import asyncio
import secrets
import sys
import tempfile
from pathlib import Path

from aiohttp.test_utils import TestClient, TestServer

sys.path.insert(0, str(Path(__file__).resolve().parent))
import playhost as ph  # noqa: E402


def make_host(install, **kw):
    argv = ["--install", str(install), "--saves", str(install / "saves"), "--max-sessions", "2"]
    for k, v in kw.items():
        argv += [f"--{k.replace('_', '-')}", str(v)]
    host = ph.PlayHost(ph.parse(argv))
    started = []

    async def fake_start(s):  # the game is not launched in these tests
        started.append(s)
        s.state = "running"
        await s.ended.wait()
        await host.stop(s)
    host.start = fake_start
    return host, started


async def main():
    ok = []

    def check(name, cond, detail=""):
        ok.append(bool(cond))
        print(f"  [{'PASS' if cond else 'FAIL'}] {name} {detail}")

    check("instance 7 is never offered", 7 not in ph.instances("1-8") and ph.instances("6-8") == [6, 8])
    with tempfile.TemporaryDirectory() as tmp:
        install = Path(tmp)
        (install / "mc3.exe").write_bytes(b"")
        (install / "hdd2" / "E" / "UDATA").mkdir(parents=True)  # a developer's own saves
        (install / "hdd3").mkdir()
        (install / ph.hdd_dir(install, 3) / ph.MARKER).write_text("x")
        check("a save folder this host did not make is skipped", not ph.usable_slot(install, 2))
        check("a save folder this host made is reused", ph.usable_slot(install, 3))
        check("a missing save folder is usable", ph.usable_slot(install, 4))
        check("instance 1 uses the plain hdd folder", ph.hdd_dir(install, 1).name == "hdd")

        host, started = make_host(install, instances="2,3,4")
        player = secrets.token_hex(32)
        s = ph.Session("a", "k", 3, 1, "A", "", player)
        host.prepare_save(s)
        udata = install / "hdd3" / "E" / "UDATA"
        (udata / "54540079").mkdir(parents=True)
        (udata / "54540079" / "Player 1.mc3Remix").write_bytes(b"career")
        host.keep_save(s)
        (udata / "54540079" / "Player 1.mc3Remix").unlink()
        host.prepare_save(s)
        check("a player's career is kept and restored", (udata / "54540079" / "Player 1.mc3Remix").read_bytes() == b"career")
        host.prepare_save(ph.Session("b", "k", 3, 1, "B", "", None))
        check("a new player starts with an empty save", not any(udata.rglob("*")))
        check("the developer's own save folder is untouched", (install / "hdd2" / "E" / "UDATA").exists())

        async with TestClient(TestServer(host.app())) as c:
            r = await c.get("/api/status")
            check("status answers without secrets", r.status == 200 and "key" not in await r.text())
            r = await c.post("/api/sessions", json={"name": "x" * 40})
            check("an over-long name is refused", r.status == 400)
            r = await c.post("/api/sessions", json={"player": "not-a-key"})
            check("a malformed player key is refused", r.status == 400)
            r = await c.post("/api/sessions", json={"name": "A"}, headers={"Origin": "https://evil.example"})
            check("a cross-site start is refused", r.status == 403)
            r = await c.post("/api/sessions", json={"name": "A", "room": "Friday", "player": player})
            body = await r.json()
            check("a session starts in the named room", r.status == 201 and body["room"] == "Friday" and body["instance"] in (3, 4),
                  str(body))
            await asyncio.sleep(0.05)
            check("the copy was started", len(started) == 1 and started[0].room == "Friday")
            r = await c.post("/api/sessions", json={"name": "B"})
            check("one game per address", r.status == 429)
            r = await c.get(f"/api/sessions/{body['id']}/stream?key=wrong")
            check("the stream needs the session key", r.status == 404)
            r = await c.delete(f"/api/sessions/{body['id']}", headers={"X-Session-Key": "wrong"})
            check("ending needs the session key", r.status == 404)
            r = await c.delete(f"/api/sessions/{body['id']}", headers={"X-Session-Key": body["key"]})
            await asyncio.sleep(0.1)
            check("the owner can end it", r.status == 200 and not host.sessions)
            r = await c.post("/api/sessions", json={})
            body = await r.json()
            check("no room means the Main Lobby", r.status == 201 and body["room"] == "Main Lobby")
            for s in list(host.sessions.values()):
                s.ended.set()
            await asyncio.sleep(0.1)

    print(f"playhost: {sum(ok)}/{len(ok)}")
    return 0 if all(ok) else 1


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
