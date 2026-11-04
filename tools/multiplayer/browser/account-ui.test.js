'use strict';
const test = require('node:test');
const assert = require('node:assert/strict');
const { webcrypto } = require('node:crypto');
const { Client, DraftVault, canonicalPath, importPath, encodeFile, validateCloud, base64ToBytes, resolveEndpoints, scopedKey, MAX_BYTES } = require('./account-ui.js');
const A = '11111111-1111-4111-8111-111111111111', B = '22222222-2222-4222-8222-222222222222';
const copy = value => JSON.parse(JSON.stringify(value));
const response = (body, status = 200) => ({ ok: status >= 200 && status < 300, status, async json() { return copy(body); } });
const session = (id = A, token = 'memory-token') => ({ token, user: { id, username: id === A ? 'Player One' : 'Player Two' }, remembered: true });
function storage() { const data = new Map(); return { data, getItem: key => data.get(key) || null, setItem: (key, value) => data.set(key, value), removeItem: key => data.delete(key) }; }
function vault() { const data = new Map(); return { data, async save(key, value) { data.set(key, copy(value)); return true; }, async load(key) { return data.has(key) ? copy(data.get(key)) : null; } }; }
function client(fetch, options = {}) { return new Client({ apiBase: 'https://game.example', fetch, crypto: webcrypto, storage: storage(), vault: vault(), ...options }); }
function empty(id = A, revision = 0) { return { schema: 1, account_id: id, revision, files: [], settings: { native: { untouched: true } }, updated_at: null }; }

test('credential API origin is independent of untrusted relay query', () => {
  const endpoints = resolveEndpoints('https://game.example/play?relay=wss://evil.example/grab&stream=ws://evil.example:8766');
  assert.equal(endpoints.api, 'https://game.example'); assert.equal(endpoints.relay, 'wss://game.example'); assert.equal(endpoints.stream, 'wss://game.example:8766');
  assert.equal(resolveEndpoints('http://localhost:8080/?relay=ws://127.0.0.1:9000').relay, 'ws://127.0.0.1:9000/');
  assert.equal(resolveEndpoints('https://game.example/?relay=wss://name:password@game.example').relay, 'wss://game.example');
  assert.equal(resolveEndpoints('file:///C:/MC3/index.html').api, 'http://localhost:8080');
});
test('portable save/config paths reject traversal, devices and alternate streams', () => {
  for (const name of ['hdd/UData/profile.dat', 'settings.toml', 'settings.ini', 'config/mc3_enhancements.toml']) assert.equal(canonicalPath(name), true);
  for (const name of ['../settings.toml', '/hdd/file', 'hdd/a:b', 'hdd/CON.dat', 'hdd/foo.', 'hdd/a\\b', 'config/secret.exe', 'assets/default.xbe', 'hdd/a/../../x']) assert.equal(canonicalPath(name), false);
  assert.equal(importPath({ name: 'Player1', webkitRelativePath: 'backup/hdd7/UData/Player1' }), 'hdd/UData/Player1');
  assert.equal(importPath({ name: 'settings.toml' }), 'settings.toml');
});
test('original byte roundtrip verifies SHA256 and rejects altered/cross-account snapshots', async () => {
  const raw = new Uint8Array([0, 255, 2, 0, 10, 13, 192, 128]);
  const encoded = await encodeFile('hdd/UData/profile', raw, webcrypto);
  assert.deepEqual(base64ToBytes(encoded.data), raw);
  const value = empty(); value.files.push(encoded);
  const verified = await validateCloud(value, A, webcrypto); assert.deepEqual(base64ToBytes(verified.files[0].data), raw);
  await assert.rejects(validateCloud(value, B, webcrypto), /belong/);
  const wrong = copy(value); wrong.files[0].sha256 = '0'.repeat(64); await assert.rejects(validateCloud(wrong, A, webcrypto), /integrity/);
  const duplicate = copy(value); duplicate.files.push({ ...encoded, path: 'hdd/UData/PROFILE' }); await assert.rejects(validateCloud(duplicate, A, webcrypto), /duplicate/);
  assert.throws(() => base64ToBytes('YQ=A'), /encoding|character/i);
  await assert.rejects(encodeFile('hdd/too-big', new Uint8Array(MAX_BYTES + 1), webcrypto), /16 MiB/);
});
test('signup uses explicit mode, exact untrimmed password, confirmation and cookie remember', async () => {
  const calls = [], local = storage(); local.setItem('mc3_token', 'obsolete-secret');
  const instance = client(async (url, options) => { calls.push({ url, options }); return response(session()); }, { storage: local });
  await assert.rejects(instance.authenticate('register', 'Player One', 'abcdefgh ', true, 'abcdefgh'), /confirmation/);
  assert.equal(calls.length, 0);
  await instance.authenticate('register', ' Player One ', '  abcdefgh  ', true, '  abcdefgh  ');
  assert.equal(calls[0].url, 'https://game.example/api/register');
  assert.deepEqual(JSON.parse(calls[0].options.body), { username: 'Player One', password: '  abcdefgh  ', remember: true });
  assert.equal(calls[0].options.credentials, 'include');
  assert.equal(local.getItem('mc3_token'), null);
  assert.equal([...local.data.values()].some(value => value.includes('memory-token') || value.includes('abcdefgh')), false);
});
test('preferences are scoped by API origin and account identity', () => {
  const local = storage(), instance = client(async () => response({}), { storage: local });
  instance.applySession(session(A)); instance.updatePreferences({ deadzone: .3 });
  instance.applySession(session(B)); assert.equal(instance.preferences.deadzone, .15); instance.updatePreferences({ deadzone: .4 });
  instance.applySession(session(A)); assert.equal(instance.preferences.deadzone, .3);
  assert.notEqual(scopedKey('https://game.example', A), scopedKey('https://other.example', A));
  assert.notEqual(scopedKey('https://game.example', A), scopedKey('https://game.example', B));
  clearTimeout(instance.syncTimer);
});
test('401 refresh is single-flight and uses cookie without stale Bearer', async () => {
  let sessions = 0, requests = 0;
  const instance = client(async (url, options) => {
    if (url.endsWith('/session')) { ++sessions; assert.equal(options.headers.Authorization, undefined); await new Promise(resolve => setTimeout(resolve, 5)); return response(session(A, 'renewed')); }
    ++requests; return options.headers.Authorization === 'Bearer renewed' ? response({ value: 'ok' }) : response({ error: 'Expired' }, 401);
  });
  instance.applySession(session());
  const values = await Promise.all([instance.request('cloud'), instance.request('cloud')]);
  assert.equal(sessions, 1); assert.equal(requests, 4); assert.deepEqual(values, [{ value: 'ok' }, { value: 'ok' }]);
});
test('401 recovery never replays a prior-account mutation into another account', async () => {
  let writes = 0;
  const instance = client(async (url) => {
    if (url.endsWith('/session')) return response(session(B, 'other-account'));
    ++writes; return response({ error: 'Expired' }, 401);
  });
  instance.applySession(session(A));
  await assert.rejects(instance.request('cloud', 'PUT', { base_revision: 0, files: [], settings: {} }), /account changed/);
  assert.equal(writes, 1); assert.equal(instance.session.user.id, B);
});
test('late remembered session cannot undo a newer explicit sign-in', async () => {
  let resolveSession;
  const instance = client(async url => url.endsWith('/session') ? new Promise(resolve => { resolveSession = resolve; }) : response(session(B)));
  const refresh = instance.refresh(); await instance.authenticate('login', 'Player Two', 'password', false);
  resolveSession(response(session(A))); assert.equal(await refresh, null); assert.equal(instance.session.user.id, B);
});
test('preference debounce sync does not upload imported files before explicit Save', async () => {
  let server = empty(), lastPut;
  const instance = client(async (url, options) => {
    if (options.method === 'PUT') {
      lastPut = JSON.parse(options.body); assert.equal(lastPut.base_revision, server.revision);
      server = { ...server, files: lastPut.files, settings: lastPut.settings, revision: server.revision + 1 };
      return response({ success: true, revision: server.revision, updated_at: '2026-11-04T00:00:00Z' });
    }
    return response(server);
  });
  instance.applySession(session()); await instance.loadCloud();
  const raw = new Uint8Array([10, 255, 0, 81]);
  await instance.importFiles([{ name: 'Player1', size: raw.length, async arrayBuffer() { return raw.buffer; } }]);
  assert.equal(server.files.length, 0); assert.equal(instance.dirty, true);
  instance.updatePreferences({ deadzone: .27 }); clearTimeout(instance.syncTimer); await instance.saveCloud({ settingsOnly: true });
  assert.equal(lastPut.files.length, 0); assert.equal(instance.draft.files.length, 1); assert.equal(instance.dirty, true);
  assert.deepEqual(lastPut.settings.native, { untouched: true });
  await instance.saveCloud(); assert.equal(lastPut.files.length, 1); assert.equal(instance.dirty, false);
  assert.deepEqual(base64ToBytes(server.files[0].data), raw);
});
test('409 retains both versions and requires a deliberate conflict choice', async () => {
  let server = empty(), writes = 0;
  const retained = vault();
  const instance = client(async (_url, options) => {
    if (options.method === 'PUT') {
      ++writes; const body = JSON.parse(options.body);
      if (body.base_revision !== server.revision) return response({ error: 'Another device changed this account.' }, 409);
      server = { ...server, files: body.files, settings: body.settings, revision: server.revision + 1 };
      return response({ revision: server.revision });
    }
    return response(server);
  }, { vault: retained });
  instance.applySession(session()); await instance.loadCloud(); instance.updatePreferences({ deadzone: .29 }); clearTimeout(instance.syncTimer);
  server = { ...server, revision: 1, settings: { browser: { deadzone: .42 }, native: { untouched: 'other device' } } };
  await assert.rejects(instance.saveCloud(), error => error.status === 409);
  assert.equal(writes, 1); assert.equal(instance.conflict.local.settings.browser.deadzone, .29); assert.equal(instance.conflict.remote.settings.browser.deadzone, .42);
  await assert.rejects(instance.saveCloud(), /Choose/); assert.equal(writes, 1);
  await instance.useCloud(); assert.equal(instance.preferences.deadzone, .42); assert.equal(instance.retained[0].local.settings.browser.deadzone, .29);
  assert.equal(retained.data.has(scopedKey('https://game.example', A, 'conflict')), true);
});
test('local conflict cannot be discarded when durable storage is unavailable', async () => {
  const instance = client(async () => response(empty()), { vault: new DraftVault(null) });
  instance.applySession(session()); await instance.loadCloud();
  instance.conflict = { local: empty(), remote: empty(A, 1) };
  await assert.rejects(instance.useCloud(), /only in memory/);
  assert.equal(instance.backup().retained_conflicts.length, 1); assert.ok(instance.conflict);
});
test('a late cloud fetch cannot populate another account UI', async () => {
  let finish;
  const instance = client(async () => new Promise(resolve => { finish = resolve; }));
  instance.applySession(session(A)); const loading = instance.loadCloud(); instance.applySession(session(B)); finish(response(empty(A)));
  assert.equal(await loading, null); assert.equal(instance.cloud, null); assert.equal(instance.draft, null);
});
