const test = require('node:test');
const assert = require('node:assert/strict');
const { Client, normalizeMessage } = require('./relay-client');

function fixture() {
  let sequence = 0;
  const timers = new Map(), sockets = [], received = [], ready = [], closed = [];
  class Socket {
    constructor() { this.readyState = 0; this.bufferedAmount = 0; this.sent = []; this.closes = []; sockets.push(this); }
    open() { this.readyState = 1; this.onopen(); }
    send(data) { this.sent.push(JSON.parse(data)); }
    close(code, reason) { assert(code === 1000 || code >= 3000 && code <= 4999); this.closes.push({ code, reason }); this.readyState = 2; }
    message(data) { this.onmessage({ data: typeof data === 'string' ? data : JSON.stringify(data) }); }
    closed(code = 1006, reason = '') { this.readyState = 3; this.onclose({ code, reason }); }
  }
  const client = new Client({ url: 'ws://localhost:8080', socketFactory: () => new Socket(),
    setTimer(fn, delay) { const id = ++sequence; timers.set(id, { fn, delay }); return id; }, clearTimer(id) { timers.delete(id); },
    onReady: user => ready.push(user), onMessage: msg => received.push(msg), onDisconnect: event => closed.push(event) });
  const fire = delay => { for (const [id, timer] of [...timers]) if (timer.delay === delay) { timers.delete(id); timer.fn(); } };
  return { client, sockets, received, ready, closed, timers, fire };
}
const a = { token: 'jwt.A.token', user: { id: 'account-A', username: 'Player A' } };
const b = { token: 'jwt.B.token', user: { id: 'account-B', username: 'Player B' } };
const authenticated = (ws, session = a) => ws.message({ type: 'auth_ok', user: session.user });

test('relay sends account auth once per socket, and room/chat messages never carry credentials', () => {
  const f = fixture(); f.client.setSession(a); const ws = f.sockets[0]; ws.open();
  assert.deepEqual(ws.sent, [{ type: 'auth', token: a.token, clientType: 'browser' }]);
  assert.equal(f.client.send({ type: 'list' }), false); authenticated(ws);
  f.client.setSession(a); assert.equal(f.sockets.length, 1); assert.equal(ws.sent.length, 1);
  assert.equal(f.client.send({ type: 'join_game', gameId: 'main-lobby' }), true);
  assert.equal(f.client.send({ type: 'chat', text: 'Hello' }), true);
  assert.equal(f.client.send({ type: 'join_game', gameId: 'room', token: a.token }), false);
  assert.equal(f.client.send({ type: 'auth', token: a.token }), false);
  assert(ws.sent.slice(1).every(msg => !('token' in msg)));
  ws.bufferedAmount = 65537; assert.equal(f.client.send({ type: 'list' }), false);
  f.client.setSession(null); assert.equal(f.timers.size, 0);
});

test('switching accounts invalidates all old callbacks before connecting with the new account', () => {
  const f = fixture(); f.client.setSession(a); const old = f.sockets[0]; old.open(); authenticated(old);
  f.client.setSession(b); const current = f.sockets[1]; current.open(); authenticated(current, b);
  old.onopen(); old.message({ type: 'game_joined', game: { id: 'old-room' }, role: 'player' }); old.closed();
  assert.equal(f.received.length, 0); assert.equal(f.closed.length, 0); assert.equal(f.ready.length, 2);
  assert.equal(current.closes.length, 0); assert.equal(current.sent[0].token, b.token);
  f.client.setSession(null); current.message({ type: 'chat', from: 'old', text: 'late' });
  assert.equal(f.received.length, 0); assert.equal(f.timers.size, 0);
});

test('mismatched auth and malformed messages never reach the UI or overwrite the account', () => {
  for (const message of [
    { type: 'auth_ok', user: b.user }, '{bad-json', '[]',
    { type: 'game_joined', game: { id: 'not-yet-authenticated' }, role: 'player' },
    { type: 'auth_ok', user: { id: a.user.id, username: 'Another name' } },
    { type: 'gameList', games: [] }
  ]) {
    const f = fixture(); f.client.setSession(a); f.sockets[0].open(); f.sockets[0].message(message);
    assert.equal(f.ready.length, 0); assert.equal(f.received.length, 0); assert.equal(f.client.ready, false);
    assert.equal(f.closed.at(-1).code, 4008); assert.equal(f.timers.size, 0);
    assert.equal(f.client.session.user.id, a.user.id);
  }
});

test('a transient close retries only the same live session; logout cancels saved retry callbacks', () => {
  const f = fixture(); f.client.setSession(a); f.sockets[0].open(); authenticated(f.sockets[0]); f.sockets[0].closed();
  f.fire(3000); assert.equal(f.sockets.length, 2); f.sockets[1].open(); authenticated(f.sockets[1]); f.sockets[1].closed();
  const saved = [...f.timers.values()][0].fn;
  f.client.setSession(null); saved(); assert.equal(f.sockets.length, 2); assert.equal(f.timers.size, 0);
  f.client.setSession(b); f.fire(5000); assert.equal(f.closed.at(-1).code, 4008); assert.equal(f.timers.size, 0);
});

test('current relay room protocol and uncapped Main Lobby are normalized without conflating account and peer IDs', () => {
  const list = normalizeMessage({ type: 'gameList', games: [{ id: 'main-lobby', name: 'Main Lobby', status: 'waiting', player_count: 14, max_players: 0, open: true }] });
  assert.equal(list.games[0].open, true); assert.equal(list.games[0].max_players, 0);
  const hosted = normalizeMessage({ type: 'gameList', games: [{ id: 'uncapped-hosted', name: 'Hosted', status: 'waiting', player_count: 14, max_players: 0, open: false }] });
  assert.equal(hosted.games[0].max_players, 0); assert.equal(hosted.games[0].open, false);
  const joined = normalizeMessage({ type: 'game_joined', game: { id: 'main-lobby', name: 'Main Lobby' }, role: 'player', color: '#44ff44' });
  assert.deepEqual(joined, { type: 'room_joined', gameId: 'main-lobby', name: 'Main Lobby', role: 'player', created: false, color: '#44ff44' });
  assert.equal('id' in joined, false); assert.equal(normalizeMessage({ type: 'player_joined', username: 'Friend' }).username, 'Friend');
  assert.equal(normalizeMessage({ type: 'input', data: { irrelevant: true } }), null);
});

test('malformed or oversized rooms, chat, metadata, binary and repeated auth fail closed', () => {
  const malformed = [
    { type: 'gameList', games: new Array(129).fill({}) },
    { type: 'gameList', games: [{ id: 'room', status: 'waiting', player_count: 1, max_players: -1 }] },
    { type: 'game_joined', game: { id: '../room' }, role: 'player' },
    { type: 'chat', from: 'Other', text: 'a'.repeat(513) },
    { type: 'auth_ok', user: a.user },
    'a'.repeat(128 * 1024 + 1)
  ];
  for (const message of malformed) {
    const f = fixture(); f.client.setSession(a); f.sockets[0].open(); authenticated(f.sockets[0]); f.sockets[0].message(message);
    assert.equal(f.received.length, 0); assert.equal(f.client.ready, false); assert.equal(f.timers.size, 0);
  }
  const f = fixture(); f.client.setSession(a); f.sockets[0].open(); authenticated(f.sockets[0]);
  f.sockets[0].onmessage({ data: new ArrayBuffer(24) }); assert.equal(f.client.ready, false);
});

test('actual account relay connects from a direct page URL, joins Main Lobby and accepts an uncapped hosted room', { timeout: 15000 }, async t => {
  const fs = require('node:fs'), os = require('node:os'), path = require('node:path');
  const net = require('node:net'), { once } = require('node:events'), { spawn } = require('node:child_process');
  const { WebSocket } = require('ws');
  const { resolveEndpoints } = require('./account-ui');
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'mc3-browser-relay-'));
  const probe = net.createServer(); probe.listen(0, '127.0.0.1'); await once(probe, 'listening');
  const port = probe.address().port; await new Promise(resolve => probe.close(resolve));
  const base = `http://127.0.0.1:${port}`, relayDirectory = path.resolve(__dirname, '../relay');
  const child = spawn(process.execPath, [path.join(relayDirectory, 'server.js')], { cwd: relayDirectory, windowsHide: true,
    env: { ...process.env, RELAY_HOST: '127.0.0.1', RELAY_PORT: String(port), DATA_DIR: directory,
      JWT_SECRET: 'synthetic-browser-relay-fixture-key-not-for-deployment', MAX_PLAYERS: '0', NODE_ENV: 'test', PUBLIC_ORIGIN: base, CORS_ORIGIN: '' },
    stdio: ['ignore', 'pipe', 'pipe'] });
  let log = ''; child.stdout.on('data', bytes => { log = (log + bytes).slice(-4096); }); child.stderr.on('data', bytes => { log = (log + bytes).slice(-4096); });
  const clients = [], sockets = [];
  t.after(async () => {
    for (const item of clients) item.client.setSession(null);
    for (const ws of sockets) ws.terminate();
    if (child.exitCode === null) { const exited = once(child, 'exit'); child.kill('SIGTERM'); await exited; }
    // Retain this synthetic database for inspection; no personal data or live service is touched.
  });
  for (let i = 0; i < 100; i++) {
    if (child.exitCode !== null) throw Error('Fixture relay exited: ' + log);
    try { if ((await fetch(base + '/healthz')).ok) break; } catch (_) {}
    if (i === 99) throw Error('Fixture relay startup timed out: ' + log);
    await new Promise(resolve => setTimeout(resolve, 25));
  }
  async function player(name) {
    const response = await fetch(base + '/api/register', { method: 'POST', headers: { 'Content-Type': 'application/json', Origin: base },
      body: JSON.stringify({ username: name, password: 'isolated-fixture-password' }) });
    assert.equal(response.status, 201); const session = await response.json(), messages = [], waiting = [];
    const push = message => {
      const index = waiting.findIndex(row => row.predicate(message));
      if (index < 0) messages.push(message);
      else { const row = waiting.splice(index, 1)[0]; clearTimeout(row.timer); row.resolve(message); }
    };
    const client = new Client({ url: resolveEndpoints(base + '/').relay,
      socketFactory(url) { const ws = new WebSocket(url, { origin: base }); sockets.push(ws); return ws; },
      onReady: user => push({ type: 'ready', user }), onMessage: push, onDisconnect: event => push({ type: 'closed', ...event }) });
    const item = { client, cookie: response.headers.get('set-cookie').split(';')[0], session,
      next(predicate) {
        const index = messages.findIndex(predicate);
        if (index >= 0) return Promise.resolve(messages.splice(index, 1)[0]);
        return new Promise((resolve, reject) => { const row = { predicate, resolve };
          row.timer = setTimeout(() => { waiting.splice(waiting.indexOf(row), 1); reject(Error('Browser fixture message timed out')); }, 3000); waiting.push(row); });
      } };
    clients.push(item); client.setSession(session);
    assert.equal((await item.next(message => message.type === 'ready')).user.id, session.user.id);
    return item;
  }
  const host = await player('browser_test_host'), guest = await player('browser_test_guest');
  assert.equal(host.client.send({ type: 'list' }), true);
  const list = await host.next(message => message.type === 'gameList');
  assert.deepEqual(list.games.map(room => [room.id, room.max_players, room.open]), [['main', 0, true]]);
  for (const player of [host, guest]) {
    assert.equal(player.client.send({ type: 'join_game', gameId: 'main' }), true);
    assert.equal((await player.next(message => message.type === 'room_joined')).gameId, 'main');
  }
  host.client.send({ type: 'chat', text: 'A real browser-protocol fixture message' });
  assert.equal((await guest.next(message => message.type === 'chat')).from, 'browser_test_host');
  host.client.send({ type: 'leave_game' });
  await host.next(message => message.type === 'gameList' && message.games[0].player_count === 1);
  host.client.send({ type: 'create_game', name: 'Uncapped browser fixture' });
  const created = await host.next(message => message.type === 'room_joined'); assert.equal(created.created, true);
  const hosted = await host.next(message => message.type === 'gameList' && message.games.some(room => room.id === created.gameId));
  assert.equal(hosted.games.find(room => room.id === created.gameId).max_players, 0);
  assert.equal(host.client.ready, true); // max_players=0 is valid even when open=false.
  assert.equal((await fetch(base + '/api/logout', { method: 'POST', headers: { 'Content-Type': 'application/json', Origin: base, Cookie: host.cookie }, body: '{}' })).status, 200);
  assert.equal((await host.next(message => message.type === 'closed')).code, 1008);
  assert.equal(guest.client.ready, true);
  assert.equal(host.client.send({ type: 'chat', text: 'after logout' }), false);
});
