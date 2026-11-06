#!/usr/bin/env python3
"""
MC3 Browser Multiplayer — paired lossless PNG stream

Captures the mc3.exe game window and streams frames to browser clients
over WebSocket, while forwarding browser input back to the game via
UDP proxy + WebSocket relay.

Architecture:
  Browser (WebSocket) ←→ StreamProxy (WebSocket + capture + HW encode) ←→ Game (UDP via proxy)

Capture paths:

1. Game-window capture (default): exact mc3.exe path/process/instance validation,
   typed Win32 client-area capture → lossless PNG → base64 JSON frame.

2. Explicit monitor sharing: mss capture → lossless PNG. A missing game window
   never activates monitor capture. One bounded worker keeps capture off the
   controller/WebSocket event loop. H.264 is unavailable until complete framed
   access units and decoder configuration are implemented; no ffmpeg is started.

Requirements:
    pip install mss websockets
    Pillow only when selecting a scale other than native 1.0
"""

import asyncio
import argparse
import base64
import ctypes
import ctypes.wintypes
import io
import json
import os
from pathlib import Path
import shutil
import socket
import struct
import subprocess
import sys
import threading
import time
import websockets
import capture_backend
from stream_security import Pairing, Budget, decode_message, MAX_MESSAGE

try:
    import mss
    HAVE_MSS = True
except ImportError:
    HAVE_MSS = False

MAGIC = b'MC3P'
PAD_MAGIC = b'MC3I'      # 24-byte controller report from the browser
PAD_REPORT_SIZE = 24
PAD_PORT = 30771         # remote_pad.h kRemotePadPort; + instance
HEADER_SIZE = 8  # 4 magic + 2 src port + 2 dst port, little-endian

# Input packet types
INPUT_TYPE_KEY = 0x01
INPUT_TYPE_GAMEPAD = 0x02

# Win32 API constants for window enumeration
GWL_STYLE = -16
WS_VISIBLE = 0x10000000
WS_CHILD = 0x40000000


def _find_mc3_window(partial_title="Midnight Club 3", instance=0, executable=None, pid=None):
    return capture_backend.find_window(partial_title, instance, expected_executable=executable, expected_pid=pid)


class FFmpegEncoder:
    """H.264 stays unavailable until complete access-unit framing exists.

    Capture uses lossless PNG; no encoder/GPU subprocess is started. The old
    blocking pipe reader could neither keep input responsive nor delimit frames.
    """
    def __init__(self, width, height, fps, encoder_preference=None):
        self.width, self.height, self.fps = width, height, fps
        self.proc = self.encoder = None
        self.disabled_reason = "Framed H.264 is unavailable; lossless PNG capture is active."

    def encode(self, bgr_bytes):
        return b''

    def close(self):
        pass


class StreamProxy:
    def __init__(self, relay_url, listen_port, fps=60, quality=75, scale=1.0,
                 capture_monitor=1, capture_window=True, hw_encoder=None, instance=0,
                 pairing_token=None, allowed_origins=None, allow_control=False,
                 game_executable=None, game_pid=None):
        self.pairing = Pairing(pairing_token or os.environ.get('MC3_STREAM_PAIRING_TOKEN', ''),
                               allowed_origins or ['http://localhost:8080', 'http://127.0.0.1:8080'])
        self.allow_control = allow_control
        self.relay_url = relay_url
        # Browser controllers go to the game's remote controller listener (runtime/core/remote_pad.h), never
        # into its network socket. Port 0 stays the host's own controller; browsers take ports 1-3.
        self.pad_addr = ('127.0.0.1', PAD_PORT + instance)
        self.pad_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.pad_ports = {}                     # browser websocket -> controller port
        self.listen_port = listen_port          # UDP port for game packets
        self.ws_port = listen_port + 1          # WebSocket port for browsers
        self.fps = fps
        self.quality = quality
        self.scale = scale                      # 1.0 = native, 0.5 = half res
        self.capture_monitor = capture_monitor  # which monitor to capture (PNG path)
        self.capture_window = capture_window    # True → match MC3 window by title (HW path)
        self.hw_encoder = hw_encoder            # preferred ffmpeg encoder name
        self.game_executable = (Path(game_executable) if game_executable is not None else
                                Path(__file__).resolve().parents[3] / 'templates/game/out/native/mc3.exe')
        self.game_pid = game_pid

        self.running = False
        self.ws_clients = set()                 # browser websocket clients
        self.relay_ws = None                    # websocket to relay
        self.game_port = None                   # learned from game's bind()
        self.udp_sock = None
        self.last_frame_time = 0
        self.frame_interval = 1.0 / fps
        self.frame_count = 0
        self.bytes_sent = 0
        self.clients_ack = {}                   # client -> last frame acked (for adaptive)

        # Screen capture state (PNG/monitor fallback path)
        self.sct = None
        self.monitor = None

        # HW-encode state
        self.encoder = None
        self.enc_width = 0
        self.enc_height = 0

    # ---- Screen capture ----
    def setup_capture(self):
        self.stream_codec = 'png'
        if self.capture_window:
            try:
                capture_backend.target_path(self.game_executable)
                if not self.game_executable.is_file():
                    raise capture_backend.CaptureError("Selected mc3.exe is unavailable.")
                win = _find_mc3_window("Midnight Club 3", self.pad_addr[1] - PAD_PORT,
                                       self.game_executable, self.game_pid)
            except (OSError, capture_backend.CaptureError) as error:
                print(f"[stream] game capture target rejected: {error}")
                return False
            if not win:
                print("[stream] Selected MC3 process/window not found; capture was not started")
                return False
            self.enc_width, self.enc_height = win['width'], win['height']
            self.mc3_hwnd, self.mc3_pid = win['hwnd'], win['pid']
            self.mc3_creation = win['creation']
            self.mc3_title = win.get('title', 'MC3')
            print(f"[stream] window-only lossless PNG: '{self.mc3_title}' "
                  f"{self.enc_width}x{self.enc_height}")
            self._start_encoder()
            return True
        if not HAVE_MSS:
            print("[stream] ERROR: mss is needed for explicitly selected monitor capture")
            return False
        try:
            # MSS GDI contexts are thread-local; capture opens its own context.
            with mss.mss() as screen:
                if not 0 <= self.capture_monitor < len(screen.monitors):
                    raise capture_backend.CaptureError("The selected monitor does not exist.")
                self.monitor = dict(screen.monitors[self.capture_monitor])
            capture_backend.dimensions(self.monitor['width'], self.monitor['height'])
            self.enc_width, self.enc_height = self.monitor['width'], self.monitor['height']
            print(f"[stream] explicitly selected monitor {self.capture_monitor}: "
                  f"{self.enc_width}x{self.enc_height}, lossless PNG")
            return True
        except Exception as error:
            print(f"[stream] capture setup failed: {error}")
            return False

    def _start_encoder(self):
        self.encoder = None
        self.stream_codec = 'png'
        if self.hw_encoder:
            print("[stream] H.264 access-unit framing is unfinished; using CPU lossless PNG")

    def capture_frame(self):
        """One complete PNG; all capture resources remain scoped to this worker."""
        if self.capture_window and hasattr(self, 'mc3_hwnd'):
            return self._capture_window_frame()
        if not self.monitor or not HAVE_MSS:
            return None
        with mss.mss() as screen:
            image = screen.grab(self.monitor)
            width, height = image.size
            rgb = image.rgb
        return ('png', capture_backend.encode_png(width, height, rgb, self.scale))

    def _capture_window_frame(self):
        width, height, rgb = capture_backend.capture_window(
            self.mc3_hwnd, self.mc3_pid, expected_executable=self.game_executable,
            expected_creation=self.mc3_creation)
        self.enc_width, self.enc_height = width, height
        return ('png', capture_backend.encode_png(width, height, rgb, self.scale))

    # ---- Network helpers ----
    def make_header(self, src_port, dst_port=0):
        return MAGIC + struct.pack('<H', src_port) + struct.pack('<H', dst_port)

    def parse_header(self, data):
        """Return (src_port, dst_port, payload). dst_port is 0 when absent."""
        if len(data) < HEADER_SIZE:
            return None, None, data
        magic = data[:4]
        if magic != MAGIC:
            return None, None, data
        src_port = struct.unpack('<H', data[4:6])[0]
        dst_port = struct.unpack('<H', data[6:8])[0]
        return src_port, dst_port, data[HEADER_SIZE:]

    # ---- Browser WebSocket handler ----
    async def browser_handler(self, websocket):
        """Pair before exposing frames or accepting input. Account credentials aren't accepted here."""
        request = getattr(websocket, 'request', None)
        headers = request.headers if request else getattr(websocket, 'request_headers', {})
        try:
            origin = headers.get('Origin')
        except Exception:
            origin = None
        if not self.pairing.admit(websocket, origin):
            await websocket.close(1008, 'Host pairing unavailable')
            return
        try:
            try:
                raw = await asyncio.wait_for(websocket.recv(), timeout=3)
            except asyncio.TimeoutError:
                await websocket.close(1008, 'Host pairing timed out')
                return
            except websockets.exceptions.ConnectionClosed:
                return
            if not self.pairing.authenticate(websocket, raw):
                await websocket.close(1008, 'Host pairing failed')
                return
            self.ws_clients.add(websocket)
            budget = Budget()
            # Send welcome with stream params
            await websocket.send(json.dumps({
                "type": "hello",
                "paired": True,
                "control": self.allow_control,
                "fps": self.fps,
                "quality": self.quality,
                "scale": self.scale,
                "width": self.enc_width or (self.monitor['width'] if self.monitor else 0),
                "height": self.enc_height or (self.monitor['height'] if self.monitor else 0),
                "codec": "png",  # H.264 stays unavailable until complete access-unit framing is verified.
            }))

            async for message in websocket:
                size = len(message) if isinstance(message, bytes) else len(message.encode('utf-8'))
                if not budget.take(size):
                    await websocket.close(1008, 'Stream message limit reached')
                    break
                if isinstance(message, bytes):
                    self.forward_pad_report(websocket, message)
                else:
                    try:
                        msg = decode_message(message)
                        await self.handle_browser_msg(websocket, msg)
                    except (ValueError, TypeError, OverflowError, RecursionError):
                        await websocket.close(1008, 'Invalid stream message')
                        break
        finally:
            self.release_pad(websocket)
            self.ws_clients.discard(websocket)
            self.pairing.release(websocket)
            print(f"[stream] browser disconnected")

    async def handle_browser_msg(self, websocket, msg):
        mtype = msg.get("type")
        if mtype == "join":
            name = msg.get("name", "BrowserPlayer")
            if self.relay_ws:
                await self.relay_ws.send(json.dumps({
                    "type": "join",
                    "name": name,
                }))
        elif mtype == "create":
            name = msg.get("name", "Host")
            if self.relay_ws:
                await self.relay_ws.send(json.dumps({
                    "type": "create",
                    "name": name,
                }))
        elif mtype == "list":
            if self.relay_ws:
                await self.relay_ws.send(json.dumps({"type": "list"}))
        elif mtype == "chat":
            if self.relay_ws:
                await self.relay_ws.send(json.dumps({
                    "type": "chat",
                    "text": msg.get("text", ""),
                }))
        elif mtype == "settings":
            q = msg.get("quality")
            s = msg.get("scale")
            f = msg.get("fps")
            if q is not None:
                self.quality = max(10, min(100, int(q)))
            if s is not None:
                self.scale = max(0.25, min(1.0, float(s)))
            if f is not None:
                self.fps = max(1, min(120, int(f)))
                self.frame_interval = 1.0 / self.fps
            print(f"[stream] settings updated: quality={self.quality}, "
                  f"scale={self.scale}, fps={self.fps}")
        elif mtype == "ping":
            await websocket.send(json.dumps({"type": "pong", "ts": msg.get("ts", 0)}))
        elif mtype == "load_save":
            # Browser requests that the native game import/load a cloud save.
            # Forward the opaque save_data as a UDP datagram to the game's bound
            # port so native code (when a save-import message handler exists) can
            # process it. Reply to the browser with an acknowledgment; native_ack
            # is set by the game only after it actually imports the save.
            slot = msg.get("slot")
            save_data = msg.get("save_data")
            save_name = msg.get("save_name")
            if slot is None or save_data is None:
                try:
                    await websocket.send(json.dumps({
                        "type": "load_save_ack", "slot": slot, "ok": False,
                        "error": "slot and save_data required"
                    }))
                except Exception:
                    pass
                return
            # The game has no network save import: a career loads from its own save files. Say so instead of
            # pushing JSON into the game's System Link socket and reporting success.
            try:
                await websocket.send(json.dumps({
                    "type": "load_save_ack", "slot": slot, "ok": False, "save_name": save_name,
                    "error": "Loading a cloud save into a running game is not supported yet; "
                             "restore it to the save folder and load it from the game's menu."
                }))
            except Exception:
                pass

    def forward_pad_report(self, websocket, data):
        """A browser's 24-byte controller report -> the game's remote controller port for that browser."""
        if not self.allow_control or websocket not in self.pairing.paired:
            return
        if len(data) != PAD_REPORT_SIZE or data[:4] != PAD_MAGIC:
            return
        port = self.pad_ports.get(websocket)
        if port is None:
            free = [p for p in (1, 2, 3) if p not in self.pad_ports.values()]
            if not free:
                return  # four players already
            port = self.pad_ports[websocket] = free[0]
            print(f"[stream] browser {websocket.remote_address} is controller {port + 1}")
        report = bytearray(data)
        report[4] = port
        try:
            self.pad_sock.sendto(bytes(report), self.pad_addr)
        except OSError as e:
            print(f"[stream] controller send error: {e}")

    def release_pad(self, websocket):
        port = self.pad_ports.pop(websocket, None)
        if port is None:
            return
        unplug = bytearray(PAD_REPORT_SIZE)
        unplug[:4] = PAD_MAGIC
        unplug[4] = port
        try:
            self.pad_sock.sendto(bytes(unplug), self.pad_addr)
        except OSError:
            pass

    # ---- Relay WebSocket ----
    async def relay_handler(self):
        """WebSocket connection to the relay server."""
        print(f"[stream] connecting to relay: {self.relay_url}")
        try:
            async with websockets.connect(self.relay_url) as ws:
                self.relay_ws = ws
                # Guest 'join' as a game client (stream proxies have no user account/token)
                await ws.send(json.dumps({
                    "type": "join",
                    "clientType": "game",
                    "name": f"stream-{socket.gethostname()}",
                }))
                async for message in ws:
                    if isinstance(message, bytes):
                        # Binary packet from relay → forward to game.
                        # The MC3P header carries dst_port so relay→game traffic is
                        # routed to the correct bound socket (per-peer identity).
                        src_port, dst_port, payload = self.parse_header(message)
                        if src_port is None:
                            continue
                        target_port = dst_port or self.game_port
                        if target_port and self.udp_sock:
                            try:
                                self.udp_sock.sendto(payload, ('127.0.0.1', target_port))
                            except OSError:
                                pass
                    else:
                        try:
                            msg = json.loads(message)
                        except json.JSONDecodeError:
                            continue
                        await self.handle_relay_msg(msg)
        except Exception as e:
            print(f"[stream] relay connection error: {e}")
        finally:
            self.relay_ws = None

    async def handle_relay_msg(self, msg):
        if msg.get("type") == "auth_ok":
            print(f"[stream] authenticated with relay as {msg.get('user', {}).get('username', '?')}")
        elif msg.get("type") == "joined":
            print(f"[stream] joined relay as {msg.get('role', '?')}")
        elif msg.get("type") == "gameList":
            await self.broadcast_to_browsers({
                "type": "gameList",
                "games": msg.get("games", []),
            })

    # ---- Broadcast ----
    async def _send_browser(self, ws, message):
        if ws not in self.pairing.paired:
            return
        try:
            await asyncio.wait_for(ws.send(message), 0.25)
        except Exception:
            # A stalled viewer loses its input lease and pairing slot immediately.
            self.release_pad(ws)
            self.ws_clients.discard(ws)
            self.pairing.release(ws)
            try:
                await asyncio.wait_for(ws.close(1008, 'Stream client is too slow'), 0.5)
            except Exception:
                transport = getattr(ws, 'transport', None)
                if transport:
                    transport.abort()

    async def broadcast_frame(self, kind, frame_bytes):
        """Send a frame to all connected browser clients.

        kind 'png'   → JSON {type:'frame', data:base64} (legacy, compatible)
        kind 'h264'  → WebSocket binary H.264 Annex-B bytes (hardware-accelerated)
        """
        if not self.ws_clients:
            return
        if kind == 'png':
            msg = json.dumps({
                "type": "frame",
                "data": base64.b64encode(frame_bytes).decode('ascii'),
                "ts": time.time() * 1000,
            })
            await asyncio.gather(*(self._send_browser(ws, msg) for ws in list(self.ws_clients)))
        elif kind == 'h264':
            await asyncio.gather(*(self._send_browser(ws, frame_bytes) for ws in list(self.ws_clients)))

    async def broadcast_to_browsers(self, msg_dict):
        """Send a JSON message to all browser clients."""
        if not self.ws_clients:
            return
        msg = json.dumps(msg_dict)
        await asyncio.gather(*(self._send_browser(ws, msg) for ws in list(self.ws_clients)))

    # ---- Loops ----
    async def capture_loop(self):
        """One bounded worker; slow capture cannot stall pad/WebSocket handling."""
        mode = "window-only+PNG" if self.capture_window else "explicit-monitor+PNG"
        print(f"[stream] capture loop starting ({mode}, target {self.fps} fps, scale={self.scale})")
        worker = capture_backend.CaptureWorker(self.capture_frame)
        next_error_log = 0.0
        try:
            while self.running:
                now = time.monotonic()
                completed = worker.poll()
                if completed is not None:
                    ok, result = completed
                    if ok and result and self.ws_clients:
                        kind, frame_bytes = result
                        self.frame_count += 1
                        self.bytes_sent += len(frame_bytes)
                        await self.broadcast_frame(kind, frame_bytes)
                    elif not ok and now >= next_error_log:
                        print(f"[stream] capture failed: {result}")
                        next_error_log = now + 5.0
                if self.ws_clients and now - self.last_frame_time >= self.frame_interval:
                    if worker.request():
                        self.last_frame_time = now
                await asyncio.sleep(0.002 if self.ws_clients else 0.1)
        finally:
            worker.close()

    async def udp_reader(self):
        """Read UDP packets from the game proxy and forward to relay."""
        loop = asyncio.get_event_loop()
        print(f"[stream] UDP listener on 127.0.0.1:{self.listen_port}")
        while self.running:
            try:
                data, addr = await loop.sock_recvfrom(self.udp_sock, 65535)
            except OSError:
                break
            src_port, dst_port, payload = self.parse_header(data)
            if src_port is not None:
                # The game is telling us its bound port (per-socket peer identity).
                # Do NOT `continue` here: a datagram may carry header + payload together.
                self.game_port = src_port
                print(f"[stream] learned game port: {src_port} (dst={dst_port})")

            # Forward to the relay only datagrams that came from the game with a valid
            # MC3P header. Raw bytes injected via UDP by forward_input_to_game (browser->
            # game direction) lack the MC3P magic and must not be echoed back to the
            # relay, otherwise they form a feedback loop.
            if src_port is not None and payload and self.relay_ws:
                try:
                    await self.relay_ws.send(json.dumps({
                        "type": "input",
                        "data": json.dumps({
                            "payload": base64.b64encode(payload).decode('ascii'),
                            "src_port": src_port,
                            "dst_port": dst_port,
                            "t": time.time() * 1000,
                        }),
                    }))
                except Exception:
                    pass

    async def run(self):
        self.running = True

        if not self.setup_capture():
            return

        # The native MC3R packet tunnel belongs to proxy.py. A stream host never
        # forwards relay-selected UDP destinations or impersonates a game peer.
        ws_server = await websockets.serve(
            self.browser_handler,
            "127.0.0.1",
            self.ws_port,
            origins=list(self.pairing.origins),
            max_size=MAX_MESSAGE, max_queue=4, write_limit=65536,
            compression=None, open_timeout=3, ping_interval=20, ping_timeout=10, close_timeout=1,
        )
        print(f"[stream] paired browser WebSocket on ws://127.0.0.1:{self.ws_port}")

        try:
            await self.capture_loop()
        finally:
            ws_server.close()
            await ws_server.wait_closed()
            self.stop()

    def stop(self):
        self.running = False
        if self.encoder:
            self.encoder.close()
        if self.udp_sock:
            self.udp_sock.close()
        self.pad_sock.close()
        if self.sct:
            self.sct.close()


def main():
    parser = argparse.ArgumentParser(description='MC3 Stream Proxy (video + input)')
    parser.add_argument('--relay', default='ws://localhost:8080', help='Relay WebSocket URL')
    parser.add_argument('--port', type=int, default=8765, help='UDP port for game; browser WS is port+1')
    parser.add_argument('--fps', type=int, default=60, help='Target capture FPS')
    parser.add_argument('--quality', type=int, default=75, help='PNG quality hint 1-100')
    parser.add_argument('--scale', type=float, default=1.0, help='Render scale 0.25-1.0')
    parser.add_argument('--monitor', type=int, default=None, help='Explicitly share a whole monitor instead of only the game window')
    parser.add_argument('--capture-window', action='store_true',
                        help='Capture the MC3 game window by title instead of a monitor')
    parser.add_argument('--instance', type=int, default=0, help='game instance (mc3.exe --instance N)')
    parser.add_argument('--game-executable', type=Path, default=None,
                        help='Absolute existing mc3.exe to capture; default is the canonical workspace executable, never launched')
    parser.add_argument('--game-pid', type=int, default=None,
                        help='Optional existing native process ID to narrow the capture target')
    parser.add_argument('--origin', action='append', help='Exact browser origin allowed to pair (repeatable)')
    parser.add_argument('--allow-control', action='store_true', help='Let paired viewers use additional controller ports; off by default')
    parser.add_argument('--hw-encoder', default=None,
                        help='Reserved for framed H.264; currently uses CPU lossless PNG without starting ffmpeg')
    args = parser.parse_args()

    if not (1 <= args.port <= 65534 and 0 <= args.instance <= 8 and 1 <= args.fps <= 60 and
            10 <= args.quality <= 100 and 0.25 <= args.scale <= 1.0):
        parser.error('Port, instance, FPS, quality or scale is outside its supported range')
    if not HAVE_MSS and args.monitor is not None:
        print("[stream] ERROR: mss not installed. Run: pip install mss websockets")
        sys.exit(1)

    proxy = StreamProxy(args.relay, args.port, args.fps, args.quality, args.scale,
                        args.monitor or 1, args.monitor is None, args.hw_encoder, args.instance,
                        allowed_origins=args.origin, allow_control=args.allow_control,
                        game_executable=args.game_executable, game_pid=args.game_pid)
    try:
        asyncio.run(proxy.run())
    except KeyboardInterrupt:
        proxy.stop()
        print("\n[stream] stopped")


if __name__ == '__main__':
    main()
