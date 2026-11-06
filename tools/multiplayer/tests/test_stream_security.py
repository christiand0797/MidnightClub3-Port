"""Real loopback WebSocket pairing with synthetic controller sink; no capture, encoder or game."""
import asyncio
import json
from pathlib import Path
import sys
import unittest
from unittest.mock import Mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'stream'))
import websockets
from stream_security import Pairing, Budget, decode_message, valid_origin, MAX_MESSAGE
from stream_proxy import StreamProxy, PAD_MAGIC, PAD_REPORT_SIZE

TOKEN = 'a' * 64
ORIGIN = 'http://127.0.0.1:8080'


class UnitTests(unittest.TestCase):
    def test_exact_origin_and_secret(self):
        for bad in ('*', 'null', 'https://host/path', 'https://u:p@host', 'http://host:0'):
            self.assertFalse(valid_origin(bad))
        p = Pairing(TOKEN, [ORIGIN], max_clients=1)
        self.assertFalse(p.admit('bad', 'http://evil.example'))
        self.assertTrue(p.admit('one', ORIGIN))
        self.assertFalse(p.admit('two', ORIGIN))
        self.assertFalse(p.authenticate('one', json.dumps({'type': 'pair', 'token': 'b' * 64})))
        self.assertTrue(p.admit('two', ORIGIN))
        self.assertTrue(p.authenticate('two', json.dumps({'type': 'pair', 'token': TOKEN})))
        self.assertFalse(p.authenticate('stranger', json.dumps({'type': 'pair', 'token': TOKEN})))
        p.release('two')
        self.assertFalse(p.paired)

    def test_bounded_messages_and_budget(self):
        for raw in ('[]', 'null', '{"x":NaN}', '{"type":"a","type":"b"}', '{"__proto__":{}}', 'x' * (MAX_MESSAGE + 1)):
            with self.subTest(raw=raw[:30]), self.assertRaises(ValueError):
                decode_message(raw)
        now = [0.0]
        budget = Budget(lambda: now[0])
        self.assertTrue(all(budget.take(24) for _ in range(180)))
        self.assertFalse(budget.take(24))
        now[0] = 1
        self.assertTrue(budget.take(24))
        self.assertFalse(budget.take(MAX_MESSAGE + 1))


class WebSocketTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.proxy = StreamProxy('ws://127.0.0.1:1', 8765, pairing_token=TOKEN,
                                 allowed_origins=[ORIGIN], allow_control=True)
        self.proxy.pad_sock.close()
        self.proxy.pad_sock = Mock()
        self.server = await websockets.serve(self.proxy.browser_handler, '127.0.0.1', 0,
            origins=[ORIGIN], max_size=MAX_MESSAGE, max_queue=4, compression=None, close_timeout=0.1)
        self.url = f'ws://127.0.0.1:{self.server.sockets[0].getsockname()[1]}'

    async def asyncTearDown(self):
        self.server.close()
        await self.server.wait_closed()
        self.proxy.stop()

    async def connect(self):
        return await websockets.connect(self.url, origin=ORIGIN, compression=None)

    async def pair(self):
        ws = await self.connect()
        await ws.send(json.dumps({'type': 'pair', 'token': TOKEN}))
        hello = json.loads(await ws.recv())
        self.assertTrue(hello['paired'])
        return ws

    async def test_origin_rejected_before_pairing(self):
        with self.assertRaises(websockets.exceptions.InvalidStatus):
            await websockets.connect(self.url, origin='https://evil.example')
        self.assertFalse(self.proxy.ws_clients)

    async def test_no_video_or_input_before_pairing(self):
        async with await self.connect() as ws:
            await ws.send(PAD_MAGIC + bytes(PAD_REPORT_SIZE - 4))
            await ws.wait_closed()
            self.assertEqual(ws.close_code, 1008)
        self.proxy.pad_sock.sendto.assert_not_called()
        self.assertFalse(self.proxy.ws_clients)

    async def test_account_jwt_is_not_a_pairing_token(self):
        async with await self.connect() as ws:
            await ws.send(json.dumps({'type': 'join', 'token': 'fixture.account.jwt'}))
            await ws.wait_closed()
            self.assertEqual(ws.close_code, 1008)
        self.assertFalse(self.proxy.pairing.paired)

    async def test_pairing_controls_and_disconnect_unplug(self):
        async with await self.pair() as ws:
            await ws.send(PAD_MAGIC + bytes(PAD_REPORT_SIZE - 4))
            await ws.send(json.dumps({'type': 'ping', 'ts': 123}))
            self.assertEqual(json.loads(await ws.recv()), {'type': 'pong', 'ts': 123})
            self.assertEqual(self.proxy.pad_sock.sendto.call_count, 1)
            sent, target = self.proxy.pad_sock.sendto.call_args.args
            self.assertEqual(sent[4], 1)  # host's physical controller remains port 0
            self.assertEqual(target, ('127.0.0.1', 30771))
        for _ in range(20):
            if not self.proxy.pairing.paired: break
            await asyncio.sleep(0.01)
        self.assertFalse(self.proxy.pairing.paired)
        self.assertEqual(self.proxy.pad_sock.sendto.call_count, 2)

    async def test_view_only_host_ignores_controller(self):
        self.proxy.allow_control = False
        async with await self.pair() as ws:
            await ws.send(PAD_MAGIC + bytes(PAD_REPORT_SIZE - 4))
            await ws.send(json.dumps({'type': 'ping'}))
            await ws.recv()
        self.proxy.pad_sock.sendto.assert_not_called()

    async def test_malformed_authenticated_message_cannot_kill_service(self):
        async with await self.pair() as ws:
            await ws.send('[]')
            await ws.wait_closed()
            self.assertEqual(ws.close_code, 1008)
        async with await self.pair() as ws:
            await ws.send(json.dumps({'type': 'settings', 'fps': {'bad': True}}))
            await ws.wait_closed()
            self.assertEqual(ws.close_code, 1008)
        async with await self.pair() as ws:
            await ws.send(json.dumps({'type': 'ping', 'ts': 3}))
            self.assertEqual(json.loads(await ws.recv())['type'], 'pong')

    async def test_pending_admission_occupies_slot_then_releases_on_disconnect(self):
        self.proxy.pairing.max_clients = 1
        first = await self.connect()
        try:
            for _ in range(50):
                if self.proxy.pairing.pending:
                    break
                await asyncio.sleep(0.01)
            self.assertEqual(len(self.proxy.pairing.pending), 1)
            second = await self.connect()
            try:
                await asyncio.wait_for(second.wait_closed(), 1)
                self.assertEqual(second.close_code, 1008)
            finally:
                await second.close()
        finally:
            await first.close()
        for _ in range(50):
            if not self.proxy.pairing.pending:
                break
            await asyncio.sleep(0.01)
        self.assertFalse(self.proxy.pairing.pending)
        async with await self.pair() as restored:
            await restored.send(json.dumps({'type': 'ping', 'ts': 4}))
            self.assertEqual(json.loads(await restored.recv())['type'], 'pong')


class SlowViewerTests(unittest.IsolatedAsyncioTestCase):
    async def exercise(self, close_stalls):
        class Viewer:
            def __init__(self, slow):
                self.slow, self.messages = slow, []
                self.transport = Mock()
                self.closed = []

            async def send(self, value):
                if self.slow:
                    await asyncio.Event().wait()
                self.messages.append(value)

            async def close(self, code, reason):
                self.closed.append((code, reason))
                if close_stalls:
                    await asyncio.Event().wait()

        slow, fast = Viewer(True), Viewer(False)
        proxy = StreamProxy.__new__(StreamProxy)
        proxy.pairing = Pairing(TOKEN, [ORIGIN])
        proxy.ws_clients = {slow, fast}
        proxy.pad_ports = {slow: 1, fast: 2}
        proxy.pad_addr, proxy.pad_sock = ('127.0.0.1', 30771), Mock()
        for viewer in (slow, fast):
            self.assertTrue(proxy.pairing.admit(viewer, ORIGIN))
            self.assertTrue(proxy.pairing.authenticate(viewer, json.dumps({'type': 'pair', 'token': TOKEN})))
        task = asyncio.create_task(proxy.broadcast_frame('png', b'synthetic PNG envelope'))
        for _ in range(20):
            if fast.messages:
                break
            await asyncio.sleep(0.005)
        self.assertEqual(len(fast.messages), 1)  # sent while the slow peer is still stalled
        self.assertFalse(task.done())
        await asyncio.wait_for(task, 1.5)
        self.assertNotIn(slow, proxy.ws_clients)
        self.assertNotIn(slow, proxy.pairing.pending | proxy.pairing.paired)
        self.assertNotIn(slow, proxy.pad_ports)
        self.assertIn(fast, proxy.ws_clients)
        self.assertIn(fast, proxy.pairing.paired)
        self.assertEqual(slow.closed[0][0], 1008)
        report, target = proxy.pad_sock.sendto.call_args.args
        self.assertEqual((report[:4], report[4], target), (PAD_MAGIC, 1, proxy.pad_addr))
        if close_stalls:
            slow.transport.abort.assert_called_once()
        else:
            slow.transport.abort.assert_not_called()

    async def test_stalled_send_releases_controller_and_pairing_without_delaying_fast_viewer(self):
        await self.exercise(False)

    async def test_stalled_close_is_aborted_after_pairing_and_input_are_released(self):
        await self.exercise(True)


if __name__ == '__main__':
    unittest.main()
