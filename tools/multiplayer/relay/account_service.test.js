'use strict';
// Real isolated Express HTTP requests + temporary account files; no deployed service or game data.
const test = require('node:test');
const assert = require('node:assert/strict');
const express = require('express');
const crypto = require('node:crypto');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const http = require('node:http');
const { once } = require('node:events');
const jwt = require('jsonwebtoken');
const { installAccountRoutes, atomicJson, readJson, activeSession, KdfLimiter, cloudPath } = require('./account_service');
const sha = bytes => crypto.createHash('sha256').update(bytes).digest('hex');
const row = (name = 'hdd/TDATA/profile.bin', bytes = Buffer.from([0, 255, 31, 97, 0])) => ({ path: name, encoding: 'base64', data: bytes.toString('base64'), sha256: sha(bytes) });
const snapshot = (revision = 0, files = [row()]) => ({ base_revision: revision, files, settings: { display: { scale: 1 }, enabled: true } });
async function until(condition) { for (let i = 0; i < 200; i++) { if (condition()) return; await new Promise(resolve => setTimeout(resolve, 5)); } assert.fail('Fixture condition timed out.'); }

async function fixture(t, options = {}) {
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'mc3-account-security-'));
  const db = { users: options.users || [] }, secret = crypto.randomBytes(48).toString('hex'); let failPersistence = false, parseCount = 0;
  const app = express();
  const small = express.json({ limit: '10kb' });
  app.use((req, res, next) => /^\/api\/cloud(?:\/|$)/.test(req.path) ? next() : small(req, res, next));
  const cloudParser = express.json({ limit: '24mb' });
  const makeToken = (user, sid) => jwt.sign({ id: user.id, username: user.username, sid }, secret, { expiresIn: '30m' });
  const verifyToken = token => {
    try {
      const claim = jwt.verify(token, secret, { algorithms: ['HS256'] });
      if (!claim || typeof claim !== 'object' || !/^[0-9a-f]{64}$/.test(claim.sid || '')) return null;
      return activeSession(readJson(path.join(directory, 'accounts-v1/devices', claim.sid + '.json'), null, 4096), claim.id) ? claim : null;
    } catch (_) { return null; }
  };
  const service = installAccountRoutes(app, { db, dataDirectory: directory,
    persist: candidate => { if (failPersistence) throw Error('Synthetic storage failure'); atomicJson(path.join(directory, 'database.json'), candidate); },
    makeToken, verifyToken, cloudJsonParser: (req, res, next) => { parseCount++; cloudParser(req, res, next); }, ...options });
  app.use((error, req, res, next) => res.status(error.type === 'entity.too.large' ? 413 : 500).json({ error: 'fixture error' }));
  const server = app.listen(0, '127.0.0.1'); await once(server, 'listening');
  t.after(() => new Promise(resolve => server.close(resolve)));
  const base = `http://127.0.0.1:${server.address().port}`;
  const request = async (route, { method = 'GET', body, cookie, token, authorization, origin, site, raw } = {}) => {
    const headers = {};
    if (cookie) headers.Cookie = cookie;
    if (token) headers.Authorization = 'Bearer ' + token;
    if (authorization !== undefined) headers.Authorization = authorization;
    if (origin !== null && (origin !== undefined || cookie)) headers.Origin = origin || base;
    if (site) headers['Sec-Fetch-Site'] = site;
    if (body !== undefined || raw !== undefined) headers['Content-Type'] = 'application/json';
    const response = await fetch(base + route, { method, headers, body: raw !== undefined ? raw : body === undefined ? undefined : JSON.stringify(body) });
    const data = await response.json(); return { status: response.status, data, cookie: (response.headers.get('set-cookie') || '').split(';')[0], headers: response.headers };
  };
  const register = async name => {
    const response = await request('/api/register', { method: 'POST', body: { username: name, password: 'password with whitespace ', remember: true } });
    assert.equal(response.status, 201); return { ...response.data, cookie: response.cookie, headers: response.headers, password: 'password with whitespace ' };
  };
  return { directory, db, service, request, register, base, makeToken, verifyToken, failPersistence: value => { failPersistence = value; }, parses: () => parseCount };
}

test('actual HTTP cloud files remain opaque/account-scoped with revision conflicts and immutable history', async t => {
  const f = await fixture(t), a = await f.register('AccountA'), b = await f.register('AccountB');
  const saved = await f.request('/api/cloud', { method: 'PUT', body: snapshot(), token: a.token }); assert.equal(saved.status, 200);
  const first = await f.request('/api/cloud', { token: a.token });
  assert.equal(first.data.account_id, a.user.id); assert.equal(first.data.revision, 1); assert.deepEqual(first.data.files[0], { ...row(), size: 5 });
  const empty = await f.request('/api/cloud', { token: b.token }); assert.deepEqual(empty.data.files, []); assert.equal(empty.data.account_id, b.user.id);
  const file = path.join(f.directory, 'accounts-v1', a.user.id, 'current.json'), hash = sha(fs.readFileSync(file));
  assert.equal((await f.request('/api/cloud', { method: 'PUT', body: snapshot(0), token: a.token })).status, 409);
  assert.equal(sha(fs.readFileSync(file)), hash);
  assert.equal((await f.request('/api/cloud', { method: 'PUT', body: { ...snapshot(), account_id: a.user.id }, token: b.token })).status, 200);
  assert.equal((await f.request('/api/cloud', { token: a.token })).data.account_id, a.user.id);
  const changed = snapshot(1, [row('hdd/TDATA/profile.bin', Buffer.from('second exact byte sequence'))]);
  const results = await Promise.all([f.request('/api/cloud', { method: 'PUT', body: changed, token: a.token }), f.request('/api/cloud', { method: 'PUT', body: changed, token: a.token })]);
  assert.deepEqual(results.map(r => r.status).sort(), [200, 409]);
  const history = readJson(path.join(f.directory, 'accounts-v1', a.user.id, 'history/1.json'), null);
  assert.equal(history.revision, 1); assert.equal(history.files[0].sha256, row().sha256);
});

test('explicit invalid Bearer never falls back to a valid remembered cookie; auth precedes cloud parsing', async t => {
  const f = await fixture(t), user = await f.register('AuthBoundary'); const before = f.parses();
  for (const authorization of ['Bearer invalid', 'Basic abc', 'Bearer ']) {
    assert.equal((await f.request('/api/cloud', { method: 'PUT', raw: '{bad json', cookie: user.cookie, authorization })).status, 401);
  }
  assert.equal((await f.request('/api/cloud', { method: 'PUT', raw: '{bad json' })).status, 401);
  assert.equal(f.parses(), before);
  assert.equal((await f.request('/api/cloud', { method: 'PUT', raw: '{bad json', token: user.token })).status, 400);
  assert.equal(f.parses(), before + 1);
  assert.equal((await f.request('/api/cloud', { cookie: user.cookie })).status, 200);
});

test('cookie mutations enforce exact origin, logout revokes cookie AND its device-bound access JWT', async t => {
  const f = await fixture(t), user = await f.register('CookieBoundary');
  assert.match(user.headers.get('set-cookie'), /HttpOnly/); assert.match(user.headers.get('set-cookie'), /SameSite=Lax/);
  for (const route of ['/api/login', '/api/register', '/api/session', '/api/logout']) {
    const body = { username: user.user.username, password: user.password };
    assert.equal((await f.request(route, { method: 'POST', cookie: user.cookie, origin: 'https://evil.example', body })).status, 403);
  }
  assert.equal((await f.request('/api/session', { method: 'POST', cookie: user.cookie, origin: null, body: {} })).status, 403);
  assert.equal((await f.request('/api/session', { method: 'POST', cookie: user.cookie, site: 'same-site', body: {} })).status, 403);
  assert.equal((await f.request('/api/cloud', { method: 'PUT', cookie: user.cookie, origin: 'https://evil.example', body: snapshot() })).status, 403);
  assert.equal((await f.request('/api/session', { method: 'POST', cookie: user.cookie, body: {} })).status, 200);
  const logout = await f.request('/api/logout', { method: 'POST', cookie: user.cookie, body: {} });
  assert.equal(logout.status, 200); assert.match(logout.headers.get('set-cookie'), /HttpOnly/); assert.match(logout.headers.get('set-cookie'), /Max-Age=0/);
  assert.equal((await f.request('/api/session', { method: 'POST', cookie: user.cookie, body: {} })).status, 401);
  assert.equal((await f.request('/api/cloud', { token: user.token })).status, 401);
  const other = await f.register('OtherDevice'); assert.equal((await f.request('/api/cloud', { token: other.token })).status, 200);
});

test('PUBLIC_ORIGIN supports a TLS frontend behind an internal HTTP host without trusting arbitrary origins', async t => {
  const f = await fixture(t, { publicOrigin: 'https://play.example' }), user = await f.register('ProxyOrigin');
  assert.equal((await f.request('/api/session', { method: 'POST', cookie: user.cookie, origin: 'https://play.example', body: {} })).status, 200);
  assert.equal((await f.request('/api/session', { method: 'POST', cookie: user.cookie, origin: f.base, body: {} })).status, 403);
  assert.equal((await f.request('/api/session', { method: 'POST', cookie: user.cookie, origin: 'https://play.example.evil', body: {} })).status, 403);
  assert.equal((await f.request('/api/cloud', { method: 'PUT', cookie: user.cookie, origin: 'https://play.example', body: snapshot() })).status, 200);
  assert.equal((await f.request('/api/cloud', { method: 'PUT', token: user.token, body: snapshot(1) })).status, 200);
});

test('reserved Windows names, aliases, collisions, hashes, nesting and file/byte bounds are rejected over HTTP', async t => {
  const f = await fixture(t), user = await f.register('PathBoundary');
  for (const name of ['hdd/../save', 'hdd/CON.bin', 'hdd/COM1', 'hdd/LPT\u00b9.dat', 'hdd/a:stream', 'config/a?.ini', 'hdd/a.', 'hdd/a\\b']) {
    assert.equal(cloudPath(name), false);
    assert.equal((await f.request('/api/cloud', { method: 'PUT', token: user.token, body: snapshot(0, [row(name)]) })).status, 400);
  }
  for (const files of [[row('hdd/a'), row('hdd/A')], [row('hdd/a'), row('hdd/a/b')], [row('hdd/a'), row('hdd/a-1'), row('hdd/a/b')], [{ ...row(), sha256: '0'.repeat(64) }], Array.from({ length: 257 }, (_, i) => row('hdd/' + i))])
    assert.equal((await f.request('/api/cloud', { method: 'PUT', token: user.token, body: snapshot(0, files) })).status, 400);
  let deep = {}; for (let i = 0; i < 18; i++) deep = { child: deep };
  assert.equal((await f.request('/api/cloud', { method: 'PUT', token: user.token, body: { ...snapshot(), settings: deep } })).status, 400);
  assert.equal((await f.request('/api/cloud', { method: 'PUT', token: user.token, body: { ...snapshot(), settings: { large: 'x'.repeat(128 * 1024) } } })).status, 413);
  const over = Buffer.alloc(16 * 1024 * 1024 + 1);
  assert.equal((await f.request('/api/cloud', { method: 'PUT', token: user.token, body: snapshot(0, [row('hdd/big', over)]) })).status, 413);
  assert.deepEqual((await f.request('/api/cloud', { token: user.token })).data.files, []);
});

test('password migration preserves credentials, salts differ, persistence failure publishes no phantom account', async t => {
  const legacy = { id: crypto.randomUUID(), username: 'Legacy', password_hash: crypto.pbkdf2Sync('legacy7', 'mc3-salt', 100000, 64, 'sha512').toString('hex') };
  const f = await fixture(t, { users: [legacy] });
  const migrated = await f.request('/api/login', { method: 'POST', body: { username: 'Legacy', password: 'legacy7' } });
  assert.equal(migrated.status, 200); assert.match(f.db.users[0].password_hash, /^scrypt-v1\$/);
  const a = await f.register('SaltA'), b = await f.register('SaltB');
  assert.notEqual(f.db.users.find(u => u.id === a.user.id).password_hash, f.db.users.find(u => u.id === b.user.id).password_hash);
  assert.equal((await f.request('/api/login', { method: 'POST', body: { username: 'SaltA', password: a.password.trim() } })).status, 401);
  const count = f.db.users.length, devices = fs.readdirSync(path.join(f.directory, 'accounts-v1/devices')).length;
  f.failPersistence(true);
  assert.equal((await f.request('/api/register', { method: 'POST', body: { username: 'NoPhantom', password: 'valid-password' } })).status, 500);
  assert.equal(f.db.users.length, count); assert.equal(fs.readdirSync(path.join(f.directory, 'accounts-v1/devices')).length, devices);
  f.failPersistence(false);
  assert.equal((await f.request('/api/register', { method: 'POST', body: { username: 'NoPhantom', password: 'valid-password' } })).status, 201);
  const duplicate = await Promise.all([f.request('/api/register', { method: 'POST', body: { username: 'RaceName', password: 'valid-password' } }), f.request('/api/register', { method: 'POST', body: { username: 'racename', password: 'valid-password' } })]);
  assert.deepEqual(duplicate.map(r => r.status).sort(), [201, 409]);
});

test('bounded password work rejects excess HTTP operations without allocating an unbounded crypto queue', async t => {
  const limiter = new KdfLimiter(1, 1); let release;
  const held = limiter.run(() => new Promise(resolve => { release = resolve; }));
  await Promise.resolve();
  const f = await fixture(t, { kdfLimiter: limiter });
  t.after(() => { if (release) release(); });
  const pending = f.request('/api/register', { method: 'POST', body: { username: 'Queued', password: 'valid-password' } });
  for (let i = 0; i < 100 && limiter.queue.length !== 1; i++) await new Promise(resolve => setTimeout(resolve, 5));
  assert.equal(limiter.active, 1); assert.equal(limiter.queue.length, 1); assert.equal(f.db.users.length, 0);
  assert.equal((await f.request('/api/register', { method: 'POST', body: { username: 'Rejected', password: 'valid-password' } })).status, 429);
  release(); await held; assert.equal((await pending).status, 201);
  await new Promise(resolve => setImmediate(resolve)); assert.equal(limiter.active, 0); assert.equal(limiter.queue.length, 0);
});

test('held partial HTTP bodies are bounded before parsing globally and per account; abort/error/finish release exactly once', async t => {
  const f = await fixture(t), users = [];
  for (let i = 0; i < 5; i++) users.push(await f.register('TransferUser' + i));
  const held = [];
  t.after(() => { for (const request of held) request.destroy(); });
  const hold = token => {
    const request = http.request(f.base + '/api/cloud', { method: 'PUT', headers: {
      Authorization: 'Bearer ' + token, 'Content-Type': 'application/json', 'Content-Length': '1000000'
    } });
    request.on('error', () => {}); request.flushHeaders(); request.write('{"base_revision":'); held.push(request); return request;
  };
  hold(users[0].token); await until(() => f.service.cloudActivity().active === 1);
  const parsed = f.parses();
  assert.equal((await f.request('/api/cloud', { method: 'PUT', raw: '{not JSON', token: users[0].token })).status, 429);
  assert.equal(f.parses(), parsed); // Rejected before touching the body parser, despite a valid access token.
  for (let i = 1; i < 4; i++) hold(users[i].token);
  await until(() => f.service.cloudActivity().active === 4);
  const fullParses = f.parses();
  assert.equal((await f.request('/api/cloud', { method: 'PUT', raw: '{not JSON', token: users[4].token })).status, 429);
  assert.equal(f.parses(), fullParses); assert.deepEqual(f.service.cloudActivity(), { active: 4, accounts: 4 });
  held[0].destroy(); await until(() => f.service.cloudActivity().active === 3);
  assert.equal((await f.request('/api/cloud', { method: 'PUT', body: snapshot(), token: users[0].token })).status, 200);
  assert.deepEqual(f.service.cloudActivity(), { active: 3, accounts: 3 });
  for (const request of held) request.destroy(); await until(() => f.service.cloudActivity().active === 0);
  assert.equal((await f.request('/api/cloud', { method: 'PUT', raw: '{not JSON', token: users[0].token })).status, 400);
  assert.deepEqual(f.service.cloudActivity(), { active: 0, accounts: 0 });
  assert.equal((await f.request('/api/cloud', { token: users[0].token })).status, 200);
  assert.deepEqual(f.service.cloudActivity(), { active: 0, accounts: 0 });
});

test('a transfer deadline destroys a held parser before releasing its budget', async t => {
  const f = await fixture(t, { limits: { cloudRequestMs: 100 } }), user = await f.register('DeadlineUser');
  const request = http.request(f.base + '/api/cloud', { method: 'PUT', headers: {
    Authorization: 'Bearer ' + user.token, 'Content-Type': 'application/json', 'Content-Length': '1000000'
  } });
  request.on('error', () => {}); t.after(() => request.destroy());
  request.flushHeaders(); request.write('{"base_revision":');
  await until(() => f.service.cloudActivity().active === 1);
  await until(() => f.service.cloudActivity().active === 0);
  assert.equal(request.destroyed, true);
  assert.deepEqual(f.service.cloudActivity(), { active: 0, accounts: 0 });
  assert.equal((await f.request('/api/cloud', { token: user.token })).status, 200);
});

test('device, account, history, global storage and bounded record reads fail closed without deleting cloud data', async t => {
  const f = await fixture(t, { limits: { sessionsPerUser: 1, historyFiles: 1 } }), user = await f.register('QuotaUser');
  assert.equal((await f.request('/api/login', { method: 'POST', body: { username: user.user.username, password: user.password } })).status, 429);
  assert.equal((await f.request('/api/cloud', { method: 'PUT', token: user.token, body: snapshot() })).status, 200);
  assert.equal((await f.request('/api/cloud', { method: 'PUT', token: user.token, body: snapshot(1) })).status, 200);
  assert.equal((await f.request('/api/cloud', { method: 'PUT', token: user.token, body: snapshot(2) })).status, 413);
  assert.equal((await f.request('/api/cloud', { token: user.token })).data.revision, 2);
  const global = await fixture(t, { limits: { totalCloudBytes: 1024 } }), a = await global.register('GlobalA'), b = await global.register('GlobalB');
  const bytes = Buffer.alloc(256);
  assert.equal((await global.request('/api/cloud', { method: 'PUT', token: a.token, body: snapshot(0, [row('hdd/raw', bytes)]) })).status, 200);
  assert.equal((await global.request('/api/cloud', { method: 'PUT', token: b.token, body: snapshot(0, [row('hdd/raw', bytes)]) })).status, 413);
  const small = await fixture(t, { limits: { cloudBytes: 512 } }), c = await small.register('BoundedRead');
  const folder = path.join(small.directory, 'accounts-v1', c.user.id); fs.mkdirSync(folder);
  const file = path.join(folder, 'current.json'); fs.writeFileSync(file, ' '.repeat(513)); const before = fs.readFileSync(file);
  assert.equal((await small.request('/api/cloud', { token: c.token })).status, 500); assert.deepEqual(fs.readFileSync(file), before);
  const devices = path.join(small.directory, 'accounts-v1/devices'), session = path.join(devices, sha(c.cookie.split('=')[1]) + '.json');
  fs.writeFileSync(session, ' '.repeat(4097));
  assert.equal((await small.request('/api/session', { method: 'POST', cookie: c.cookie, body: {} })).status, 500);
  assert.equal(fs.statSync(session).size, 4097);
  const users = await fixture(t, { limits: { users: 1 } }); await users.register('OnlyUser');
  assert.equal((await users.request('/api/register', { method: 'POST', body: { username: 'OverUserLimit', password: 'valid-password' } })).status, 413);
  assert.equal(users.db.users.length, 1);
});
