'use strict';
const test = require('node:test');
const assert = require('node:assert/strict');
const { EventEmitter } = require('node:events');
const { TransportSecurity, TokenBucket, validateMessage, validateRoomRequest, ipFromRequest, originAllowed } = require('./transport_security');

class Socket extends EventEmitter {
  constructor() { super(); this.readyState = 1; this.bufferedAmount = 0; this.sent = []; this.pings = 0; this.terminated = 0; this.closed = []; }
  send(value, callback) { this.sent.push(value); if (callback) callback(); }
  close(code, reason) { this.closed.push({ code, reason }); this.readyState = 2; }
  terminate() { this.terminated++; this.readyState = 3; }
  ping() { this.pings++; }
}
const req = (ip = '192.0.2.1') => ({ socket: { remoteAddress: ip }, headers: { 'x-forwarded-for': '198.51.100.1' } });
function fixture(options = {}) {
  let now = 0;
  const security = new TransportSecurity({ now: () => now, ...options });
  return { security, advance: ms => { now += ms; }, socket(user = 'user1', ip = '192.0.2.1') {
    const ws = new Socket(); assert.equal(security.admit(ws, req(ip)).ok, true);
    if (user) assert.equal(security.markAuthenticated(ws, user).ok, true);
    return ws;
  } };
}
const read = (security, ws, value) => security.read(ws, Buffer.from(JSON.stringify(value)), false);

test('token bucket is bounded, refills by elapsed time, and rejects invalid/backward time credit', () => {
  const bucket = new TokenBucket(2, 3, 1000);
  assert.equal(bucket.take(3, 1000), true);
  assert.equal(bucket.take(1, 0), false);
  assert.equal(bucket.take(1, 1500), true);
  assert.equal(bucket.take(4, 100000), false);
  assert.equal(bucket.take(3, 100000), true);
  assert.equal(bucket.take(NaN, 100000), false);
});
test('direct peer address and strict browser origin policy ignore forwarded spoofing', () => {
  assert.equal(ipFromRequest(req('::ffff:192.0.2.1')), '192.0.2.1');
  assert.equal(ipFromRequest({ headers: { 'x-forwarded-for': '192.0.2.2' } }), 'unknown');
  assert.equal(originAllowed(undefined, 'relay.example'), true);
  assert.equal(originAllowed('https://relay.example', 'relay.example'), true);
  assert.equal(originAllowed('https://ui.example', 'relay.example', 'https://ui.example'), true);
  for (const origin of ['null', 'https://evil.example', 'https://relay.example.evil', 'https://relay.example/path', 'javascript:alert(1)']) assert.equal(originAllowed(origin, 'relay.example', '*'), false);
  assert.equal(originAllowed('https://dev.example', 'localhost:8080', '*', true), true);
});
test('global, IP, account and address-table quotas release without growing without bound', () => {
  const f = fixture({ maxConnections: 2, maxConnectionsPerIp: 1, maxIpStates: 2, maxConnectionsPerUser: 1 });
  const first = f.socket('same');
  assert.equal(f.security.admit(new Socket(), req()).ok, false);
  const second = f.socket(null, '192.0.2.2');
  assert.equal(f.security.markAuthenticated(second, 'same').ok, false);
  assert.equal(f.security.admit(new Socket(), req('192.0.2.3')).ok, false);
  f.security.release(first); f.security.release(second);
  f.advance(900001); f.security.prune();
  assert.equal(f.security.ips.size, 0);
  assert.equal(f.security.users.size, 0);
});
test('ws Buffer text, split buffers, ArrayBuffer and strings parse; malformed bytes cannot throw', () => {
  const { security, socket } = fixture(); const ws = socket();
  assert.equal(read(security, ws, { type: 'list' }).ok, true);
  assert.equal(security.read(ws, '{"type":"list"}', false).ok, true);
  assert.equal(security.read(ws, [Buffer.from('{"type":'), Buffer.from('"list"}')], false).ok, true);
  const array = Uint8Array.from(Buffer.from('{"type":"list"}'));
  assert.equal(security.read(ws, array.buffer, false).ok, true);
  for (const raw of [Buffer.from('{'), Buffer.from('null'), Buffer.from('[]'), Buffer.from('1'), Buffer.from([0xff])]) assert.equal(security.read(ws, raw, false).ok, false);
  const malformedBinary = security.read(ws, Buffer.alloc(1), true);
  assert.equal(malformedBinary.ok, false); assert.ok([1003, 1007].includes(malformedBinary.code));
  assert.equal(security.read(ws, Buffer.alloc(16385), false).code, 1009);
  assert.equal(security.read(ws, { unexpected: true }, false).ok, false);
});
test('message and byte token buckets cover each connection and aggregate IP traffic', () => {
  const f = fixture({ messageBurst: 2, messagesPerSecond: 1, ipMessageBurst: 3, ipMessagesPerSecond: 1 });
  const a = f.socket('a'), b = f.socket('b');
  assert.equal(read(f.security, a, { type: 'list' }).ok, true);
  assert.equal(read(f.security, a, { type: 'list' }).ok, true);
  assert.equal(read(f.security, a, { type: 'list' }).ok, false);
  assert.equal(read(f.security, b, { type: 'list' }).ok, true);
  assert.equal(read(f.security, b, { type: 'list' }).ok, false);
  f.advance(1000); assert.equal(read(f.security, b, { type: 'list' }).ok, true);
  const bytes = fixture({ byteBurst: 20, bytesPerSecond: 1 }); const ws = bytes.socket();
  assert.equal(read(bytes.security, ws, { type: 'list' }).ok, true);
  assert.equal(read(bytes.security, ws, { type: 'list' }).ok, false);
});
test('message schemas reject unknown types, prototype keys, infinities, excessive nesting and oversized chat', () => {
  for (const data of [null, [], { type: 'unknown' }, { type: 'set_ready', ready: 'yes' }, { type: 'join_game', gameId: '../a' },
    { type: 'input', data: { x: Infinity } }, { type: 'chat', text: 'x'.repeat(513) }, { type: 'ping', ts: -1 },
    JSON.parse('{"type":"input","data":{"__proto__":{}}}')]) assert.equal(validateMessage(data, 8).ok, false);
  let deep = {}; for (let i = 0; i < 12; ++i) deep = { child: deep };
  assert.equal(validateMessage({ type: 'input', data: deep }, 8).ok, false);
  assert.equal(validateRoomRequest({ name: 'room', maxPlayers: 64 }).ok, true);    // no ceiling on a host's own cap
  assert.equal(validateRoomRequest({ name: 'room', maxPlayers: -1 }).ok, false);
  assert.equal(validateRoomRequest({ name: 1, maxPlayers: 1 }).ok, false);
  assert.deepEqual(validateRoomRequest({ name: ' room ' }).room, { name: 'room', maxPlayers: 0, region: 'global' });  // 0: no player cap
});
test('authentication deadline and repeated auth cannot be bypassed between heartbeat ticks', () => {
  const f = fixture(); const ws = f.socket(null);
  assert.equal(read(f.security, ws, { type: 'list' }).ok, false);
  assert.equal(read(f.security, ws, { type: 'auth', token: 'a.b.c' }).ok, true);
  f.advance(15000);
  assert.equal(read(f.security, ws, { type: 'auth', token: 'a.b.c' }).ok, false);
  assert.equal(f.security.markAuthenticated(ws, 'user').ok, false);
  const second = f.socket('user2');
  assert.equal(read(f.security, second, { type: 'auth', token: 'a.b.c' }).ok, false);
});
test('room quotas prevent repeated creation and multi-room membership while counting players by account', () => {
  const f = fixture({ maxRooms: 1 }); const host = f.socket('host'), companion = f.socket('host'), guest = f.socket('guest'), stranger = f.socket('stranger');
  assert.equal(f.security.createRoom(host, 'one', { name: 'One', maxPlayers: 2 }).ok, true);
  assert.equal(f.security.createRoom(host, 'two', { name: 'Two' }).ok, false);
  assert.equal(f.security.createRoom(guest, 'two', { name: 'Two' }).ok, false);
  assert.equal(f.security.joinRoom(companion, 'one').ok, true);
  assert.equal(f.security.joinRoom(guest, 'one').ok, true);
  assert.equal(f.security.joinRoom(stranger, 'one').ok, false);
  f.security.leaveRoom(guest); assert.equal(f.security.joinRoom(stranger, 'one').ok, true);
  f.security.removeRoom('one'); assert.equal(f.security.rooms.size, 0);
  assert.equal(f.security.createRoom(guest, 'two', { name: 'Two' }).ok, true);
});
test('spectator and per-IP room-creation quotas are independently bounded', () => {
  const f = fixture({ maxSpectators: 1 }); const host = f.socket('host'), a = f.socket('a'), b = f.socket('b');
  assert.equal(f.security.createRoom(host, 'one', { name: 'One' }).ok, true);
  assert.equal(f.security.joinRoom(a, 'one', { spectator: true }).ok, true);
  assert.equal(f.security.joinRoom(b, 'one', { spectator: true }).ok, false);
  f.security.removeRoom('one');
  assert.equal(f.security.createRoom(host, 'two', { name: 'Two' }).ok, true); f.security.removeRoom('two');
  assert.equal(f.security.createRoom(host, 'three', { name: 'Three' }).ok, false);
});
test('heartbeat checks every socket and handles pong/error/close without crashing', () => {
  const f = fixture({ heartbeatMs: 100, pongTimeoutMs: 100, closeGraceMs: 10 });
  const a = new Socket(), b = new Socket();
  f.security.attach(a, req(), () => {}); f.security.attach(b, req(), () => {});
  f.security.markAuthenticated(a, 'a'); f.security.markAuthenticated(b, 'b');
  f.advance(100); f.security.tick(); assert.equal(a.pings, 1); assert.equal(b.pings, 1);
  b.emit('pong'); f.advance(100); f.security.tick();
  assert.equal(a.terminated, 1); assert.equal(b.pings, 2); assert.equal(b.terminated, 0);
  b.emit('error', new Error('synthetic')); assert.equal(b.terminated, 1); assert.equal(f.security.sockets.size, 0);
});
test('attached malformed messages and synchronous/asynchronous handler failures close only their own socket', async () => {
  const f = fixture(); const a = new Socket(), b = new Socket(), c = new Socket();
  f.security.attach(a, req(), () => { throw Error('handler'); }); f.security.markAuthenticated(a, 'a');
  a.emit('message', Buffer.from('{"type":"list"}'), false); assert.equal(a.closed[0].code, 1011);
  f.security.attach(b, req(), () => Promise.reject(Error('async'))); f.security.markAuthenticated(b, 'b');
  b.emit('message', Buffer.from('{"type":"list"}'), false); await Promise.resolve(); assert.equal(b.closed[0].code, 1011);
  f.security.attach(c, req(), () => { throw Error('must not be reached'); });
  c.emit('message', Buffer.from('{'), false); assert.equal(c.closed[0].code, 1007);
});
test('outbound backpressure, send/ping failures and chat flood are contained', () => {
  const f = fixture(); const ws = f.socket();
  assert.equal(f.security.send(ws, { type: 'test' }), true);
  for (let i = 0; i < 6; ++i) assert.equal(read(f.security, ws, { type: 'chat', text: 'hi' }).ok, true);
  assert.equal(read(f.security, ws, { type: 'chat', text: 'hi' }).ok, false);
  ws.bufferedAmount = 1024 * 1024 + 1; assert.equal(f.security.send(ws, {}), false); assert.equal(ws.closed.length, 1);
  const second = f.socket('second'); second.send = () => { throw Error('send'); };
  assert.equal(f.security.send(second, {}), false);
  const third = f.socket('third'); third.ping = () => { throw Error('ping'); };
  f.advance(30000); f.security.tick(); assert.equal(third.terminated, 1);
});
