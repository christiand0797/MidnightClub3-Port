#!/usr/bin/env python3
"""
Two-peer integration test for the MC3 multiplayer relay + UDP<->WS game proxies.

Starts the relay server (node subprocess) on a test port, runs two in-process
game proxies (different UDP ports) attached as 'game' clients, and verifies:

  1. Relay parses binary WebSocket messages (binary ping frame -> pong) as well
     as text messages (bug #6 from network_integration_review_20261104.md).
  2. Guest 'join' handshake authenticates proxies as 'game' clients (bug #7;
     adapters have no user account/token, so the relay accepts a guest 'join').
  3. A datagram sent from proxy A reaches proxy B through the relay with the
     correct source port identity (the MC3P header port travels with the payload;
     header+payload are no longer split by a `continue`, bug #5).
  4. Reconnect: after the relay restarts, proxies re-authenticate, re-create /
     re-join a game, and delivery works again.
  5. Latency: round-trip probe through the relay (min / avg / max ms).
  6. Malformed frames: wrong magic, truncated header, oversized payload, and
     non-MC3P binary frames are all rejected/dropped without crashing the relay
     or the proxies (bug #5/#6 robustness).

Usage:
    python tools/multiplayer/tests/test_two_peer.py

Requires: node, and python3 with the `websockets` package.
Does NOT build or launch mc3.exe.
"""

import asyncio
import base64
import json
import os
import socket
import struct
import subprocess
import sys
import tempfile
import time
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[3]
RELAY_DIR = REPO_ROOT / "tools" / "multiplayer" / "relay"
sys.path.insert(0, str(REPO_ROOT / "tools" / "multiplayer" / "proxy"))

import websockets  # noqa: E402
from proxy import Proxy, MAGIC  # noqa: E402

RELAY_PORT = 8192
RELAY_URL = f"ws://127.0.0.1:{RELAY_PORT}/ws"

# Phase 1 ports
UDP_A, GAME_A = 30741, 30741
UDP_B, GAME_B = 30742, 30742
# Phase 2 (reconnect) ports - fresh to avoid Windows bind/TIME_WAIT issues
UDP_A2, GAME_A2 = 30743, 30743
UDP_B2, GAME_B2 = 30744, 30744

PAYLOAD = b"PEER-A-TO-PEER-B"
SRC_PORT_A = 30751  # source port identity carried in the MC3P header
SRC_PORT_B = 30752

NAME_A = "game-A"
NAME_B = "game-B"
NAME_A2 = "game-A2"
NAME_B2 = "game-B2"


def make_datagram(src_port, dst_port, payload):
    return MAGIC + struct.pack("<H", src_port) + struct.pack("<H", dst_port) + payload


def now_ms():
    return time.time() * 1000.0


async def wait_for_port(port, host="127.0.0.1", timeout=15.0):
    deadline = asyncio.get_event_loop().time() + timeout
    while True:
        try:
            reader, writer = await asyncio.wait_for(
                asyncio.open_connection(host, port), 0.5
            )
            writer.close()
            try:
                await writer.wait_closed()
            except Exception:
                pass
            return True
        except OSError:
            if asyncio.get_event_loop().time() >= deadline:
                return False
            await asyncio.sleep(0.25)


async def wait_for_ws_message(ws, expected_type, timeout=5.0):
    deadline = asyncio.get_event_loop().time() + timeout
    while True:
        remaining = deadline - asyncio.get_event_loop().time()
        if remaining <= 0:
            raise TimeoutError(f"timed out waiting for ws message {expected_type!r}")
        try:
            message = await asyncio.wait_for(ws.recv(), remaining)
        except websockets.exceptions.ConnectionClosed:
            raise TimeoutError(f"ws closed while waiting for {expected_type!r}")
        if isinstance(message, (bytes, bytearray)):
            message = bytes(message).decode("utf-8")
        try:
            msg = json.loads(message)
        except json.JSONDecodeError:
            continue
        if isinstance(msg, dict) and msg.get("type") == expected_type:
            return msg


async def start_proxy(relay_url, udp_port, game_udp_port, name=None):
    p = Proxy(relay_url, udp_port, game_udp_port)
    if name:
        p.client_name = name
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.setblocking(False)
    sock.bind(("0.0.0.0", udp_port))
    p.udp_sock = sock
    p.running = True
    task = asyncio.ensure_future(asyncio.gather(p.udp_reader(), p.ws_handler()))
    return p, task, sock


async def wait_ready(proxy, timeout=10.0):
    await asyncio.wait_for(proxy.ws_connected.wait(), timeout=timeout)
    await asyncio.wait_for(proxy.auth_ok.wait(), timeout=timeout)


async def recv_from(mailbox, sender, timeout=5.0):
    """Dequeue until a message from `sender` arrives."""
    deadline = asyncio.get_event_loop().time() + timeout
    while True:
        remaining = deadline - asyncio.get_event_loop().time()
        if remaining <= 0:
            raise TimeoutError(f"timed out waiting for message from {sender!r}")
        msg = await asyncio.wait_for(mailbox.get(), remaining)
        if msg.get("from") == sender:
            return msg


def drain(mailbox):
    while True:
        try:
            mailbox.get_nowait()
        except asyncio.QueueEmpty:
            return


async def main():
    results = []

    def record(name, ok, detail=""):
        results.append((name, bool(ok), detail))
        print(("PASS" if ok else "FAIL") + f": {name}" + (f" — {detail}" if detail else ""))

    relay_proc = None
    proxies = []  # list of (proxy, task, sock)

    async def stop_proxies():
        for p, task, sock in proxies:
            p.running = False
            task.cancel()
            try:
                await task
            except asyncio.CancelledError:
                pass
            except Exception:
                pass
            try:
                sock.close()
            except OSError:
                pass
        proxies.clear()

    def stop_relay():
        nonlocal relay_proc
        if relay_proc is not None and relay_proc.poll() is None:
            relay_proc.terminate()
            try:
                relay_proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                relay_proc.kill()
                relay_proc.wait()
        relay_proc = None

    relay_log = ""

    def start_relay(port, data_dir):
        env = dict(os.environ)
        env["RELAY_PORT"] = str(port)
        env["DATA_DIR"] = str(data_dir)
        return subprocess.Popen(
            ["node", "server.js"],
            cwd=str(RELAY_DIR),
            env=env,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )

    exit_code = 0
    try:
        # ------------------------------------------------------------------
        # Phase 0: relay + WebSocket framing (binary + text ping/pong)
        # ------------------------------------------------------------------
        data_dir1 = tempfile.mkdtemp(prefix="mc3-relay-test-")
        relay_proc = start_relay(RELAY_PORT, data_dir1)
        if not await wait_for_port(RELAY_PORT):
            if relay_proc:
                try:
                    relay_log = relay_proc.stdout.read()
                except Exception:
                    relay_log = ""
            stop_relay()
            record("relay starts and listens", False, f"port {RELAY_PORT}: {relay_log[-500:]}")
            return 1
        record("relay starts and listens", True, f"port {RELAY_PORT}")

        async with websockets.connect(RELAY_URL) as ws:
            ts = int(now_ms())
            # Binary frame carrying UTF-8 JSON: exercises the binary path.
            await ws.send(json.dumps({"type": "ping", "ts": ts}).encode("utf-8"))
            pong = await wait_for_ws_message(ws, "pong")
            record(
                "binary websocket message parsed (decode UTF-8, JSON.parse)",
                pong.get("ts") == ts,
                f"echo ts={pong.get('ts')}, sent {ts}",
            )
            # Text frame: text path still works.
            ts2 = ts + 1
            await ws.send(json.dumps({"type": "ping", "ts": ts2}))
            pong2 = await wait_for_ws_message(ws, "pong")
            record(
                "text websocket message parsed",
                pong2.get("ts") == ts2,
                f"echo ts={pong2.get('ts')}, sent {ts2}",
            )

        # ------------------------------------------------------------------
        # Phase 1: two game proxies, guest auth, create + join game
        # ------------------------------------------------------------------
        (pA, tA, sA), (pB, tB, sB) = await asyncio.gather(
            start_proxy(RELAY_URL, UDP_A, GAME_A, NAME_A),
            start_proxy(RELAY_URL, UDP_B, GAME_B, NAME_B),
        )
        proxies.extend([(pA, tA, sA), (pB, tB, sB)])

        await asyncio.gather(wait_ready(pA), wait_ready(pB))
        record(
            "proxies guest-authenticate as 'game' clients",
            pA.auth_ok.is_set() and pB.auth_ok.is_set() and pA.client_name != pB.client_name,
            f"{pA.client_name}, {pB.client_name}",
        )

        await pA.ws.send(
            json.dumps({"type": "create_game", "name": "TwoPeer Test", "maxPlayers": 2})
        )
        await asyncio.wait_for(pA.game_id_event.wait(), timeout=10)
        game_id = pA.game_id
        await pB.ws.send(json.dumps({"type": "join_game", "gameId": game_id}))
        await asyncio.wait_for(pB.game_id_event.wait(), timeout=10)
        record(
            "game created by A and joined by B (two independent game peers)",
            pA.game_id is not None and pA.game_id == pB.game_id,
            f"gameId={pA.game_id}",
        )

        # ------------------------------------------------------------------
        # Phase 2: datagram A -> relay -> B with source port identity
        # ------------------------------------------------------------------
        drain(pA.incoming)
        drain(pB.incoming)
        inject = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            # Simulate game A sending to game B: MC3P header carries src_port and
            # dst_port. dst_port = GAME_B (proxy B's learned game port).
            inject.sendto(
                make_datagram(SRC_PORT_A, GAME_B, PAYLOAD), ("127.0.0.1", UDP_A)
            )
        finally:
            inject.close()

        msg_b = await recv_from(pB.incoming, NAME_A)
        delivery_ok = (
            msg_b.get("payload") == PAYLOAD
            and msg_b.get("src_port") == SRC_PORT_A
            and msg_b.get("dst_port") == GAME_B
            and msg_b.get("from") == NAME_A
        )
        record(
            "datagram A -> relay -> B with correct source port identity",
            delivery_ok,
            f"received payload={msg_b.get('payload')!r} src_port={msg_b.get('src_port')} "
            f"dst_port={msg_b.get('dst_port')} from={msg_b.get('from')} "
            f"(expect payload={PAYLOAD!r} src_port={SRC_PORT_A} dst_port={GAME_B} from={NAME_A})",
        )

        # ------------------------------------------------------------------
        # Phase 3: latency round-trip through the relay
        # ------------------------------------------------------------------
        N = 10
        drain(pA.incoming)
        drain(pB.incoming)
        rtts = []
        one_ways = []
        for i in range(N):
            t1 = now_ms()
            probe = f"LATENCY-{i}".encode()
            inner = json.dumps(
                {
                    "payload": base64.b64encode(probe).decode("ascii"),
                    "src_port": SRC_PORT_A,
                    "dst_port": GAME_B,
                    "t": t1,
                }
            )
            await pA.ws.send(json.dumps({"type": "input", "data": inner}))
            mb = await recv_from(pB.incoming, NAME_A)
            t2 = mb["recv_ts"]
            if mb.get("payload") != probe or mb.get("src_port") != SRC_PORT_A:
                raise AssertionError(
                    f"latency probe mismatch at i={i}: {mb.get('payload')!r} src_port={mb.get('src_port')}"
                )
            # B echoes immediately back through the relay.
            t2m = now_ms()
            echo = f"ECHO-{i}".encode()
            inner2 = json.dumps(
                {
                    "payload": base64.b64encode(echo).decode("ascii"),
                    "src_port": SRC_PORT_B,
                    "dst_port": GAME_A,
                    "t": t2m,
                }
            )
            await pB.ws.send(json.dumps({"type": "input", "data": inner2}))
            ma = await recv_from(pA.incoming, NAME_B)
            t3 = now_ms()
            if ma.get("payload") != echo or ma.get("src_port") != SRC_PORT_B:
                raise AssertionError(
                    f"latency echo mismatch at i={i}: {ma.get('payload')!r} src_port={ma.get('src_port')}"
                )
            rtts.append(t3 - t1)
            one_ways.append(t2 - t1)

        def stats(values):
            return (min(values), sum(values) / len(values), max(values))

        rtt_min, rtt_avg, rtt_max = stats(rtts)
        ow_min, ow_avg, ow_max = stats(one_ways)
        record(
            "latency round-trip through relay (ms)",
            True,
            f"n={N} rtt min={rtt_min:.1f} avg={rtt_avg:.1f} max={rtt_max:.1f} | "
            f"one-way min={ow_min:.1f} avg={ow_avg:.1f} max={ow_max:.1f}",
        )

        # ------------------------------------------------------------------
        # Phase 3.5: malformed frames and edge cases (bug #5/#6 robustness)
        # ------------------------------------------------------------------
        # Restart fresh proxies so both have live WS connections for the
        # forwarding checks (latency test left prior WS connections closed).
        await stop_proxies()
        (pA, tA, sA), (pB, tB, sB) = await asyncio.gather(
            start_proxy(RELAY_URL, UDP_A, GAME_A, NAME_A),
            start_proxy(RELAY_URL, UDP_B, GAME_B, NAME_B),
        )
        proxies.extend([(pA, tA, sA), (pB, tB, sB)])
        await asyncio.gather(wait_ready(pA), wait_ready(pB))

        # Both proxies must be in a game for the relay to forward input datagrams.
        await pA.ws.send(json.dumps({"type": "create_game", "name": "Malformed Test", "maxPlayers": 2}))
        await asyncio.wait_for(pA.game_id_event.wait(), timeout=10)
        game_id = pA.game_id
        await pB.ws.send(json.dumps({"type": "join_game", "gameId": game_id}))
        await asyncio.wait_for(pB.game_id_event.wait(), timeout=10)

        drain(pA.incoming)
        drain(pB.incoming)

        def inject_raw_udp(port, raw):
            s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            try:
                s.sendto(raw, ("127.0.0.1", port))
            finally:
                s.close()

        malformed_ok = True
        details = []

        # Case 1: wrong magic — proxy should not forward to relay (no MC3P header).
        inject_raw_udp(UDP_A, b"XXXX" + struct.pack("<H", SRC_PORT_A) + struct.pack("<H", GAME_B) + b"wrong-magic")
        await asyncio.sleep(0.25)
        try:
            _ = await asyncio.wait_for(pB.incoming.get(), timeout=0.3)
            malformed_ok = False
            details.append("wrong-magic leaked to peer")
        except asyncio.TimeoutError:
            pass  # expected: dropped

        # Case 2: truncated header (less than 8 bytes) — not forwarded.
        inject_raw_udp(UDP_A, b"MC3" + struct.pack("<H", SRC_PORT_A))
        await asyncio.sleep(0.25)
        try:
            _ = await asyncio.wait_for(pB.incoming.get(), timeout=0.3)
            malformed_ok = False
            details.append("truncated-header leaked to peer")
        except asyncio.TimeoutError:
            pass  # expected: dropped

        # Case 3: valid MC3P header + payload — forwarded to peer with identity,
        # in both directions (A→B and B→A).
        inject_raw_udp(UDP_A, MAGIC + struct.pack("<H", SRC_PORT_A) + struct.pack("<H", GAME_B) + b"case3-ab")
        try:
            mb = await asyncio.wait_for(pB.incoming.get(), timeout=1.0)
            ab_ok = mb.get("payload") == b"case3-ab" and mb.get("src_port") == SRC_PORT_A
        except asyncio.TimeoutError:
            ab_ok = False
        inject_raw_udp(UDP_B, MAGIC + struct.pack("<H", SRC_PORT_B) + struct.pack("<H", GAME_A) + b"case3-ba")
        try:
            mb = await asyncio.wait_for(pA.incoming.get(), timeout=1.0)
            ba_ok = mb.get("payload") == b"case3-ba" and mb.get("src_port") == SRC_PORT_B
        except asyncio.TimeoutError:
            ba_ok = False
        case3_ok = ab_ok and ba_ok
        details.append(f"valid MC3P header forwarded both directions: ab={ab_ok} ba={ba_ok}")
        malformed_ok = malformed_ok and case3_ok

        record(
            "malformed frames rejected/dropped without crashing relay or proxies",
            malformed_ok,
            "; ".join(details),
        )

        # ------------------------------------------------------------------
        # Phase 4: reconnect after relay restart
        # ------------------------------------------------------------------
        await stop_proxies()
        stop_relay()

        await wait_for_port(RELAY_PORT)  # ensure old listener fully released
        data_dir2 = tempfile.mkdtemp(prefix="mc3-relay-test-")
        relay_proc = start_relay(RELAY_PORT, data_dir2)
        if not await wait_for_port(RELAY_PORT):
            if relay_proc:
                try:
                    relay_log = relay_proc.stdout.read()
                except Exception:
                    relay_log = ""
            stop_relay()
            record("relay restarts and listens again", False, f"port {RELAY_PORT}: {relay_log[-500:]}")
            return 1
        record("relay restarts and listens again", True, f"port {RELAY_PORT}")

        (pA2, tA2, sA2), (pB2, tB2, sB2) = await asyncio.gather(
            start_proxy(RELAY_URL, UDP_A2, GAME_A2, NAME_A2),
            start_proxy(RELAY_URL, UDP_B2, GAME_B2, NAME_B2),
        )
        proxies.extend([(pA2, tA2, sA2), (pB2, tB2, sB2)])

        await asyncio.gather(wait_ready(pA2), wait_ready(pB2))
        record(
            "proxies re-authenticate after relay restart",
            pA2.auth_ok.is_set() and pB2.auth_ok.is_set(),
            f"{pA2.client_name}, {pB2.client_name}",
        )

        await pA2.ws.send(
            json.dumps({"type": "create_game", "name": "TwoPeer Test Reconnect", "maxPlayers": 2})
        )
        await asyncio.wait_for(pA2.game_id_event.wait(), timeout=10)
        game_id2 = pA2.game_id
        await pB2.ws.send(json.dumps({"type": "join_game", "gameId": game_id2}))
        await asyncio.wait_for(pB2.game_id_event.wait(), timeout=10)
        record(
            "game recreated and rejoined after relay restart",
            pA2.game_id is not None and pA2.game_id == pB2.game_id,
            f"gameId={pA2.game_id}",
        )

        drain(pA2.incoming)
        drain(pB2.incoming)
        inject2 = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            inject2.sendto(
                make_datagram(SRC_PORT_A, GAME_B2, PAYLOAD), ("127.0.0.1", UDP_A2)
            )
        finally:
            inject2.close()

        msg_b2 = await recv_from(pB2.incoming, NAME_A2)
        reconnect_ok = (
            msg_b2.get("payload") == PAYLOAD
            and msg_b2.get("src_port") == SRC_PORT_A
            and msg_b2.get("dst_port") == GAME_B2
            and msg_b2.get("from") == NAME_A2
        )
        record(
            "delivery works after relay reconnect (post-restart)",
            reconnect_ok,
            f"received payload={msg_b2.get('payload')!r} src_port={msg_b2.get('src_port')} "
            f"dst_port={msg_b2.get('dst_port')} from={msg_b2.get('from')} "
            f"(expect payload={PAYLOAD!r} src_port={SRC_PORT_A} dst_port={GAME_B2} from={NAME_A2})",
        )

    except Exception as exc:  # noqa: BLE001 - report any test infrastructure failure
        import traceback

        print(f"TEST ERROR: {type(exc).__name__}: {exc}")
        traceback.print_exc()
        for p, _t, _s in proxies:
            cc = getattr(p.ws, "close_code", None) if p.ws else None
            st = getattr(p.ws, "state", None) if p.ws else None
            print(f"proxy {p.client_name}: ws state={st} close_code={cc}")
        if relay_proc is not None:
            try:
                relay_log = relay_proc.stdout.read()
            except Exception:
                relay_log = ""
            print("RELAY LOG (tail):\n", relay_log[-2500:])
        exit_code = 1
    finally:
        await stop_proxies()
        stop_relay()

    # ------------------------------------------------------------------
    # Summary
    # ------------------------------------------------------------------
    print()
    print("=" * 60)
    print("Two-peer integration test summary")
    print("=" * 60)
    passed = sum(1 for _, ok, _ in results if ok)
    for name, ok, detail in results:
        print(f"  [{'PASS' if ok else 'FAIL'}] {name}" + (f" — {detail}" if detail else ""))
    print(f"Results: {passed}/{len(results)} checks passed")
    if passed == len(results) and exit_code == 0:
        print("OVERALL: PASS")
        return 0
    print("OVERALL: FAIL")
    return 1


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
