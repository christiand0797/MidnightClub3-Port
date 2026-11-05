#!/usr/bin/env python3
"""
MC3 UDP <-> WebSocket relay proxy (one per running game).

The native game (runtime/xdk/xnet.cpp) sends every System Link broadcast, plus every datagram for a relay
player, to this proxy on 127.0.0.1. The proxy carries them over the relay to the other players in the same
relay room and hands their datagrams back to the game. Each remote player gets a peer number (1-254); the game
sees that player at the virtual address 127.0.1.<peer>.

Datagram header, both directions on the local UDP hop ("MC3R", little-endian):
    magic[4] | src port u16 | dst port u16 | peer u32 (0 = everyone in the room)
The older 8-byte "MC3P" header (magic | src | dst) from the game is still accepted and treated as peer 0.

Usage:
    py -3.12 proxy.py --relay ws://relay:8080 [--instance N] [--room NAME | --join GAME_ID]
Instance N listens on 30741 + N (xnet.cpp kProxyPort + instance). With no --room the game joins the relay's Main
Lobby, where everyone online is (no player cap; the game's own System Link matches show up in Join Game). Any
other --room name is a room of its own, made on first use and not listed, so friends sharing the name play
privately. The proxy reconnects and rejoins the same room if the relay connection drops.
"""

import argparse
import asyncio
import base64
import json
import socket
import struct
import time

import websockets

import mcnet

MAGIC = b'MC3P'           # legacy 8-byte header, game -> proxy only
RELAY_MAGIC = b'MC3R'     # 12-byte header, both directions
HEADER_SIZE = 8
RELAY_HEADER_SIZE = 12
BASE_PORT = 30741
MAX_PEER = 254


class Proxy:
    def __init__(self, relay_url, proxy_udp_port, game_udp_port, room=None, join_id=None, name=None,
                 verbose=False, game_ip='127.0.0.1', record=None):
        self.relay_url = relay_url
        self.proxy_udp_port = proxy_udp_port
        self.game_udp_port = game_udp_port  # fallback target port when a datagram names none
        self.room = room
        self.join_id = join_id
        self.verbose = verbose
        self.event_window, self.event_budget = 0.0, 20
        self.record = open(record, 'a', encoding='ascii') if record else None  # --record: every datagram, hex
        self.ws = None
        self.udp_sock = None
        self.game_port = None   # the game's socket port, learned from its datagrams
        self.game_ip = game_ip  # instance N binds 127.0.0.N; updated from the address the game sends from
        self.client_name = name
        self.running = False
        self.peers = {}          # relay client id -> peer number
        self.peer_names = {}     # peer number -> relay client id
        # Test/observability hooks
        self.incoming = asyncio.Queue()  # envelopes received from the relay
        self.auth_ok = asyncio.Event()
        self.ws_connected = asyncio.Event()
        self.game_id = None
        self.game_id_event = asyncio.Event()

    def log(self, text):
        if self.verbose:
            print(text, flush=True)

    # ---- framing ----
    def make_header(self, src_port, dst_port=0):
        return MAGIC + struct.pack('<HH', src_port, dst_port)

    def make_relay_header(self, src_port, dst_port, peer):
        return RELAY_MAGIC + struct.pack('<HHI', src_port & 0xFFFF, dst_port & 0xFFFF, peer)

    def parse_header(self, data):
        """Return (src_port, dst_port, payload); src_port is None when there is no 8-byte MC3P header."""
        if len(data) < HEADER_SIZE or data[:4] != MAGIC:
            return None, None, data
        src_port, dst_port = struct.unpack('<HH', data[4:8])
        return src_port, dst_port, data[HEADER_SIZE:]

    def parse_game_datagram(self, data):
        """Return (src_port, dst_port, peer, payload) for MC3R or MC3P; None for anything else."""
        if len(data) >= RELAY_HEADER_SIZE and data[:4] == RELAY_MAGIC:
            src_port, dst_port, peer = struct.unpack('<HHI', data[4:12])
            return src_port, dst_port, peer, data[RELAY_HEADER_SIZE:]
        src_port, dst_port, payload = self.parse_header(data)
        if src_port is None:
            return None
        return src_port, dst_port, 0, payload

    def peer_for(self, name):
        """Stable peer number for a relay client id (None when all 254 are taken)."""
        if name in self.peers:
            return self.peers[name]
        for peer in range(1, MAX_PEER + 1):
            if peer not in self.peer_names:
                self.peers[name] = peer
                self.peer_names[peer] = name
                self.log(f"[proxy] {name} is peer {peer} (127.0.1.{peer})")
                return peer
        return None

    # ---- game -> relay ----
    async def udp_reader(self):
        print(f"[proxy] UDP listener on 127.0.0.1:{self.proxy_udp_port}", flush=True)
        loop = asyncio.get_running_loop()
        while self.running:
            try:
                data, addr = await loop.sock_recvfrom(self.udp_sock, 65535)
            except OSError:
                break
            if addr[1] == self.proxy_udp_port:
                continue  # our own delivery bounced back; forwarding it would loop through the relay
            parsed = self.parse_game_datagram(data)
            if parsed is None:
                continue  # not from the game's tunnel; never echoed (no feedback loop)
            src_port, dst_port, peer, payload = parsed
            if self.game_port != src_port:
                print(f"[proxy] game socket {addr[0]}:{src_port}", flush=True)
            self.game_port, self.game_ip = src_port, addr[0]
            if not payload or not self.ws:
                continue
            self.note('out', peer, src_port, dst_port, payload)
            self.report_events('out', payload)
            envelope = {'type': 'input', 'data': json.dumps({
                'payload': base64.b64encode(payload).decode('ascii'),
                'src_port': src_port, 'dst_port': dst_port, 't': time.time() * 1000})}
            if peer:
                target = self.peer_names.get(peer)
                if target is None:
                    continue  # that player left
                envelope['to'] = target
            try:
                await self.ws.send(json.dumps(envelope))
                self.log(f"[proxy] -> relay {envelope.get('to', 'room')}: {len(payload)} bytes "
                         f"{src_port}->{dst_port}")
            except Exception as e:
                print(f"[proxy] relay send error: {e}", flush=True)

    def report_events(self, direction, payload):
        """The game's race setup/state/checkpoint/score messages, to the relay (rankings, see mcnet.py)."""
        if not self.ws or not self.game_id:
            return
        now = time.time()
        if now - self.event_window > 1:
            self.event_window, self.event_budget = now, 20
        for event in mcnet.race_events(payload):
            if self.event_budget <= 0:
                return
            self.event_budget -= 1
            event['dir'] = direction
            asyncio.ensure_future(self.ws.send(json.dumps({'type': 'race_event', 'event': event})))

    def note(self, direction, peer, src_port, dst_port, payload):
        if self.record:
            self.record.write(json.dumps({'t': round(time.time(), 3), 'dir': direction, 'peer': peer,
                                          'src': src_port, 'dst': dst_port, 'hex': payload.hex()}) + chr(10))
            self.record.flush()

    # ---- relay -> game ----
    def deliver(self, name, payload, src_port, dst_port):
        peer = self.peer_for(name or '?')
        target_port = dst_port or self.game_port or self.game_udp_port
        if not peer or not target_port or target_port == self.proxy_udp_port or not self.udp_sock:
            return False
        packet = self.make_relay_header(src_port or 0, target_port, peer) + payload
        try:
            self.udp_sock.sendto(packet, (self.game_ip, target_port))
        except OSError as e:
            print(f"[proxy] UDP send error: {e}", flush=True)
            return False
        self.log(f"[proxy] <- relay {name} (peer {peer}): {len(payload)} bytes -> {self.game_ip}:{target_port}")
        return True

    async def handle_relay_input(self, msg):
        src = msg.get('from')
        peer_id = msg.get('fromId') or src  # route by the relay-assigned id; names are self-chosen
        try:
            inner = json.loads(msg.get('data', '{}'))
            payload = base64.b64decode(inner.get('payload', ''), validate=True)
            src_port = int(inner.get('src_port') or 0)
            dst_port = int(inner.get('dst_port') or 0)
            send_ts = inner.get('t')
        except Exception:
            return
        if not payload or not (0 <= src_port <= 0xFFFF and 0 <= dst_port <= 0xFFFF):
            return
        self.incoming.put_nowait({'from': src, 'payload': payload, 'src_port': src_port, 'dst_port': dst_port,
                                  'send_ts': send_ts, 'recv_ts': time.time() * 1000})
        self.note('in', self.peers.get(peer_id, 0), src_port, dst_port, payload)
        self.report_events('in', payload)
        self.deliver(peer_id, payload, src_port, dst_port)

    async def enter_room(self, ws):
        if self.join_id:
            await ws.send(json.dumps({'type': 'join_game', 'gameId': self.join_id}))
        else:  # no name = the Main Lobby
            await ws.send(json.dumps({'type': 'enter_room', 'name': self.room or ''}))

    async def ws_handler(self):
        print(f"[proxy] connecting to relay: {self.relay_url}", flush=True)
        async with websockets.connect(self.relay_url) as ws:
            self.ws = ws
            if not self.client_name:
                self.client_name = f'game-{socket.gethostname()}-{self.proxy_udp_port}'
            # Guest attach: a proxy has no account; the relay grants room transport only, no saves/cloud.
            await ws.send(json.dumps({'type': 'join', 'clientType': 'game', 'name': self.client_name}))
            self.ws_connected.set()
            try:
                async for message in ws:
                    if isinstance(message, bytes):
                        try:
                            message = message.decode('utf-8')
                        except UnicodeDecodeError:
                            continue
                    try:
                        msg = json.loads(message)
                    except json.JSONDecodeError:
                        continue
                    kind = msg.get('type')
                    if kind == 'auth_ok':
                        self.auth_ok.set()
                        print(f"[proxy] attached to relay as {self.client_name}", flush=True)
                        await self.enter_room(ws)
                    elif kind in ('game_created', 'game_joined'):
                        game = msg.get('game', {})
                        self.game_id = game.get('id')
                        self.room_name = game.get('name')
                        self.game_id_event.set()
                        count = game.get('playerCount')
                        where = 'Main Lobby' if game.get('main') else f"room \"{self.room_name}\""
                        print(f"[proxy] in {where} ({self.game_id}), {count} player(s) online here", flush=True)
                    elif kind == 'player_joined':
                        print(f"[proxy] {msg.get('username')} joined ({msg.get('playerCount')} here)", flush=True)
                    elif kind == 'input':
                        await self.handle_relay_input(msg)
                    elif kind == 'player_left':
                        peer = self.peers.pop(msg.get('id'), None)
                        if peer:
                            self.peer_names.pop(peer, None)
                    elif kind == 'error':
                        print(f"[proxy] relay error: {msg.get('error')}", flush=True)
            finally:
                print(f"[proxy] {self.client_name} websocket connection ended", flush=True)
                self.ws = None
                self.game_id = None
                self.peers.clear()
                self.peer_names.clear()
                self.auth_ok.clear()
                self.game_id_event.clear()

    async def relay_loop(self):
        # keep the game online: reconnect with backoff and rejoin the same room
        delay = 1
        while self.running:
            try:
                await self.ws_handler()
                delay = 1
            except (OSError, websockets.exceptions.WebSocketException) as e:
                print(f"[proxy] relay unreachable ({e.__class__.__name__}); retrying in {delay}s", flush=True)
            if not self.running:
                break
            await asyncio.sleep(delay)
            delay = min(delay * 2, 30)

    async def run(self):
        self.running = True
        self.udp_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.udp_sock.setblocking(False)
        # Loopback only: nothing on the LAN may inject datagrams into the game's tunnel.
        self.udp_sock.bind(('127.0.0.1', self.proxy_udp_port))
        await asyncio.gather(self.udp_reader(), self.relay_loop())

    def stop(self):
        self.running = False
        if self.udp_sock:
            self.udp_sock.close()


def main():
    parser = argparse.ArgumentParser(description='MC3 UDP <-> WebSocket relay proxy')
    parser.add_argument('--relay', default='ws://localhost:8080', help='relay WebSocket URL')
    parser.add_argument('--instance', type=int, default=0, help='game instance (mc3.exe --instance N)')
    parser.add_argument('--proxy-udp', type=int, help='UDP port to receive from the game (default 30741 + instance)')
    parser.add_argument('--game-udp', type=int, default=3074, help='fallback game UDP port')
    room = parser.add_mutually_exclusive_group()
    room.add_argument('--room', default='', help='room name; empty = the Main Lobby (everyone online)')
    room.add_argument('--join', help='join this relay game id')
    parser.add_argument('--name', help='name shown to the other players')
    parser.add_argument('--verbose', action='store_true', help='log every datagram')
    parser.add_argument('--record', help='append every datagram (hex, JSON lines) to this file, for protocol work')
    args = parser.parse_args()
    port = args.proxy_udp or BASE_PORT + args.instance
    proxy = Proxy(args.relay, port, args.game_udp, room=None if args.join else args.room, join_id=args.join,
                  name=args.name, verbose=args.verbose, record=args.record,
                  game_ip=f'127.0.0.{args.instance}' if args.instance else '127.0.0.1')
    try:
        asyncio.run(proxy.run())
    except KeyboardInterrupt:
        proxy.stop()
        print("\n[proxy] stopped")


if __name__ == '__main__':
    main()
