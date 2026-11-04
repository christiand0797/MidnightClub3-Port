#!/usr/bin/env python3
"""
MC3 Browser Multiplayer — Stream Proxy (GPU-optimized)

Captures the mc3.exe game window and streams frames to browser clients
over WebSocket, while forwarding browser input back to the game via
UDP proxy + WebSocket relay.

Architecture:
  Browser (WebSocket) ←→ StreamProxy (WebSocket + capture + HW encode) ←→ Game (UDP via proxy)

Two capture/encode paths:

1. Monitor capture (PNG, CPU, fallback): whole-monitor mss capture → PNG → base64 → JSON frame.
   Existing behaviour; usable where ffmpeg / a hardware encoder is unavailable.

2. Game-window capture + HW encode (H.264, GPU): enumerate Win32 windows, match the MC3 game
   window by title, capture its client rect, pipe raw BGR frames to an ffmpeg subprocess using a
   hardware encoder (h264_nvenc / h264_qsv / h264_amf). H.264 Annex-B bytes are broadcast as
   WebSocket *binary* frames — no base64, no per-frame JSON. The browser decodes via WebCodecs
   VideoDecoder or a <video> element. This is the recommended path for hardware-accelerated,
   game-window-only video in Firefox/Chromium.

Requirements:
    pip install mss websockets
    ffmpeg (winget Gyan.FFmpeg) for the hardware-encode path
"""

import asyncio
import base64
import ctypes
import ctypes.wintypes
import io
import json
import os
import shutil
import socket
import struct
import subprocess
import sys
import threading
import time
import websockets

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


def _find_mc3_window(partial_title="Midnight Club 3"):
    """Return (hwnd, width, height) for the first visible top-level window whose
    title contains `partial_title` (case-insensitive), or None if not found."""
    if not ctypes:
        return None
    user32 = ctypes.windll.user32
    gdi32 = ctypes.windll.gdi32

    found = {}

    @ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.wintypes.HWND, ctypes.wintypes.LPARAM)
    def enum_proc(hwnd, lparam):
        if not user32.IsWindowVisible(hwnd):
            return True
        length = user32.GetWindowTextLengthW(hwnd)
        if length == 0:
            return True
        buf = ctypes.create_unicode_buffer(length + 1)
        user32.GetWindowTextW(hwnd, buf, length + 1)
        title = buf.value
        if partial_title.lower() in title.lower():
            style = user32.GetWindowLongW(hwnd, GWL_STYLE)
            if style & WS_CHILD:
                return True
            rect = ctypes.wintypes.RECT()
            if user32.GetClientRect(hwnd, ctypes.byref(rect)):
                found['hwnd'] = hwnd
                found['width'] = rect.right - rect.left
                found['height'] = rect.bottom - rect.top
                found['title'] = title
                return False  # stop enumeration
        return True

    user32.EnumWindows(enum_proc, 0)
    return found or None


class FFmpegEncoder:
    """Subprocess ffmpeg that consumes raw BGR24 frames on stdin and emits
    H.264 Annex-B bytes on stdout via a hardware encoder."""

    def __init__(self, width, height, fps, encoder_preference=None):
        self.width = width
        self.height = height
        self.fps = fps
        self.proc = None
        self.encoder = None
        self._start(encoder_preference)

    def _detect_encoder(self, preference):
        """Pick the first available hardware encoder; fall back to none."""
        candidates = []
        if preference:
            candidates.append(preference)
        # NVIDIA NVENC, Intel QuickSync, AMD AMF, then software libx264 last resort
        candidates += ['h264_nvenc', 'h264_qsv', 'h264_amf', 'libx264']
        ffmpeg = shutil.which('ffmpeg')
        if not ffmpeg:
            return None, None
        for enc in candidates:
            if enc == 'libx264':
                return 'libx264', ffmpeg  # software fallback always acceptable
            # Probe encoder availability
            try:
                out = subprocess.run(
                    [ffmpeg, '-hide_banner', '-encoders'],
                    capture_output=True, text=True, timeout=10
                )
                if enc in out.stdout:
                    return enc, ffmpeg
            except Exception:
                pass
        return None, None

    def _start(self, preference):
        self.encoder, ffmpeg = self._detect_encoder(preference)
        if not self.encoder:
            print(f"[stream] no ffmpeg hardware encoder available; HW-encode path disabled")
            return
        # Build ffmpeg command: raw BGR24 → H.264 Annex-B, low latency, tune for streaming
        cmd = [
            ffmpeg,
            '-hide_banner', '-loglevel', 'error',
            '-f', 'rawvideo',
            '-pixel_format', 'bgr24',
            '-video_size', f'{self.width}x{self.height}',
            '-framerate', str(self.fps),
            '-i', '-',
            '-c:v', self.encoder,
            '-preset', 'p1' if self.encoder == 'h264_nvenc' else 'medium',
            '-tune', 'hq',
            '-rc', 'vbr' if self.encoder == 'h264_nvenc' else '',
            '-rc-lookahead', '0',
            '-f', 'h264',
            '-an',
            '-',
        ]
        cmd = [c for c in cmd if c != '']
        try:
            self.proc = subprocess.Popen(
                cmd,
                stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,
                stderr=subprocess.DEVNULL,
            )
            print(f"[stream] ffmpeg HW encoder started: {self.encoder} "
                  f"{self.width}x{self.height}@{self.fps}fps")
        except Exception as e:
            print(f"[stream] ffmpeg start failed: {e}")
            self.proc = None

    def encode(self, bgr_bytes):
        """Feed one raw BGR24 frame; return H.264 bytes available now (may be b'')."""
        if not self.proc or not self.proc.stdin:
            return b''
        try:
            self.proc.stdin.write(bgr_bytes)
            self.proc.stdin.flush()
        except (BrokenPipeError, OSError):
            self.proc = None
            return b''
        # Drain available output without blocking
        out = b''
        try:
            while True:
                chunk = self.proc.stdout.read1(65536)
                if not chunk:
                    break
                out += chunk
        except (BlockingIOError, OSError):
            pass
        return out

    def close(self):
        try:
            if self.proc and self.proc.stdin:
                self.proc.stdin.close()
        except Exception:
            pass
        try:
            if self.proc:
                self.proc.terminate()
                self.proc.wait(timeout=3)
        except Exception:
            try:
                self.proc.kill()
            except Exception:
                pass


class StreamProxy:
    def __init__(self, relay_url, listen_port, fps=60, quality=75, scale=1.0,
                 capture_monitor=1, capture_window=False, hw_encoder=None, instance=0):
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
        if self.capture_window:
            win = _find_mc3_window("Midnight Club 3")
            if win:
                self.enc_width = win['width']
                self.enc_height = win['height']
                self.mc3_hwnd = win['hwnd']
                self.mc3_title = win.get('title', 'MC3')
                print(f"[stream] window capture: '{self.mc3_title}' "
                      f"{self.enc_width}x{self.enc_height} (hwnd={win['hwnd']})")
                self._start_encoder()
                return True
            print("[stream] MC3 window not found by title; falling back to monitor capture")

        if not HAVE_MSS:
            print("[stream] ERROR: mss not installed. Run: pip install mss websockets")
            return False
        try:
            self.sct = mss.mss()
            monitors = self.sct.monitors
            if self.capture_monitor < len(monitors):
                self.monitor = monitors[self.capture_monitor]
            else:
                self.monitor = monitors[1] if len(monitors) > 1 else monitors[0]
            print(f"[stream] capture monitor {self.capture_monitor}: "
                  f"{self.monitor['width']}x{self.monitor['height']}")
            return True
        except Exception as e:
            print(f"[stream] capture setup failed: {e}")
            return False

    def _start_encoder(self):
        if self.encoder:
            return  # already running
        self.encoder = FFmpegEncoder(self.enc_width, self.enc_height, self.fps, self.hw_encoder)

    def capture_frame(self):
        """Capture one frame. Returns (kind, bytes) where kind is 'png' or 'h264', or None."""
        if self.capture_window and hasattr(self, 'mc3_hwnd'):
            return self._capture_window_frame()
        if not self.sct or not self.monitor:
            return None
        try:
            img = self.sct.grab(self.monitor)
            buf = io.BytesIO()
            img.save(buf, format="PNG")
            png_bytes = buf.getvalue()
            if self.scale < 1.0:
                from PIL import Image  # only needed for scaling
                pil_img = Image.open(io.BytesIO(png_bytes))
                new_w = max(320, int(pil_img.width * self.scale))
                new_h = max(240, int(pil_img.height * self.scale))
                pil_img = pil_img.resize((new_w, new_h), Image.LANCZOS)
                buf2 = io.BytesIO()
                pil_img.save(buf2, format="PNG")
                png_bytes = buf2.getvalue()
            return ('png', png_bytes)
        except Exception as e:
            print(f"[stream] capture error: {e}")
            return None

    def _capture_window_frame(self):
        """Capture the MC3 client rect via PrintWindow, encode via ffmpeg HW encoder."""
        try:
            user32 = ctypes.windll.user32
            gdi32 = ctypes.windll.gdi32

            hwnd = self.mc3_hwnd
            rect = ctypes.wintypes.RECT()
            if not user32.GetClientRect(hwnd, ctypes.byref(rect)):
                return None
            width = rect.right - rect.left
            height = rect.bottom - rect.top
            if width <= 0 or height <= 0:
                return None

            hwnd_dc = user32.GetDC(hwnd)
            mem_dc = gdi32.CreateCompatibleDC(hwnd_dc)
            bmp = gdi32.CreateCompatibleBitmap(hwnd_dc, width, height)
            gdi32.SelectObject(mem_dc, bmp)
            # PrintWindow copies the window's client area (PW_CLIENTONLY = 1)
            res = user32.PrintWindow(hwnd, mem_dc, 1)
            if res == 0:
                gdi32.DeleteObject(bmp)
                gdi32.DeleteDC(mem_dc)
                user32.ReleaseDC(hwnd, hwnd_dc)
                return None

            bmi = ctypes.wintypes.BITMAPINFO()
            bmi.bmiHeader.biSize = ctypes.sizeof(ctypes.wintypes.BITMAPINFOHEADER)
            bmi.bmiHeader.biWidth = width
            bmi.bmiHeader.biHeight = -height  # top-down DIB
            bmi.bmiHeader.biPlanes = 1
            bmi.bmiHeader.biBitCount = 24
            bmi.bmiHeader.biCompression = 0  # BI_RGB

            buf_size = width * height * 3
            buf = ctypes.create_string_buffer(buf_size)
            gdi32.GetDIBits(mem_dc, bmp, 0, height, buf, ctypes.byref(bmi), 0)  # DIB_RGB_COLORS=0

            gdi32.DeleteObject(bmp)
            gdi32.DeleteDC(mem_dc)
            user32.ReleaseDC(hwnd, hwnd_dc)

            if res == 0:
                return None

            # Re-encode if dimensions changed (e.g. window resized)
            if width != self.enc_width or height != self.enc_height:
                self.enc_width, self.enc_height = width, height
                self._start_encoder()

            if not self.encoder or not self.encoder.proc:
                return None

            h264 = self.encoder.encode(buf.raw)
            if h264:
                return ('h264', h264)
            return None
        except Exception as e:
            print(f"[stream] window capture error: {e}")
            return None

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
        """Handle browser client connections."""
        print(f"[stream] browser connected: {websocket.remote_address}")
        self.ws_clients.add(websocket)
        try:
            # Send welcome with stream params
            await websocket.send(json.dumps({
                "type": "hello",
                "fps": self.fps,
                "quality": self.quality,
                "scale": self.scale,
                "width": self.enc_width or (self.monitor['width'] if self.monitor else 0),
                "height": self.enc_height or (self.monitor['height'] if self.monitor else 0),
                "codec": "h264" if (self.capture_window and self.encoder) else "png",
            }))

            async for message in websocket:
                if isinstance(message, bytes):
                    self.forward_pad_report(websocket, message)
                else:
                    try:
                        msg = json.loads(message)
                    except json.JSONDecodeError:
                        continue
                    await self.handle_browser_msg(websocket, msg)
        finally:
            self.release_pad(websocket)
            self.ws_clients.discard(websocket)
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
            dead = set()
            for ws in list(self.ws_clients):
                try:
                    await ws.send(msg)
                except Exception:
                    dead.add(ws)
            self.ws_clients -= dead
        elif kind == 'h264':
            dead = set()
            for ws in list(self.ws_clients):
                try:
                    await ws.send(frame_bytes)  # binary frame
                except Exception:
                    dead.add(ws)
            self.ws_clients -= dead

    async def broadcast_to_browsers(self, msg_dict):
        """Send a JSON message to all browser clients."""
        if not self.ws_clients:
            return
        msg = json.dumps(msg_dict)
        dead = set()
        for ws in list(self.ws_clients):
            try:
                await ws.send(msg)
            except Exception:
                dead.add(ws)
        self.ws_clients -= dead

    # ---- Loops ----
    async def capture_loop(self):
        """Capture frames and broadcast at target FPS."""
        mode = "window+HW" if self.capture_window else "monitor+PNG"
        print(f"[stream] capture loop starting ({mode}, {self.fps} fps, "
              f"scale={self.scale})")
        while self.running:
            now = time.time()
            if now - self.last_frame_time < self.frame_interval:
                await asyncio.sleep(0.002)
                continue
            if not self.ws_clients:
                # No browsers — don't waste CPU capturing
                await asyncio.sleep(0.1)
                continue
            result = self.capture_frame()
            if result:
                kind, frame_bytes = result
                self.last_frame_time = now
                self.frame_count += 1
                self.bytes_sent += len(frame_bytes)
                await self.broadcast_frame(kind, frame_bytes)
            await asyncio.sleep(0)

    async def udp_reader(self):
        """Read UDP packets from the game proxy and forward to relay."""
        loop = asyncio.get_event_loop()
        print(f"[stream] UDP listener on 0.0.0.0:{self.listen_port}")
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

        # UDP socket for game <-> proxy
        self.udp_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.udp_sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.udp_sock.setblocking(False)
        self.udp_sock.bind(('0.0.0.0', self.listen_port))

        # WebSocket server for browsers
        ws_server = await websockets.serve(
            self.browser_handler,
            "0.0.0.0",
            self.ws_port,
        )
        print(f"[stream] browser WebSocket on ws://0.0.0.0:{self.ws_port}")
        print(f"[stream] UDP game port on 0.0.0.0:{self.listen_port}")

        await asyncio.gather(
            self.capture_loop(),
            self.udp_reader(),
            self.relay_handler(),
        )

    def stop(self):
        self.running = False
        if self.encoder:
            self.encoder.close()
        if self.udp_sock:
            self.udp_sock.close()


def main():
    parser = argparse.ArgumentParser(description='MC3 Stream Proxy (video + input)')
    parser.add_argument('--relay', default='ws://localhost:8080', help='Relay WebSocket URL')
    parser.add_argument('--port', type=int, default=8765, help='UDP port for game; browser WS is port+1')
    parser.add_argument('--fps', type=int, default=60, help='Target capture FPS')
    parser.add_argument('--quality', type=int, default=75, help='PNG quality hint 1-100')
    parser.add_argument('--scale', type=float, default=1.0, help='Render scale 0.25-1.0')
    parser.add_argument('--monitor', type=int, default=1, help='Monitor index to capture (fallback path)')
    parser.add_argument('--capture-window', action='store_true',
                        help='Capture the MC3 game window by title instead of a monitor')
    parser.add_argument('--instance', type=int, default=0, help='game instance (mc3.exe --instance N)')
    parser.add_argument('--hw-encoder', default=None,
                        help='Preferred ffmpeg encoder: h264_nvenc, h264_qsv, h264_amf')
    args = parser.parse_args()

    if not HAVE_MSS and not args.capture_window:
        print("[stream] ERROR: mss not installed. Run: pip install mss websockets")
        sys.exit(1)

    proxy = StreamProxy(args.relay, args.port, args.fps, args.quality, args.scale,
                        args.monitor, args.capture_window, args.hw_encoder, args.instance)
    try:
        asyncio.run(proxy.run())
    except KeyboardInterrupt:
        proxy.stop()
        print("\n[stream] stopped")


if __name__ == '__main__':
    main()
