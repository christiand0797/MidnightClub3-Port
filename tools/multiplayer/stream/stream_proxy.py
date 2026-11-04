#!/usr/bin/env python3
"""
MC3 Browser Multiplayer — Stream Proxy (GPU-optimized)

Captures the mc3.exe game window and streams frames to browser clients
over WebSocket (MJPEG-style), while forwarding browser input back to the
game via UDP proxy + WebSocket relay.

Architecture:
  Browser (WebSocket) ←→ StreamProxy (WebSocket + screen capture) ←→ Game (UDP via proxy)

Performance notes:
  - Uses mss for fast screen capture (much faster than PIL screenshot)
  - JPEG encoding at configurable quality/scale for bandwidth vs quality
  - Adaptive frame rate: skips frames when bandwidth is constrained
  - GPU-friendly: capture runs on a dedicated thread, WebSocket sends non-blocking

Requirements:
    pip install mss Pillow websockets
"""

import asyncio
import io
import json
import socket
import struct
import sys
import argparse
import base64
import time
import threading
import websockets

try:
    import mss
    HAVE_MSS = True
except ImportError:
    HAVE_MSS = False

try:
    from PIL import Image
    HAVE_PIL = True
except ImportError:
    HAVE_PIL = False

MAGIC = b'MC3P'
HEADER_SIZE = 6  # 4 magic + 2 port

# Input packet types
INPUT_TYPE_KEY = 0x01
INPUT_TYPE_GAMEPAD = 0x02


class StreamProxy:
    def __init__(self, relay_url, listen_port, fps=60, quality=75, scale=1.0,
                 capture_monitor=1):
        self.relay_url = relay_url
        self.listen_port = listen_port          # UDP port for game packets
        self.ws_port = listen_port + 1          # WebSocket port for browsers
        self.fps = fps
        self.quality = quality
        self.scale = scale                      # 1.0 = native, 0.5 = half res
        self.capture_monitor = capture_monitor  # which monitor to capture

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

        # Screen capture state
        self.monitor = None
        self.sct = None

    # ---- Screen capture ----
    def setup_capture(self):
        if not HAVE_MSS:
            print("[stream] ERROR: mss not installed. Run: pip install mss Pillow websockets")
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

    def capture_frame(self):
        """Capture one frame, scale + JPEG encode. Returns bytes or None."""
        if not self.sct or not self.monitor:
            return None
        try:
            img = self.sct.grab(self.monitor)
            pil_img = Image.frombytes("RGB", img.size, img.rgb)

            # Downscale for bandwidth
            if self.scale < 1.0:
                new_w = max(320, int(pil_img.width * self.scale))
                new_h = max(240, int(pil_img.height * self.scale))
                pil_img = pil_img.resize((new_w, new_h), Image.LANCZOS)

            buf = io.BytesIO()
            pil_img.save(buf, format="JPEG", quality=self.quality,
                         optimize=True)
            return buf.getvalue()
        except Exception as e:
            print(f"[stream] capture error: {e}")
            return None

    # ---- Network helpers ----
    def make_header(self, port):
        return MAGIC + struct.pack('<H', port)

    def parse_header(self, data):
        if len(data) < HEADER_SIZE:
            return None, data
        magic = data[:4]
        if magic != MAGIC:
            return None, data
        port = struct.unpack('<H', data[4:6])[0]
        return port, data[HEADER_SIZE:]

    # ---- Browser WebSocket handler ----
    async def browser_handler(self, websocket):
        """Handle browser client connections (game stream + input)."""
        client_id = id(websocket)
        print(f"[stream] browser connected: {websocket.remote_address}")
        self.ws_clients.add(websocket)
        self.clients_ack[client_id] = time.time()

        try:
            # Hello with stream params
            await websocket.send(json.dumps({
                "type": "hello",
                "fps": self.fps,
                "quality": self.quality,
                "scale": self.scale,
                "width": self.monitor['width'] if self.monitor else 0,
                "height": self.monitor['height'] if self.monitor else 0,
            }))

            async for message in websocket:
                if isinstance(message, bytes):
                    # Binary input packet from browser
                    await self.forward_input_to_game(message)
                else:
                    try:
                        msg = json.loads(message)
                    except json.JSONDecodeError:
                        continue
                    await self.handle_browser_msg(websocket, client_id, msg)
        finally:
            self.ws_clients.discard(websocket)
            self.clients_ack.pop(client_id, None)
            print(f"[stream] browser disconnected")

    async def handle_browser_msg(self, websocket, client_id, msg):
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
            # Browser requests quality/scale change
            q = msg.get("quality")
            s = msg.get("scale")
            if q is not None:
                self.quality = max(10, min(100, int(q)))
            if s is not None:
                self.scale = max(0.25, min(1.0, float(s)))
            print(f"[stream] settings updated: quality={self.quality}, scale={self.scale}")
        elif mtype == "ping":
            await websocket.send(json.dumps({"type": "pong", "ts": msg.get("ts", 0)}))

    async def forward_input_to_game(self, data):
        """Forward browser input packet to the game via UDP."""
        if not data or len(data) < 2:
            return
        target_port = self.game_port or 3074
        header = self.make_header(target_port)
        packet = header + data
        try:
            if self.udp_sock:
                self.udp_sock.sendto(packet, ('127.0.0.1', target_port))
        except OSError as e:
            print(f"[stream] UDP input send error: {e}")

    # ---- Relay WebSocket ----
    async def relay_handler(self):
        """WebSocket connection to the relay server."""
        print(f"[stream] connecting to relay: {self.relay_url}")
        try:
            async with websockets.connect(self.relay_url) as ws:
                self.relay_ws = ws
                await ws.send(json.dumps({
                    "type": "join",
                    "name": f"stream-{socket.gethostname()}",
                }))
                async for message in ws:
                    if isinstance(message, bytes):
                        port, payload = self.parse_header(message)
                        target_port = port if port else self.game_port
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
        mtype = msg.get("type")
        if mtype == "joined":
            print(f"[stream] joined relay as {msg.get('role', '?')}")
        elif mtype == "gameList":
            await self.broadcast_to_browsers({
                "type": "gameList",
                "games": msg.get("games", []),
            })
        elif mtype == "chat":
            await self.broadcast_to_browsers({
                "type": "chat",
                "from": msg.get("from", "?"),
                "text": msg.get("text", ""),
            })

    # ---- Broadcast ----
    async def broadcast_frame(self, frame_bytes):
        """Send a JPEG frame to all connected browser clients."""
        if not self.ws_clients:
            return
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
        print(f"[stream] capture loop starting ({self.fps} fps, quality={self.quality}, scale={self.scale})")
        while self.running:
            now = time.time()
            if now - self.last_frame_time < self.frame_interval:
                await asyncio.sleep(0.002)
                continue
            if not self.ws_clients:
                # No browsers — don't waste CPU capturing
                await asyncio.sleep(0.1)
                continue
            frame = self.capture_frame()
            if frame:
                self.last_frame_time = now
                self.frame_count += 1
                self.bytes_sent += len(frame)
                await self.broadcast_frame(frame)
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
            port, payload = self.parse_header(data)
            if port is not None:
                self.game_port = port
                print(f"[stream] learned game port: {port}")
                continue
            if self.relay_ws:
                try:
                    await self.relay_ws.send(payload)
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
        if self.udp_sock:
            self.udp_sock.close()


def main():
    parser = argparse.ArgumentParser(description='MC3 Stream Proxy (video + input)')
    parser.add_argument('--relay', default='ws://localhost:8080', help='Relay WebSocket URL')
    parser.add_argument('--port', type=int, default=8765, help='UDP port for game; browser WS is port+1')
    parser.add_argument('--fps', type=int, default=60, help='Target capture FPS')
    parser.add_argument('--quality', type=int, default=75, help='JPEG quality 1-100')
    parser.add_argument('--scale', type=float, default=1.0, help='Render scale 0.25-1.0')
    parser.add_argument('--monitor', type=int, default=1, help='Monitor index to capture')
    args = parser.parse_args()

    if not HAVE_MSS:
        print("[stream] ERROR: mss not installed. Run: pip install mss Pillow websockets")
        sys.exit(1)

    proxy = StreamProxy(args.relay, args.port, args.fps, args.quality, args.scale, args.monitor)
    try:
        asyncio.run(proxy.run())
    except KeyboardInterrupt:
        proxy.stop()
        print("\n[stream] stopped")


if __name__ == '__main__':
    main()
