#!/usr/bin/env python3
"""
MC3 UDP↔WebSocket Proxy

Sits between the MC3 game and the multiplayer relay server.
- Receives UDP packets from the game (with a 4-byte magic header + game port)
- Forwards them to the relay as WebSocket binary messages
- Receives WebSocket binary from the relay, sends as UDP to the game's bound port

Usage:
    python proxy.py --relay ws://relay:8080 --proxy-udp 30741 --game-udp 3074
"""

import asyncio
import json
import socket
import struct
import sys
import argparse
import websockets

# Protocol header: magic "MC3P" (4 bytes) + game_udp_port (2 bytes, little-endian)
MAGIC = b'MC3P'
HEADER_SIZE = 6  # 4 magic + 2 port


class Proxy:
    def __init__(self, relay_url, proxy_udp_port, game_udp_port):
        self.relay_url = relay_url
        self.proxy_udp_port = proxy_udp_port
        self.game_udp_port = game_udp_port
        self.ws = None
        self.udp_sock = None
        self.game_port = None  # learned from the game's outgoing packets
        self.running = False

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

    async def udp_reader(self):
        """Read UDP packets from the game and forward to relay."""
        print(f"[proxy] UDP listener on 0.0.0.0:{self.proxy_udp_port}")
        while self.running:
            try:
                data, addr = await asyncio.get_event_loop().sock_recvfrom(self.udp_sock, 65535)
            except OSError:
                break

            # Check if this is a control/header packet from the game
            port, payload = self.parse_header(data)
            if port is not None:
                # The game is telling us its port
                self.game_port = port
                print(f"[proxy] learned game port: {port}")
                continue

            # It's a game packet - forward to relay
            if self.ws and self.ws.open:
                await self.ws.send(payload)
                print(f"[proxy] → relay: {len(payload)} bytes from game")

    async def ws_handler(self):
        """WebSocket connection to relay - receive packets, send to game."""
        print(f"[proxy] connecting to relay: {self.relay_url}")
        async with websockets.connect(self.relay_url) as ws:
            self.ws = ws
            # Identify as a game proxy
            await ws.send(json.dumps({
                'type': 'join',
                'name': f'game-{socket.gethostname()}',
            }))

            async for message in ws:
                if isinstance(message, bytes):
                    # Binary packet from relay - forward to game
                    port, payload = self.parse_header(message)
                    target_port = port if port else self.game_port
                    if target_port and self.udp_sock:
                        try:
                            self.udp_sock.sendto(payload, ('127.0.0.1', target_port))
                            print(f"[proxy] ← relay: {len(payload)} bytes → game port {target_port}")
                        except OSError as e:
                            print(f"[proxy] UDP send error: {e}")
                    else:
                        print(f"[proxy] no game port known, dropping {len(message)} bytes")
                else:
                    # Text message from relay
                    try:
                        msg = json.loads(message)
                    except json.JSONDecodeError:
                        continue
                    if msg.get('type') == 'joined':
                        print(f"[proxy] joined relay as {msg.get('role', '?')}")
                    elif msg.get('type') == 'gameList':
                        pass  # ignore for now
                    elif msg.get('type') == 'error':
                        print(f"[proxy] relay error: {msg.get('error')}")

    async def run(self):
        self.running = True

        # Create UDP socket
        self.udp_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.udp_sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.udp_sock.setblocking(False)
        self.udp_sock.bind(('0.0.0.0', self.proxy_udp_port))

        # Run UDP reader and WebSocket handler concurrently
        await asyncio.gather(
            self.udp_reader(),
            self.ws_handler(),
        )

    def stop(self):
        self.running = False
        if self.udp_sock:
            self.udp_sock.close()


def main():
    parser = argparse.ArgumentParser(description='MC3 UDP↔WebSocket Proxy')
    parser.add_argument('--relay', default='ws://localhost:8080', help='Relay WebSocket URL')
    parser.add_argument('--proxy-udp', type=int, default=30741, help='UDP port to receive from game')
    parser.add_argument('--game-udp', type=int, default=3074, help='Game UDP port to send back to')
    args = parser.parse_args()

    proxy = Proxy(args.relay, args.proxy_udp, args.game_udp)
    try:
        asyncio.run(proxy.run())
    except KeyboardInterrupt:
        proxy.stop()
        print("\n[proxy] stopped")


if __name__ == '__main__':
    main()
