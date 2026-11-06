// Rooms on their own objects (src/room.js) against a running server, every mix of clients:
//   npx wrangler dev --port 8787        then   MC3_WORLD=ws://127.0.0.1:8787 node --test rooms_live.test.mjs
// Local servers only (it opens a handful of guest connections).
import test from 'node:test';
import assert from 'node:assert/strict';
test.after(() => setTimeout(() => process.exit(process.exitCode || 0), 200));  // sockets may linger

const BASE = (process.env.MC3_WORLD || 'ws://127.0.0.1:8787').replace(/\/$/, '');
if (!/^ws:\/\/(127\.0\.0\.1|localhost)[:/]/.test(BASE)) throw new Error('local servers only');

// a game client: main socket, and the room socket when useRoom and offered
function client(name, room, useRoom) {
  return new Promise((resolve, reject) => {
    const c = { name, main: new WebSocket(BASE + '/ws'), roomWs: null, got: [], id: null };
    const timer = setTimeout(() => reject(new Error(name + ': no room')), 8000);
    const take = m => { if (m.type === 'input') c.got.push(JSON.parse(m.data).text); };
    c.main.onopen = () => c.main.send(JSON.stringify({ type: 'join', clientType: 'game', name }));
    c.main.onmessage = e => {
      const m = JSON.parse(e.data);
      take(m);
      if (m.type === 'auth_ok') c.main.send(JSON.stringify({ type: 'enter_room', name: room }));
      if (m.type === 'game_joined') {
        if (!useRoom || !m.roomWs) { clearTimeout(timer); return resolve(c); }
        c.roomWs = new WebSocket(BASE + m.roomWs);
        c.roomWs.onmessage = ev => take(JSON.parse(ev.data));
        c.roomWs.onopen = () => setTimeout(() => { clearTimeout(timer); resolve(c); }, 300);  // the World learns of it
        c.roomWs.onerror = () => reject(new Error(name + ': room socket refused'));
      }
    };
  });
}
const say = (c, text) => (c.roomWs || c.main).send(JSON.stringify({ type: 'input', data: JSON.stringify({ text }) }));
const settle = () => new Promise(r => setTimeout(r, 600));
const close = (...cs) => cs.forEach(c => { try { c.roomWs?.close(); c.main.close(); } catch (_) {} });

test('room sockets reach each other without the World', async () => {
  const room = 'rooms-test-' + Date.now();
  const a = await client('a', room, true), b = await client('b', room, true);
  assert.ok(a.roomWs && b.roomWs, 'game_joined offers roomWs');
  say(a, 'a->b'); await settle();
  assert.deepEqual(b.got, ['a->b']);
  close(a, b);
});

test('an older client and a room client hear each other through the World', async () => {
  const room = 'rooms-mix-' + Date.now();
  const r = await client('room', room, true), old = await client('old', room, false);
  say(r, 'room->old'); say(old, 'old->room'); await settle();
  assert.deepEqual(old.got, ['room->old']);
  assert.deepEqual(r.got, ['old->room']);
  close(r, old);
});

test('a room ticket is used once and only for its room', async () => {
  const room = 'rooms-ticket-' + Date.now();
  const main = new WebSocket(BASE + '/ws');
  const path = await new Promise(resolve => {
    main.onopen = () => main.send(JSON.stringify({ type: 'join', clientType: 'game', name: 't' }));
    main.onmessage = e => { const m = JSON.parse(e.data); if (m.type === 'auth_ok') main.send(JSON.stringify({ type: 'enter_room', name: room })); if (m.type === 'game_joined') resolve(m.roomWs); };
  });
  const opens = url => new Promise(r => { const w = new WebSocket(url); w.onopen = () => { w.close(); r(true); }; w.onerror = () => r(false); });
  assert.equal(await opens(BASE + path.replace(/\/ws\/room\/[^?]+/, '/ws/room/elsewhere')), false);  // wrong room
  assert.equal(await opens(BASE + path), false);  // the wrong-room try already spent the ticket
  main.close();
});
