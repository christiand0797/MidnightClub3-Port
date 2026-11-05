// End-to-end accounts + cloud saves + car clubs against a running World:
//   MC3_WORLD=http://127.0.0.1:8787 node accounts_live.test.mjs      (wrangler dev)
//   MC3_WORLD=https://mc3.top node accounts_live.test.mjs             (live; makes a throwaway test account)
import test from 'node:test';
import assert from 'node:assert/strict';
import { createHash, randomBytes } from 'node:crypto';

const BASE = (process.env.MC3_WORLD || 'http://127.0.0.1:8787').replace(/\/$/, '');
const ORIGIN = new URL(BASE).origin;
const name = 'zt' + randomBytes(5).toString('hex').slice(0, 9);
const password = randomBytes(12).toString('base64url');
let cookie = '', token = '';

async function api(path, { method, body, auth, withCookie = true, origin = ORIGIN } = {}) {
  method = method || (body === undefined ? 'GET' : 'POST');
  const headers = { Origin: origin };
  if (body !== undefined) headers['Content-Type'] = 'application/json';
  if (auth) headers.Authorization = 'Bearer ' + auth;
  if (withCookie && cookie) headers.Cookie = cookie;
  const r = await fetch(BASE + '/api/' + path, { method, headers, body: body === undefined ? undefined : JSON.stringify(body) });
  const set = r.headers.get('set-cookie');
  return { status: r.status, set, json: await r.json().catch(() => null) };
}
function ws(onOpen) {
  return new Promise((resolve, reject) => {
    const s = new WebSocket(BASE.replace(/^http/, 'ws') + '/ws'), got = [];
    const timer = setTimeout(() => { s.close(); reject(Error('timeout: ' + JSON.stringify(got))); }, 10000);
    s.onopen = () => onOpen(s);
    s.onmessage = e => { const m = JSON.parse(e.data); got.push(m); if (s.until && s.until(m, got)) { clearTimeout(timer); s.close(); resolve(got); } };
    s.onclose = e => { clearTimeout(timer); resolve(got.concat([{ type: '_closed', code: e.code }])); };
    s.onerror = () => {};
  });
}

test('register, sign-in rules and device cookie', async () => {
  let r = await api('register', { body: { username: name, password: 'short' } });
  assert.equal(r.status, 400);
  r = await api('register', { body: { username: name, password }, origin: 'https://evil.example' });
  assert.equal(r.status, 403);
  r = await api('register', { body: { username: name, password, remember: true } });
  assert.equal(r.status, 201, JSON.stringify(r.json));
  assert.equal(r.json.user.username, name);
  assert.match(r.set, /mc3_device=[A-Za-z0-9_-]{43}; Path=\/; HttpOnly; SameSite=Lax/);
  cookie = r.set.split(';')[0]; token = r.json.token;
  r = await api('register', { body: { username: name.toUpperCase(), password }, withCookie: false });
  assert.equal(r.status, 409);
  r = await api('login', { body: { username: name, password: password + 'x' }, withCookie: false });
  assert.equal(r.status, 401);
  r = await api('login', { body: { username: name.toUpperCase(), password }, withCookie: false });
  assert.equal(r.status, 200);
  r = await api('session', { body: {} });
  assert.equal(r.status, 200); assert.equal(r.json.user.username, name);
  r = await api('profile', { auth: token, withCookie: false });
  assert.equal(r.json.username, name);
  r = await api('profile', { auth: token.slice(0, -2) + 'AA', withCookie: false });
  assert.equal(r.status, 401);
});

test('cloud saves: empty, write, conflict, read back', async () => {
  let r = await api('cloud', { auth: token, withCookie: false });
  assert.equal(r.status, 200); assert.equal(r.json.revision, 0);
  const bytes = randomBytes(700 * 1024);   // spans two storage chunks once base64'd
  const file = { path: 'hdd/E/UDATA/test/save.bin', encoding: 'base64', data: bytes.toString('base64'),
    sha256: createHash('sha256').update(bytes).digest('hex') };
  r = await api('cloud', { method: 'PUT', auth: token, withCookie: false, body: { base_revision: 0, files: [file], settings: { a: 1 } } });
  assert.equal(r.status, 200, JSON.stringify(r.json)); assert.equal(r.json.revision, 1);
  r = await api('cloud', { method: 'PUT', auth: token, withCookie: false, body: { base_revision: 0, files: [], settings: {} } });
  assert.equal(r.status, 409);
  r = await api('cloud', { method: 'PUT', auth: token, withCookie: false, body: { base_revision: 1, files: [{ ...file, sha256: '0'.repeat(64) }], settings: {} } });
  assert.equal(r.status, 400);
  r = await api('cloud', { method: 'PUT', auth: token, withCookie: false, body: { base_revision: 1, files: [{ ...file, path: '../x' }], settings: {} } });
  assert.equal(r.status, 400);
  r = await api('cloud', { auth: token, withCookie: false });
  assert.equal(r.json.revision, 1); assert.equal(r.json.files[0].data, file.data); assert.deepEqual(r.json.settings, { a: 1 });
  r = await api('cloud');   // the device cookie alone also reads
  assert.equal(r.json.revision, 1);
});

test('signed-in players get car clubs over the WebSocket; guests cannot', async () => {
  const club = 'Z' + name.slice(2, 10);
  const got = await ws(s => {
    s.until = m => m.type === 'club' || m.type === 'error';
    s.onopen = null;
    s.send(JSON.stringify({ type: 'auth', token, clientType: 'browser' }));
    const prev = s.onmessage;
    s.onmessage = e => { const m = JSON.parse(e.data); if (m.type === 'auth_ok') s.send(JSON.stringify({ type: 'club_create', name: club, tag: 'ZT' + name.slice(2, 4).toUpperCase().replace(/[^A-Z0-9]/g, 'X') })); prev(e); };
  });
  assert.equal(got[0].type, 'auth_ok', JSON.stringify(got));
  assert.equal(got[0].user.username, name);
  const reply = got.find(m => m.type === 'club' || m.type === 'error');
  assert.equal(reply.type, 'club', JSON.stringify(reply));
  // leave so the throwaway club disappears
  await ws(s => { s.until = m => m.type === 'club_left' || m.type === 'error' || m.type === 'clubs';
    s.onmessage = (prev => e => { const m = JSON.parse(e.data); if (m.type === 'auth_ok') { s.send(JSON.stringify({ type: 'club_leave' })); s.send(JSON.stringify({ type: 'club_list' })); } prev(e); })(s.onmessage);
    s.send(JSON.stringify({ type: 'auth', token, clientType: 'browser' })); });
  const guest = await ws(s => { s.until = m => m.type === 'error';
    s.onmessage = (prev => e => { const m = JSON.parse(e.data); if (m.type === 'auth_ok') s.send(JSON.stringify({ type: 'club_create', name: 'Guest Club', tag: 'GC' })); prev(e); })(s.onmessage);
    s.send(JSON.stringify({ type: 'join', name: 'guesty', clientType: 'browser' })); });
  assert.match(guest.find(m => m.type === 'error').error, /Sign in/);
});

test('signing out revokes the device and its access tokens', async () => {
  let r = await api('logout', { method: 'POST', body: {} });
  assert.equal(r.status, 200); assert.match(r.set, /Max-Age=0/);
  r = await api('session', { body: {} });
  assert.equal(r.status, 401);
  r = await api('cloud', { auth: token, withCookie: false });
  assert.equal(r.status, 401);
  const got = await ws(s => s.send(JSON.stringify({ type: 'auth', token, clientType: 'browser' })));
  assert.ok(got.some(m => m.type === '_closed' && m.code === 1008), JSON.stringify(got));
});
