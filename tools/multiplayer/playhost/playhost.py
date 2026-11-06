#!/usr/bin/env python3
"""MC3 play host: play the PC port in a browser, in the same online world as players running it locally.

Each browser player gets a copy of the game of their own on this machine: the same mc3.exe, settings, HD textures,
PBR packs and mods as the local build (it is the local build, run from --install). The copy runs with
MC3_RELAY_ONLY=1, so it talks to other players only through the relay, exactly like a player at home: its tunnel
(proxy.py) joins the Main Lobby, or the room the player typed, and everyone there sees its System Link matches.
The window is streamed to the browser (stream/stream_proxy.py) and the browser's gamepad/keyboard drives the copy as
player 1 (MC3_REMOTE_PRIMARY=1, runtime/core/remote_pad.cpp).

  Browser --HTTPS/WSS--> play host --(local)--> stream_proxy <--window--> mc3.exe --instance N <--UDP--> proxy.py
                                                                                               relay (Main Lobby)

Copies use instance numbers from --instances (default 1-6 and 8; 7 is never used). A copy's save folder (hdd,
hddN) is used only when it does not exist yet or this host made it (a .playhost marker), so a developer's own saves
are never touched. A browser keeps a private player key in its storage; its career is restored into the copy when it
comes back and saved again when the session ends.

  py -3.12 tools/multiplayer/playhost/playhost.py --install templates/game/out/native --relay ws://127.0.0.1:8080/ws
       [--disc PATH] [--port 8790] [--public-origin https://play.example.net] [--max-sessions 2]

Run it behind a TLS reverse proxy for players outside this PC (it binds 127.0.0.1 unless --bind is given).
"""
import argparse
import asyncio
import hashlib
import hmac
import json
import os
import re
import secrets
import shutil
import signal
import socket
import stat
import subprocess
import sys
import time
from pathlib import Path

from aiohttp import web, ClientSession, WSMsgType, ClientConnectorError

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
MULTI = ROOT / "tools" / "multiplayer"
NAME = re.compile(r"^[^\x00-\x1f\x7f]{1,24}$")
ROOM = re.compile(r"^[^\x00-\x1f\x7f]{0,50}$")
PLAYER = re.compile(r"^[0-9a-f]{64}$")
MARKER = ".playhost"
DEFAULT_INSTANCES = (1, 2, 3, 4, 5, 6, 8)
STREAM_BASE = 8800  # copy k streams on STREAM_BASE + 2k (UDP) and +1 (browser WebSocket, loopback only)
MAX_SAVE_BYTES = 16 * 1024 * 1024


def private_tree(root, max_bytes=MAX_SAVE_BYTES, max_files=4096):
    """Reject links and excessive trees before any copy or rename. Never delete a player tree."""
    root = Path(root).absolute()
    for part in (root, *root.parents):
        if part.exists() or part.is_symlink():
            st = part.lstat()
            if stat.S_ISLNK(st.st_mode) or getattr(st, 'st_file_attributes', 0) & 0x400:
                raise OSError('Linked save paths are not supported.')
    if not root.exists():
        return
    total = count = 0
    for base, dirs, files in os.walk(root, followlinks=False):
        for name in dirs + files:
            p = Path(base) / name
            st = p.lstat()
            count += 1
            if stat.S_ISLNK(st.st_mode) or getattr(st, 'st_file_attributes', 0) & 0x400:
                raise OSError('Linked save entries are not supported.')
            if not stat.S_ISDIR(st.st_mode):
                if not stat.S_ISREG(st.st_mode) or st.st_nlink != 1:
                    raise OSError('Save entries must be private regular files.')
                total += st.st_size
            if count > max_files or total > max_bytes:
                raise OSError('Save storage quota reached; existing files were preserved.')


def retain_tree(source):
    """Same-parent rename with a fresh name; the old bytes remain available for recovery."""
    source = Path(source).absolute()
    private_tree(source)
    dest = source.with_name(source.name + '.retained-' + secrets.token_hex(8))
    if dest.parent.resolve() != source.parent.resolve() or dest.exists():
        raise OSError('Unsafe save retention path.')
    source.rename(dest)
    return dest


def hdd_dir(install, instance):
    return install / ("hdd" if instance <= 1 else f"hdd{instance}")


def usable_slot(install, instance):
    d = hdd_dir(install, instance)
    return instance != 7 and (not d.exists() or (d / MARKER).exists())


def free_port(port):
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        try:
            s.bind(("127.0.0.1", port))
            return True
        except OSError:
            return False


class Session:
    def __init__(self, sid, key, instance, slot, name, room, player):
        self.id, self.key, self.instance, self.slot = sid, key, instance, slot
        self.name, self.room, self.player = name, room, player
        self.pair = secrets.token_hex(32)
        self.procs = {}
        self.created = self.last_viewer = time.time()
        self.viewers = 0
        self.state = "starting"
        self.ended = asyncio.Event()
        self.stop_lock = asyncio.Lock()

    @property
    def stream_port(self):
        return STREAM_BASE + 2 * self.slot

    def public(self):
        return {"id": self.id, "name": self.name, "room": self.room or "Main Lobby", "state": self.state,
                "age": int(time.time() - self.created), "viewers": self.viewers}


class PlayHost:
    def __init__(self, a):
        self.a = a
        self.install = a.install.resolve()
        self.exe = self.install / "mc3.exe"
        self.sessions = {}
        self.lock = asyncio.Lock()
        self.internal_origin = f"http://127.0.0.1:{a.port}"
        self.save_root = (a.saves or self.install / "playhost_saves").resolve()

    # ---------- slots and saves ----------
    def free_slot(self):
        used = {s.instance for s in self.sessions.values()}
        for k, n in enumerate(self.a.instances):
            if n not in used and usable_slot(self.install, n) and free_port(STREAM_BASE + 2 * k + 1):
                return n, k
        return None

    def player_saves(self, player):
        return self.save_root / hashlib.sha256(player.encode()).hexdigest()[:32] / "UDATA"

    def prepare_save(self, s):
        d = hdd_dir(self.install, s.instance)
        if not usable_slot(self.install, s.instance):
            raise OSError('Slot is no longer owned by this host.')
        private_tree(d, 256 * 1024 * 1024)
        private_tree(self.save_root, 2 * 1024 * 1024 * 1024, 32768)
        d.mkdir(parents=True, exist_ok=True)
        (d / MARKER).write_text("made by tools/multiplayer/playhost; reset for each browser session\n")
        udata = d / "E" / "UDATA"
        if udata.exists():
            retain_tree(udata)
        if s.player and self.player_saves(s.player).exists():
            private_tree(self.player_saves(s.player))
            shutil.copytree(self.player_saves(s.player), udata)
        else:
            udata.mkdir(parents=True)

    def keep_save(self, s):
        udata = hdd_dir(self.install, s.instance) / "E" / "UDATA"
        if not s.player or not udata.exists() or not any(udata.rglob("*")):
            return
        dest = self.player_saves(s.player)
        private_tree(udata)
        private_tree(self.save_root, 2 * 1024 * 1024 * 1024, 32768)
        tmp = dest.with_name("UDATA.new-" + secrets.token_hex(8))
        shutil.copytree(udata, tmp)
        previous = None
        if dest.exists():
            previous = retain_tree(dest)
        try:
            tmp.rename(dest)
        except OSError:
            if previous is not None and not dest.exists():
                previous.rename(dest)
            raise

    # ---------- processes ----------
    def spawn(self, s, name, args, env=None, cwd=None):
        log = open(self.install / "logs" / f"playhost-{s.id}-{name}.log", "w", encoding="utf-8", errors="replace")
        e = dict(os.environ)
        e.update(env or {})
        flags = subprocess.CREATE_NEW_PROCESS_GROUP if os.name == "nt" else 0
        p = subprocess.Popen(args, cwd=cwd or self.install, env=e, stdout=log, stderr=subprocess.STDOUT,
                             creationflags=flags)
        s.procs[name] = (p, log)
        return p

    async def start(self, s):
        try:
            (self.install / "logs").mkdir(exist_ok=True)
            self.prepare_save(s)
            py = self.a.python
            game_env = {"MC3_RELAY_ONLY": "1", "MC3_REMOTE_PRIMARY": "1", "XBE_VERBOSE": "1"}
            if self.a.disc:
                game_env["MC3_DISC"] = str(self.a.disc)
            game = self.spawn(s, "game", [str(self.exe), "--instance", str(s.instance)], game_env)
            tunnel = [py, "-u", str(MULTI / "proxy" / "proxy.py"), "--relay", self.a.relay,
                      "--instance", str(s.instance), "--name", s.name]
            if s.room:
                tunnel += ["--room", s.room]
            self.spawn(s, "tunnel", tunnel, cwd=MULTI)
            await asyncio.sleep(self.a.window_wait)  # the stream proxy looks for the game's window when it starts
            stream = [py, "-u", str(MULTI / "stream" / "stream_proxy.py"), "--relay", self.a.relay,
                      "--port", str(s.stream_port), "--instance", str(s.instance), "--capture-window",
                      "--allow-control", "--origin", self.internal_origin, "--game-executable", str(self.exe),
                      "--game-pid", str(game.pid), "--fps", str(self.a.fps), "--scale", str(self.a.scale)]
            self.spawn(s, "stream", stream, {"MC3_STREAM_PAIRING_TOKEN": s.pair}, cwd=MULTI / "stream")
            s.state = "running"
            while not s.ended.is_set():
                dead = [n for n, (p, _) in s.procs.items() if p.poll() is not None]
                if dead:
                    print(f"[playhost] {s.id}: {', '.join(dead)} exited", flush=True)
                    break
                if not s.viewers and time.time() - s.last_viewer > self.a.idle:
                    print(f"[playhost] {s.id}: no browser for {self.a.idle}s", flush=True)
                    break
                try:
                    await asyncio.wait_for(s.ended.wait(), 2)
                except asyncio.TimeoutError:
                    pass
        except Exception as e:  # a copy that cannot start must not take the host down
            print(f"[playhost] {s.id}: start failed: {e}", flush=True)
        finally:
            await self.stop(s)

    async def stop(self, s):
        async with s.stop_lock:
            await self._stop(s)

    async def _stop(self, s):
        if s.state == "ended":
            return
        s.state = "ending"
        for name in ("stream", "tunnel", "game"):
            entry = s.procs.get(name)
            if not entry:
                continue
            p, log = entry
            if p.poll() is None:
                p.terminate()
                try:
                    await asyncio.wait_for(asyncio.to_thread(p.wait), 10)
                except asyncio.TimeoutError:
                    p.kill()
                    await asyncio.to_thread(p.wait)
            log.close()
        try:
            self.keep_save(s)
        except OSError as e:
            print(f"[playhost] {s.id}: save not kept: {e}", flush=True)
        s.state = "ended"
        s.ended.set()
        async with self.lock:
            self.sessions.pop(s.id, None)
        print(f"[playhost] {s.id}: ended (instance {s.instance})", flush=True)

    # ---------- HTTP ----------
    def client_ip(self, req):
        if self.a.trust_proxy:
            fwd = req.headers.get("X-Forwarded-For", "")
            if fwd:
                return fwd.split(",")[0].strip()
        return req.remote or "?"

    def same_origin(self, req):
        origin = req.headers.get("Origin")
        allowed = {self.internal_origin, f"http://localhost:{self.a.port}"} | set(self.a.public_origin or [])
        return origin is None or origin in allowed

    async def page(self, req):
        return web.FileResponse(HERE / "play.html", headers={"Cache-Control": "no-cache",
                                                               "Content-Security-Policy": "default-src 'self'; img-src 'self' data: blob:; connect-src 'self'; style-src 'self' 'unsafe-inline'; script-src 'self' 'unsafe-inline'"})

    async def status(self, req):
        free = sum(1 for n in self.a.instances if usable_slot(self.install, n)) - len(self.sessions)
        return web.json_response({"service": "mc3-playhost", "free": max(0, min(free, self.a.max_sessions - len(self.sessions))),
                                  "max": self.a.max_sessions, "sessions": [s.public() for s in self.sessions.values()]})

    async def create(self, req):
        if not self.same_origin(req):
            return web.json_response({"error": "Cross-site request refused."}, status=403)
        try:
            body = await req.json()
        except (json.JSONDecodeError, UnicodeDecodeError):
            return web.json_response({"error": "A JSON object is required."}, status=400)
        if not isinstance(body, dict):
            return web.json_response({"error": "A JSON object is required."}, status=400)
        name = str(body.get("name") or "").strip() or "Browser player"
        room = str(body.get("room") or "").strip()
        player = body.get("player")
        if not NAME.match(name) or not ROOM.match(room) or (player is not None and not (isinstance(player, str) and PLAYER.match(player))):
            return web.json_response({"error": "Invalid name, room or player key."}, status=400)
        ip = self.client_ip(req)
        async with self.lock:
            if sum(1 for x in self.sessions.values() if x.ip == ip) >= self.a.per_address:
                return web.json_response({"error": "This address already has a game running. Close it first."}, status=429)
            if len(self.sessions) >= self.a.max_sessions:
                return web.json_response({"error": "Every game slot on this host is in use. Try again shortly."}, status=503)
            slot = self.free_slot()
            if slot is None:
                return web.json_response({"error": "No free game slot on this host."}, status=503)
            if player and any(s.player == player for s in self.sessions.values()):
                return web.json_response({"error": "Your career is already open in another tab."}, status=409)
            s = Session(secrets.token_hex(8), secrets.token_hex(32), slot[0], slot[1], name, room, player)
            s.ip = ip
            self.sessions[s.id] = s
        asyncio.create_task(self.start(s))
        print(f"[playhost] {s.id}: {name!r} -> instance {s.instance}, {room or 'Main Lobby'}", flush=True)
        return web.json_response({"id": s.id, "key": s.key, "instance": s.instance,
                                  "room": room or "Main Lobby", "stream": f"/api/sessions/{s.id}/stream"}, status=201)

    def session_for(self, req):
        s = self.sessions.get(req.match_info["sid"])
        key = req.headers.get("X-Session-Key") or req.query.get("key", "")
        if not s or not isinstance(key, str) or not PLAYER.fullmatch(key) or not hmac.compare_digest(key, s.key):
            return None
        return s

    async def end(self, req):
        if not self.same_origin(req):
            return web.json_response({"error": "Cross-site request refused."}, status=403)
        s = self.session_for(req)
        if not s:
            return web.json_response({"error": "No such session."}, status=404)
        s.ended.set()
        return web.json_response({"ok": True})

    async def stream(self, req):
        """The browser's stream: this host pairs with the copy's stream proxy, then relays both ways."""
        if not self.same_origin(req):
            return web.Response(status=403)
        s = self.session_for(req)
        if not s:
            return web.Response(status=404)
        if s.viewers >= 1:
            return web.Response(status=429, text='A browser is already connected to this game.')
        s.viewers += 1
        ws = web.WebSocketResponse(heartbeat=20, max_msg_size=4096)
        try:
            await ws.prepare(req)
            upstream = None
            deadline = time.time() + self.a.start_timeout
            async with ClientSession() as http:
                while upstream is None and not s.ended.is_set() and not ws.closed:
                    await ws.send_json({"type": "host", "state": "starting", "instance": s.instance,
                                        "room": s.room or "Main Lobby"})
                    try:
                        upstream = await http.ws_connect(f"ws://127.0.0.1:{s.stream_port + 1}",
                                                         origin=self.internal_origin, max_msg_size=8 * 1024 * 1024)
                    except (ClientConnectorError, OSError, Exception):
                        if time.time() > deadline:
                            await ws.send_json({"type": "host", "state": "failed",
                                                "error": "The game did not start. Try again in a minute."})
                            return ws
                        await asyncio.sleep(1)
                if upstream is None:
                    return ws
                await upstream.send_str(json.dumps({"type": "pair", "token": s.pair}))
                await ws.send_json({"type": "host", "state": "running"})

                async def down():
                    async for m in upstream:
                        if m.type == WSMsgType.TEXT:
                            await asyncio.wait_for(ws.send_str(m.data), 2)
                        elif m.type == WSMsgType.BINARY:
                            await asyncio.wait_for(ws.send_bytes(m.data), 2)
                        else:
                            break

                async def up():
                    async for m in ws:
                        if m.type == WSMsgType.BINARY and len(m.data) == 24:
                            await upstream.send_bytes(m.data)  # controller reports only
                        elif m.type == WSMsgType.TEXT and len(m.data) <= 4096:
                            try:
                                msg = json.loads(m.data)
                            except json.JSONDecodeError:
                                continue
                            if isinstance(msg, dict) and msg.get("type") == "ping":
                                await upstream.send_str(json.dumps({"type": "ping", "ts": msg.get("ts", 0)}))
                        elif m.type in (WSMsgType.CLOSE, WSMsgType.ERROR):
                            break

                done, pending = await asyncio.wait([asyncio.create_task(down()), asyncio.create_task(up())],
                                                   return_when=asyncio.FIRST_COMPLETED)
                for t in pending:
                    t.cancel()
                await asyncio.gather(*pending, return_exceptions=True)
                for t in done:
                    if not t.cancelled():
                        t.exception()  # Consume forwarding failure; teardown releases the controller upstream.
                await upstream.close()
        finally:
            s.viewers -= 1
            s.last_viewer = time.time()
        return ws

    def app(self):
        app = web.Application(client_max_size=4096)
        app.router.add_get("/", self.page)
        app.router.add_get("/api/status", self.status)
        app.router.add_post("/api/sessions", self.create)
        app.router.add_delete("/api/sessions/{sid}", self.end)
        app.router.add_get("/api/sessions/{sid}/stream", self.stream)
        app.on_shutdown.append(self.shutdown)
        return app

    async def shutdown(self, _app):
        for s in list(self.sessions.values()):
            s.ended.set()
            await self.stop(s)


def instances(text):
    out = []
    for part in text.split(","):
        if "-" in part:
            lo, hi = part.split("-")
            out += range(int(lo), int(hi) + 1)
        elif part.strip():
            out.append(int(part))
    out = [n for n in dict.fromkeys(out) if 1 <= n <= 8 and n != 7]  # mc3.exe accepts 0-8; 7 is the owner's own
    if not out:
        raise argparse.ArgumentTypeError("no usable instance numbers (1-8, never 7)")
    return out


def parse(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--install", type=Path, default=ROOT / "templates" / "game" / "out" / "native",
                    help="folder holding mc3.exe (the local build the browser copies run)")
    ap.add_argument("--relay", default="ws://127.0.0.1:8080/ws", help="the relay everyone (local and browser) uses")
    ap.add_argument("--disc", type=Path, default=None, help="the game disc image (MC3_DISC), if not configured")
    ap.add_argument("--bind", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8790)
    ap.add_argument("--public-origin", action="append", help="exact https origin the page is served from publicly")
    ap.add_argument("--trust-proxy", action="store_true", help="take the client address from X-Forwarded-For")
    ap.add_argument("--instances", type=instances, default=list(DEFAULT_INSTANCES))
    ap.add_argument("--max-sessions", type=int, default=2, help="copies at once (each needs GPU time)")
    ap.add_argument("--per-address", type=int, default=1, help="games one client address may run at once")
    ap.add_argument("--idle", type=int, default=120, help="seconds without a browser before a copy is closed")
    ap.add_argument("--start-timeout", type=int, default=90)
    ap.add_argument("--window-wait", type=float, default=12, help="seconds before streaming the new window")
    ap.add_argument("--fps", type=int, default=30)
    ap.add_argument("--scale", type=float, default=0.75,
                    help="stream picture scale 0.25-1 (the stream is lossless PNG today: smaller is faster)")
    ap.add_argument("--saves", type=Path, default=None, help="where browser careers are kept between sessions")
    ap.add_argument("--python", default=sys.executable)
    args = ap.parse_args(argv)
    if not (1 <= args.max_sessions <= len(args.instances) and 1 <= args.per_address <= args.max_sessions):
        ap.error('Session limits must fit the selected instance slots.')
    if not (1 <= args.fps <= 60 and 0.25 <= args.scale <= 1 and 5 <= args.idle <= 3600
            and 5 <= args.start_timeout <= 300 and 0 <= args.window_wait <= 60 and 1024 <= args.port <= 65535):
        ap.error('Invalid stream/time/port budget.')
    return args


def main(argv=None):
    a = parse(argv)
    host = PlayHost(a)
    if not host.exe.exists():
        sys.exit(f"no mc3.exe in {host.install}")
    print(f"[playhost] http://{a.bind}:{a.port}/ -> {host.exe}, relay {a.relay}, "
          f"instances {a.instances}, up to {a.max_sessions} at once", flush=True)
    web.run_app(host.app(), host=a.bind, port=a.port, print=None, handle_signals=True)


if __name__ == "__main__":
    main()
