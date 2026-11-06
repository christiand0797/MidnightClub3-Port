'use strict';
// Scale behaviour of the World (docs/notes/online_scaling.md): Main Lobby instances, the player cap and its queue,
// /api/players. Runs the real World with a minimal Durable Object stub, as world_security.test.cjs does.
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const { webcrypto } = require('node:crypto');
const { validateMessage } = require('../relay/transport_security');
const { Social } = require('../relay/social');
const source = fs.readFileSync(path.join(__dirname, 'src/world.js'), 'utf8')
  .replace(/^import .*;\r?\n/gm, '').replace('export class World', 'class World') + '\nmodule.exports = { World };';
const sandbox = { module: { exports: {} }, DurableObject: class { constructor(ctx, env) { this.ctx = ctx; this.env = env; } },
  validateMessage, Social, TextEncoder, crypto: webcrypto, Date, Response, URL, console, JSON, setTimeout, clearTimeout };
vm.runInNewContext(source, sandbox, { filename: 'world.js' });
const { World } = sandbox.module.exports;

function socket(info) {
  return { info: structuredClone(info), sent: [], closed: [], bufferedAmount: 0,
    deserializeAttachment() { return structuredClone(this.info); }, serializeAttachment(v) { this.info = structuredClone(v); },
    send(raw) { this.sent.push(JSON.parse(raw)); }, close(code, reason) { this.closed.push({ code, reason }); } };
}
async function fixture(env = {}) {
  const sockets = [];
  const ctx = { getWebSockets: () => sockets, blockConcurrencyWhile(fn) { this.ready = fn(); },
    storage: { get: async () => undefined, put: async () => {}, getAlarm: async () => null, setAlarm: async () => {} } };
  const world = new World(ctx, env); await ctx.ready;
  const add = () => { const ws = socket({ ip: 'x' + sockets.length, at: Date.now() }); sockets.push(ws); return ws; };
  const join = async (name) => { const ws = add(); await world.webSocketMessage(ws, JSON.stringify({ type: 'join', name, clientType: 'game' })); return ws; };
  const close = async (ws) => { sockets.splice(sockets.indexOf(ws), 1); await world.webSocketClose(ws, 1000); };
  return { world, sockets, add, join, close };
}
const last = (ws, type) => ws.sent.filter(m => m.type === type).at(-1);

test('the Main Lobby fills instances of 32, the next player opens Main Lobby 2', async () => {
  const f = await fixture();
  const players = [];
  for (let i = 0; i < 33; i++) {
    const ws = await f.join('p' + i);
    await f.world.webSocketMessage(ws, JSON.stringify({ type: 'enter_room', name: '' }));
    players.push(ws);
  }
  assert.equal(last(players[0], 'game_joined').game.id, 'main');
  assert.equal(last(players[31], 'game_joined').game.id, 'main');
  const extra = last(players[32], 'game_joined').game;
  assert.equal(extra.id, 'main-2');
  assert.equal(extra.name, 'Main Lobby 2');
  assert.equal(extra.main, true);
  const lobbies = f.world.listing().filter(r => r.main);
  assert.equal(JSON.stringify(lobbies.map(r => [r.name, r.player_count])), JSON.stringify([['Main Lobby', 32], ['Main Lobby 2', 1]]));
  // a datagram in Main Lobby 2 reaches its room only
  const other = await f.join('q');
  await f.world.webSocketMessage(other, JSON.stringify({ type: 'enter_room', name: '' }));
  assert.equal(last(other, 'game_joined').game.id, 'main-2');  // the fullest instance with room
  await f.world.webSocketMessage(players[32], JSON.stringify({ type: 'input', data: '{"payload":"AA=="}' }));
  assert.equal(last(other, 'input').from, 'p32');
  assert.equal(players[0].sent.filter(m => m.type === 'input').length, 0);
  // the last player leaving an instance removes it; the first instance stays
  await f.close(players[32]); await f.close(other);
  assert.equal(f.world.rooms.has('main-2'), false);
  assert.equal(f.world.rooms.has('main'), true);
});

test('past the player cap a sign-in waits in a queue and is admitted when a place frees', async () => {
  const f = await fixture({ MAX_PLAYERS: '2' });
  const a = await f.join('a'), b = await f.join('b'), c = await f.join('c'), d = await f.join('d');
  assert.ok(last(a, 'auth_ok') && last(b, 'auth_ok'));
  assert.equal(last(c, 'auth_ok'), undefined);
  assert.deepEqual([last(c, 'queue').pos, last(d, 'queue').pos], [1, 2]);
  // a queued client cannot enter rooms
  await f.world.webSocketMessage(c, JSON.stringify({ type: 'enter_room', name: '' }));
  assert.equal(last(c, 'game_joined'), undefined);
  await f.close(a);
  assert.equal(last(c, 'auth_ok').user.username, 'c');
  assert.equal(last(d, 'queue').pos, 1);
  await f.world.webSocketMessage(c, JSON.stringify({ type: 'enter_room', name: '' }));
  assert.equal(last(c, 'game_joined').game.id, 'main');
  // queued clients are not expired as never-authenticated ones
  d.info.at = Date.now() - 60000; f.world.cache.delete(d);
  await f.world.alarm();
  assert.equal(d.closed.length, 0);
});

test('/api/players reports players, queue and lobbies', async () => {
  const f = await fixture({ MAX_PLAYERS: '1' });
  const a = await f.join('a'); await f.join('b');
  await f.world.webSocketMessage(a, JSON.stringify({ type: 'enter_room', name: '' }));
  const r = await (await f.world.fetch(new Request('https://mc3.test/api/players'))).json();
  assert.equal(r.players, 1); assert.equal(r.queue, 1); assert.equal(r.max, 1); assert.equal(r.full, true);
  assert.equal(JSON.stringify(r.lobbies), JSON.stringify([{ name: 'Main Lobby', players: 1 }]));
});
