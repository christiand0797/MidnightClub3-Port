'use strict';
// Isolated localhost server + temporary database. Never contacts or changes the deployed relay.
const test = require('node:test');
const assert = require('node:assert/strict');
const net = require('node:net');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { spawn } = require('node:child_process');
const { once } = require('node:events');
const { WebSocket } = require('ws');

async function unusedPort() {
  const probe = net.createServer(); probe.listen(0, '127.0.0.1'); await once(probe, 'listening');
  const port = probe.address().port; await new Promise(resolve => probe.close(resolve)); return port;
}
function inbox(ws) {
  const queue = [], waiting = [];
  ws.on('message', bytes => {
    const message = JSON.parse(bytes.toString());
    const index = waiting.findIndex(row => row.test(message));
    if (index >= 0) { const row = waiting.splice(index, 1)[0]; clearTimeout(row.timer); row.resolve(message); }
    else queue.push(message);
  });
  return predicate => {
    const index = queue.findIndex(predicate); if (index >= 0) return Promise.resolve(queue.splice(index, 1)[0]);
    return new Promise((resolve, reject) => {
      const row = { test: predicate, resolve };
      row.timer = setTimeout(() => { const i = waiting.indexOf(row); if (i >= 0) waiting.splice(i, 1); reject(Error('Timed out waiting for fixture message')); }, 3000);
      waiting.push(row);
    });
  };
}
test('actual HTTP/WS server contains malformed inputs, preserves room ownership and keeps the account service wired', { timeout: 20000 }, async t => {
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'mc3-relay-security-'));
  const port = await unusedPort(), base = `http://127.0.0.1:${port}`;
  const child = spawn(process.execPath, [path.join(__dirname, 'server.js')], { cwd: __dirname, windowsHide: true,
    env: { ...process.env, RELAY_PORT: String(port), DATA_DIR: directory, JWT_SECRET: 'synthetic-transport-test-key-not-a-live-service-secret', NODE_ENV: 'production', CORS_ORIGIN: 'https://ui.example' }, stdio: ['ignore', 'pipe', 'pipe'] });
  const sockets = []; let logs = '';
  child.stdout.on('data', data => { logs = (logs + data.toString()).slice(-4096); });
  child.stderr.on('data', data => { logs = (logs + data.toString()).slice(-4096); });
  t.after(async () => {
    for (const socket of sockets) socket.terminate();
    if (child.exitCode === null) { const exited = once(child, 'exit'); child.kill('SIGTERM'); await exited; }
    // Preserve the fixture database/backups for inspection; no recursive cleanup touches player data.
  });
  for (let i = 0; i < 100; i++) {
    if (child.exitCode !== null) throw Error('Fixture server exited: ' + logs);
    try { const response = await fetch(base + '/api/games'); if (response.ok) break; } catch (_) {}
    await new Promise(resolve => setTimeout(resolve, 30));
    if (i === 99) throw Error('Fixture server did not start: ' + logs);
  }
  const json = async (route, body, token, method = 'POST') => fetch(base + route, { method,
    headers: { 'Content-Type': 'application/json', ...(token ? { Authorization: 'Bearer ' + token } : {}) }, body: JSON.stringify(body) });
  const response = await fetch(base + '/api/games');
  assert.equal(response.headers.get('x-content-type-options'), 'nosniff');
  assert.equal(response.headers.get('x-frame-options'), 'DENY');
  assert.equal(response.headers.has('x-powered-by'), false);
  assert.equal((await fetch(base + '/api/games', { headers: { Origin: 'https://evil.example' } })).status, 403);
  const allowed = await fetch(base + '/api/games', { headers: { Origin: 'https://ui.example' } });
  assert.equal(allowed.headers.get('access-control-allow-origin'), 'https://ui.example');
  assert.equal((await fetch(base + '/api/login', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: '{' })).status, 400);
  assert.equal((await json('/api/login', [])).status, 400);
  assert.equal((await json('/api/login', { username: 'x'.repeat(11000) })).status, 413);
  assert.equal((await json('/api/register', { username: {}, password: 'fixture-password' })).status, 400);
  const registered = await json('/api/register', { username: 'transport_fixture_a', password: 'fixture-password-a' });
  assert.equal(registered.status, 201); const first = await registered.json();
  const secondResponse = await json('/api/register', { username: 'transport_fixture_b', password: 'fixture-password-b' });
  assert.equal(secondResponse.status, 201); const second = await secondResponse.json();
  const open = async () => {
    const ws = new WebSocket(`ws://127.0.0.1:${port}/ws`, { origin: 'https://ui.example' });
    ws.on('error', () => {}); const next = inbox(ws); sockets.push(ws); await once(ws, 'open'); return { ws, next };
  };
  const player = await open(); player.ws.send(JSON.stringify({ type: 'auth', token: first.token }));
  assert.equal((await player.next(message => message.type === 'auth_ok')).user.id, first.user.id);
  player.ws.send(JSON.stringify({ type: 'create_game', name: 'Fixture room', maxPlayers: 2 }));
  const created = await player.next(message => message.type === 'game_created'); assert.equal(created.game.playerCount, 1);
  player.ws.send(JSON.stringify({ type: 'create_game', name: 'Another room' }));
  assert.match((await player.next(message => message.type === 'error')).error, /current room|quota/i);
  player.ws.send(JSON.stringify({ type: 'chat', text: 'fixture chat' }));
  assert.equal((await player.next(message => message.type === 'chat')).text, 'fixture chat');
  player.ws.send(JSON.stringify({ type: 'leave_game' }));
  assert.deepEqual((await player.next(message => message.type === 'gameList' && message.games.length === 0)).games, []);
  const malformed = await open(); const badClosed = once(malformed.ws, 'close'); malformed.ws.send('{');
  assert.equal((await badClosed)[0], 1007);
  const binary = await open(); const binaryClosed = once(binary.ws, 'close'); binary.ws.send(Buffer.from([1, 2, 3]));
  assert.ok([1003, 1007].includes((await binaryClosed)[0]));
  const oversized = await open(); const largeClosed = once(oversized.ws, 'close'); oversized.ws.send('x'.repeat(17000));
  await largeClosed; assert.equal(child.exitCode, null);
  assert.equal((await json('/api/games', { name: 'bad', max_players: '999999' }, first.token)).status, 400);
  const one = await (await json('/api/games', { name: 'One' }, first.token)).json();
  const two = await (await json('/api/games', { name: 'Two' }, first.token)).json();
  assert.ok(one.game.id && two.game.id);
  assert.equal((await json('/api/games', { name: 'Over quota' }, first.token)).status, 429);
  const other = await (await json('/api/games', { name: 'Other' }, second.token)).json();
  assert.equal((await json(`/api/games/${one.game.id}/join`, {}, second.token)).status, 200);
  assert.equal((await json(`/api/games/${other.game.id}/leave`, {}, second.token)).status, 200);
  const retained = await (await fetch(base + `/api/games/${one.game.id}`)).json();
  assert.equal(retained.game.players.length, 2); assert.ok(retained.game.players.some(row => row.user_id === first.user.id));
  assert.equal((await json('/api/career', { current_money: '<img src=x onerror=alert(1)>' }, first.token)).status, 400);
  assert.equal((await json('/api/career', { cities_unlocked: ['<script>'] }, first.token)).status, 400);
  assert.equal((await json('/api/saves', { slot: '1x', save_data: {} }, first.token)).status, 400);
  assert.equal((await json('/api/car-setups', { car_name: {}, setup_data: {} }, first.token)).status, 400);
  for (let i = 0; i < 2; i++) assert.equal((await fetch(base + '/api/career', { headers: { Authorization: 'Bearer ' + first.token } })).status, 200);
  assert.equal(child.exitCode, null);
});
