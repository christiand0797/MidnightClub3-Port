// Accounts and cloud saves for the Cloudflare World, in the Durable Object's own SQLite. Same HTTP contract as
// relay/account_service.js (register, login, session, logout, GET/PUT cloud), so browser/account-ui.js and the
// launcher's cloud snapshot work against mc3.top unchanged. Pure Web APIs: also runs under Node for tests.
//
// - Passwords: PBKDF2-SHA256 (WebCrypto's ceiling of 100,000 iterations), per-user salt.
// - Devices: an HttpOnly mc3_device cookie; only its SHA-256 is stored. Sign-out revokes it.
// - Access tokens: short-lived HMAC tokens naming the device session; a revoked device stops its tokens.
// - Cloud: opaque save/config bytes, never edited. Stored in chunks (SQLite values cap at 2 MB), current + one
//   previous revision kept, optimistic base_revision checks as the Node service.

const MAX_FILE_BYTES = 16 * 1024 * 1024;       // per snapshot, decoded
const MAX_BODY_BYTES = 24 * 1024 * 1024;       // per request, JSON
const CHUNK = 512 * 1024;                      // characters per stored chunk
const LIMITS = { users: 10000, sessionsPerUser: 32, totalCloudBytes: 3 * 1024 * 1024 * 1024 };
const ACCESS_MS = 30 * 60e3, REMEMBER_MS = 30 * 86400e3, SHORT_MS = 12 * 3600e3;
const ITERATIONS = 100000;
const COOKIE = 'mc3_device';
const NAME = /^[A-Za-z0-9_ -]{3,20}$/;
const enc = new TextEncoder();

export class HttpError extends Error { constructor(status, message) { super(message); this.status = status; } }
const fail = (status, message) => { throw new HttpError(status, message); };
const plain = v => v !== null && typeof v === 'object' && !Array.isArray(v) && Object.getPrototypeOf(v) === Object.prototype;
const hex = bytes => [...new Uint8Array(bytes)].map(b => b.toString(16).padStart(2, '0')).join('');
const unhex = s => Uint8Array.from(s.match(/../g) || [], h => parseInt(h, 16));
const b64url = bytes => btoa(String.fromCharCode(...new Uint8Array(bytes))).replace(/\+/g, '-').replace(/\//g, '_').replace(/=+$/, '');
const fromB64url = s => Uint8Array.from(atob(s.replace(/-/g, '+').replace(/_/g, '/') + '='.repeat((4 - s.length % 4) % 4)), c => c.charCodeAt(0));
const sha256 = async data => hex(await crypto.subtle.digest('SHA-256', typeof data === 'string' ? enc.encode(data) : data));
const random = n => crypto.getRandomValues(new Uint8Array(n));
function equal(a, b) { if (a.length !== b.length) return false; let d = 0; for (let i = 0; i < a.length; i++) d |= a.charCodeAt(i) ^ b.charCodeAt(i); return d === 0; }

async function pbkdf2(password, salt) {
  const key = await crypto.subtle.importKey('raw', enc.encode(password), 'PBKDF2', false, ['deriveBits']);
  return hex(await crypto.subtle.deriveBits({ name: 'PBKDF2', hash: 'SHA-256', salt, iterations: ITERATIONS }, key, 256));
}
export async function passwordHash(password) {
  const salt = random(16);
  return `pbkdf2-sha256-v1$${ITERATIONS}$${hex(salt)}$${await pbkdf2(password, salt)}`;
}
export async function passwordMatches(password, stored) {
  const m = /^pbkdf2-sha256-v1\$(\d+)\$([0-9a-f]{32})\$([0-9a-f]{64})$/.exec(stored || '');
  if (!m || Number(m[1]) !== ITERATIONS) return false;
  return equal(await pbkdf2(password, unhex(m[2])), m[3]);
}
const DUMMY = 'pbkdf2-sha256-v1$100000$00000000000000000000000000000000$' + '0'.repeat(64);

// Portable virtual paths (as account_service.js cloudPath).
export function cloudPath(value) {
  if (typeof value !== 'string' || value.length > 180 || /[\\:\x00-\x1f<>"|?*]/.test(value) || value.normalize('NFC') !== value ||
      /[\uD800-\uDFFF]/.test(value.replace(/[\uD800-\uDBFF][\uDC00-\uDFFF]/g, ''))) return false;
  const parts = value.split('/');
  if (parts.some(p => !p || p === '.' || p === '..' || /[. ]$/.test(p) || /^(?:CON|PRN|AUX|NUL|CLOCK\$|CONIN\$|CONOUT\$|(?:COM|LPT)[1-9¹²³])(?:\.|$)/i.test(p))) return false;
  if (value === 'settings.toml' || value === 'settings.ini') return true;
  if (parts[0] === 'config') return parts.length <= 4 && /\.(toml|ini|json|cfg)$/i.test(value);
  return parts[0] === 'hdd' && parts.length <= 8 && parts.length >= 2;
}

export async function validateSnapshot(body) {
  if (!plain(body) || !Number.isSafeInteger(body.base_revision) || body.base_revision < 0) fail(400, 'A cloud base revision is required.');
  if (!Array.isArray(body.files) || body.files.length > 256 || !plain(body.settings)) fail(400, 'Invalid save/config snapshot.');
  const pending = [{ value: body.settings, depth: 0 }]; let nodes = 0;
  while (pending.length) {
    const { value, depth } = pending.pop();
    if (++nodes > 8192 || depth > 16) fail(400, 'Settings nesting exceeds its bound.');
    if (value === null || typeof value === 'boolean' || typeof value === 'string') continue;
    if (typeof value === 'number') { if (!Number.isFinite(value)) fail(400, 'Settings contain an invalid number.'); continue; }
    if (!Array.isArray(value) && !plain(value)) fail(400, 'Settings contain unsupported values.');
    for (const key of Object.keys(value)) {
      if (key.length > 128 || ['__proto__', 'constructor', 'prototype'].includes(key)) fail(400, 'Settings contain an unsafe key.');
      pending.push({ value: value[key], depth: depth + 1 });
    }
  }
  if (enc.encode(JSON.stringify(body.settings)).length > 128 * 1024) fail(413, 'Settings exceed the cloud limit.');
  const names = new Set(); let total = 0; const files = [];
  for (const file of body.files) {
    if (!plain(file) || !cloudPath(file.path) || typeof file.data !== 'string' || file.encoding !== 'base64' ||
        !/^[0-9a-f]{64}$/.test(file.sha256 || '')) fail(400, 'Invalid cloud file path, encoding or digest.');
    const key = file.path.toLowerCase(); if (names.has(key)) fail(400, 'Duplicate cloud file path.'); names.add(key);
    if (file.data.length > Math.ceil(MAX_FILE_BYTES / 3) * 4 || file.data.length % 4 || /[^A-Za-z0-9+/=]/.test(file.data)) fail(400, 'Invalid file encoding.');
    let bytes;
    try { bytes = Uint8Array.from(atob(file.data), c => c.charCodeAt(0)); } catch (_) { fail(400, 'Invalid file encoding.'); }
    let canonical = ''; for (let i = 0; i < bytes.length; i += 0x8000) canonical += String.fromCharCode(...bytes.subarray(i, i + 0x8000));
    if (btoa(canonical) !== file.data) fail(400, 'File encoding is not canonical base64.');
    total += bytes.length; if (total > MAX_FILE_BYTES) fail(413, 'Save/config files exceed 16 MiB.');
    if (await sha256(bytes) !== file.sha256) fail(400, 'Cloud file digest mismatch.');
    files.push({ path: file.path, encoding: 'base64', data: file.data, sha256: file.sha256, size: bytes.length });
  }
  for (const name of names) {
    let parent = name;
    while (parent.includes('/')) { parent = parent.slice(0, parent.lastIndexOf('/')); if (names.has(parent)) fail(400, 'Cloud paths conflict as files and directories.'); }
  }
  return { files, settings: body.settings, bytes: total };
}

export class Accounts {
  // sql: a DurableObjectStorage.sql (or a compatible stub). now: clock for tests.
  constructor(sql, { now = () => Date.now() } = {}) {
    this.sql = sql; this.now = now; this.attempts = new Map();
    sql.exec(`CREATE TABLE IF NOT EXISTS meta(k TEXT PRIMARY KEY, v TEXT NOT NULL)`);
    sql.exec(`CREATE TABLE IF NOT EXISTS users(id TEXT PRIMARY KEY, name TEXT NOT NULL, name_lc TEXT NOT NULL UNIQUE,
      hash TEXT NOT NULL, created INTEGER NOT NULL, last_seen INTEGER)`);
    sql.exec(`CREATE TABLE IF NOT EXISTS sessions(sid TEXT PRIMARY KEY, user_id TEXT NOT NULL, created INTEGER NOT NULL,
      expires INTEGER NOT NULL, remember INTEGER NOT NULL, revoked INTEGER NOT NULL DEFAULT 0)`);
    sql.exec(`CREATE INDEX IF NOT EXISTS sessions_user ON sessions(user_id)`);
    sql.exec(`CREATE TABLE IF NOT EXISTS cloud_head(user_id TEXT NOT NULL, revision INTEGER NOT NULL, parts INTEGER NOT NULL,
      bytes INTEGER NOT NULL, updated_at TEXT NOT NULL, PRIMARY KEY(user_id, revision))`);
    sql.exec(`CREATE TABLE IF NOT EXISTS cloud_part(user_id TEXT NOT NULL, revision INTEGER NOT NULL, part INTEGER NOT NULL,
      data TEXT NOT NULL, PRIMARY KEY(user_id, revision, part))`);
  }
  one(query, ...args) { return this.sql.exec(query, ...args).toArray()[0] || null; }
  all(query, ...args) { return this.sql.exec(query, ...args).toArray(); }

  async hmacKey() {
    if (!this.key) {
      let row = this.one(`SELECT v FROM meta WHERE k = 'hmac'`);
      if (!row) { row = { v: hex(random(32)) }; this.sql.exec(`INSERT OR IGNORE INTO meta(k, v) VALUES ('hmac', ?)`, row.v); row = this.one(`SELECT v FROM meta WHERE k = 'hmac'`); }
      this.key = await crypto.subtle.importKey('raw', unhex(row.v), { name: 'HMAC', hash: 'SHA-256' }, false, ['sign', 'verify']);
    }
    return this.key;
  }
  async makeToken(user, sid) {
    const body = b64url(enc.encode(JSON.stringify({ id: user.id, username: user.name, sid, exp: this.now() + ACCESS_MS })));
    return body + '.' + b64url(await crypto.subtle.sign('HMAC', await this.hmacKey(), enc.encode(body)));
  }
  // A valid, unexpired token whose device session is still active -> { id, username, sid }; otherwise null.
  async verifyToken(token) {
    if (typeof token !== 'string' || token.length > 2048 || !/^[A-Za-z0-9_-]+\.[A-Za-z0-9_-]+$/.test(token)) return null;
    const [body, sig] = token.split('.');
    try {
      if (!await crypto.subtle.verify('HMAC', await this.hmacKey(), fromB64url(sig), enc.encode(body))) return null;
      const p = JSON.parse(new TextDecoder().decode(fromB64url(body)));
      if (!plain(p) || typeof p.id !== 'string' || typeof p.sid !== 'string' || !(p.exp > this.now())) return null;
      if (!this.activeSession(p.sid, p.id)) return null;
      const user = this.one(`SELECT id, name FROM users WHERE id = ?`, p.id);
      return user ? { id: user.id, username: user.name, sid: p.sid } : null;
    } catch (_) { return null; }
  }
  activeSession(sid, userId) {
    const s = this.one(`SELECT user_id, expires, revoked FROM sessions WHERE sid = ?`, sid);
    return !!s && !s.revoked && s.expires > this.now() && (userId === undefined || s.user_id === userId) ? s : null;
  }

  // ---------- HTTP ----------
  // Returns a Response for /api/register|login|session|logout|cloud, or null for other paths.
  async handle(request) {
    const url = new URL(request.url);
    const route = url.pathname.replace(/^\/api\//, '');
    if (!['register', 'login', 'session', 'logout', 'cloud', 'profile', 'account'].includes(route)) return null;
    // Workers fault if a response goes out while the request body is unread; drain whatever a route didn't read.
    const drain = async () => { if (request.body && !request.bodyUsed) await request.arrayBuffer().catch(() => {}); };
    try {
      const headers = { 'Cache-Control': 'no-store' };
      const result = await this.route(route, request, url, headers);
      await drain();
      return Response.json(result.body, { status: result.status || 200, headers });
    } catch (error) {
      await drain();
      const status = error instanceof HttpError ? error.status : 500;
      if (status === 500) console.error('account error', error && error.stack || error);
      return Response.json({ error: status === 500 ? 'Account storage is temporarily unavailable.' : error.message },
        { status, headers: { 'Cache-Control': 'no-store', ...(status === 429 ? { 'Retry-After': '30' } : {}) } });
    }
  }

  sameOrigin(request, url) {
    const site = request.headers.get('Sec-Fetch-Site'), origin = request.headers.get('Origin');
    if (site !== null && site !== 'same-origin' && site !== 'none') fail(403, 'Account changes require the same origin.');
    if (origin === null) {
      if ((request.headers.get('Cookie') || '').includes(COOKIE + '=') && site !== 'same-origin') fail(403, 'Cookie-authenticated changes require an Origin header.');
      return;
    }
    if (origin !== url.origin) fail(403, 'Account changes require the same origin.');
  }
  cookieToken(request) {
    const header = request.headers.get('Cookie') || '';
    if (header.length > 8192) fail(400, 'Cookie header exceeds its limit.');
    const m = header.match(/(?:^|;\s*)mc3_device=([A-Za-z0-9_-]{43})(?:;|$)/);
    return m ? m[1] : '';
  }
  async fromCookie(request) {
    const token = this.cookieToken(request); if (!token) return null;
    const sid = await sha256(token), s = this.activeSession(sid);
    if (!s) return null;
    const user = this.one(`SELECT id, name FROM users WHERE id = ?`, s.user_id);
    return user ? { user, sid, token, remember: !!this.one(`SELECT remember FROM sessions WHERE sid = ?`, sid).remember } : null;
  }
  async authenticate(request) {
    const header = request.headers.get('Authorization');
    if (header !== null) {
      const m = header.length <= 4096 && /^Bearer ([A-Za-z0-9_.-]+)$/.exec(header);
      const v = m && await this.verifyToken(m[1]);
      if (!v) fail(401, 'Invalid or expired access token.');   // never falls back to the cookie
      return { user: { id: v.id, name: v.username }, method: 'bearer' };
    }
    const saved = await this.fromCookie(request);
    if (!saved) fail(401, 'Sign in to access your cloud files.');
    return { user: saved.user, method: 'cookie' };
  }
  // Per-address and per-name throttle for password attempts (in memory; resets when the object sleeps).
  throttle(request, name) {
    const now = this.now(), keys = ['ip:' + (request.headers.get('CF-Connecting-IP') || 'unknown'), 'n:' + name.toLowerCase()];
    if (this.attempts.size > 4096) for (const [k, v] of this.attempts) if (now - v.at > 600e3) this.attempts.delete(k);
    for (const k of keys) {
      const a = this.attempts.get(k) || { n: 0, at: now };
      if (now - a.at > 600e3) { a.n = 0; a.at = now; }
      if (++a.n > (k.startsWith('ip:') ? 30 : 10)) { this.attempts.set(k, a); fail(429, 'Too many sign-in attempts. Wait a few minutes and try again.'); }
      this.attempts.set(k, a);
    }
  }
  async body(request, limit = 65536) {
    const len = Number(request.headers.get('Content-Length') || 0);
    if (len > limit) fail(413, 'Request exceeds its limit.');
    const text = await request.text();
    if (enc.encode(text).length > limit) fail(413, 'Request exceeds its limit.');
    try { return JSON.parse(text); } catch (_) { fail(400, 'Malformed JSON request.'); }
  }
  async newDevice(user, remember, url, headers) {
    const now = this.now();
    this.sql.exec(`DELETE FROM sessions WHERE user_id = ? AND (revoked = 1 OR expires <= ?)`, user.id, now);
    if (this.one(`SELECT COUNT(*) AS n FROM sessions WHERE user_id = ?`, user.id).n >= LIMITS.sessionsPerUser) {
      // Oldest device signs out first rather than locking the player out.
      this.sql.exec(`DELETE FROM sessions WHERE sid = (SELECT sid FROM sessions WHERE user_id = ? ORDER BY created LIMIT 1)`, user.id);
    }
    const token = b64url(random(32)), sid = await sha256(token);
    this.sql.exec(`INSERT INTO sessions(sid, user_id, created, expires, remember) VALUES (?, ?, ?, ?, ?)`,
      sid, user.id, now, now + (remember ? REMEMBER_MS : SHORT_MS), remember ? 1 : 0);
    const secure = url.protocol === 'https:' ? '; Secure' : '';
    headers['Set-Cookie'] = `${COOKIE}=${token}; Path=/; HttpOnly; SameSite=Lax${secure}${remember ? '; Max-Age=2592000' : ''}`;
    return { token: await this.makeToken(user, sid), user: { id: user.id, username: user.name, rating: 1000 }, remembered: remember };
  }

  async route(route, request, url, headers) {
    const method = request.method;
    if (route === 'register' && method === 'POST') {
      this.sameOrigin(request, url);
      const b = await this.body(request);
      if (!plain(b)) fail(400, 'A JSON object is required.');
      const { username, password, remember = true } = b;
      if (typeof username !== 'string' || !NAME.test(username.trim()) || typeof password !== 'string' || password.length < 8 || password.length > 256)
        fail(400, 'Use a 3–20 character name and an 8–256 character password.');
      const name = username.trim();
      this.throttle(request, name);
      if (this.one(`SELECT COUNT(*) AS n FROM users`).n >= LIMITS.users) fail(413, 'Account storage quota reached.');
      if (this.one(`SELECT id FROM users WHERE name_lc = ?`, name.toLowerCase())) fail(409, 'That player name is already registered.');
      const hash = await passwordHash(password);
      if (this.one(`SELECT id FROM users WHERE name_lc = ?`, name.toLowerCase())) fail(409, 'That player name is already registered.');
      const user = { id: crypto.randomUUID(), name };
      this.sql.exec(`INSERT INTO users(id, name, name_lc, hash, created, last_seen) VALUES (?, ?, ?, ?, ?, ?)`,
        user.id, name, name.toLowerCase(), hash, this.now(), this.now());
      return { status: 201, body: await this.newDevice(user, remember === true, url, headers) };
    }
    if (route === 'login' && method === 'POST') {
      this.sameOrigin(request, url);
      const b = await this.body(request);
      if (!plain(b)) fail(400, 'A JSON object is required.');
      const { username, password, remember = true } = b;
      if (typeof username !== 'string' || username.trim().length < 3 || username.trim().length > 20 || typeof password !== 'string' || !password.length || password.length > 256)
        fail(400, 'Enter a player name and password.');
      this.throttle(request, username.trim());
      const row = this.one(`SELECT id, name, hash FROM users WHERE name_lc = ?`, username.trim().toLowerCase());
      const ok = await passwordMatches(password, row ? row.hash : DUMMY);
      if (!row || !ok) fail(401, 'Player name or password is incorrect.');
      this.sql.exec(`UPDATE users SET last_seen = ? WHERE id = ?`, this.now(), row.id);
      return { body: await this.newDevice({ id: row.id, name: row.name }, remember === true, url, headers) };
    }
    if (route === 'session' && method === 'POST') {
      this.sameOrigin(request, url);
      if (request.headers.get('Authorization') !== null) await this.authenticate(request);
      const saved = await this.fromCookie(request);
      if (!saved) fail(401, 'Sign in to continue.');
      return { body: { token: await this.makeToken(saved.user, saved.sid), user: { id: saved.user.id, username: saved.user.name, rating: 1000 }, remembered: saved.remember } };
    }
    if (route === 'logout' && method === 'POST') {
      this.sameOrigin(request, url);
      if (request.headers.get('Authorization') !== null) await this.authenticate(request);
      const token = this.cookieToken(request);
      if (token) this.sql.exec(`UPDATE sessions SET revoked = 1 WHERE sid = ?`, await sha256(token));
      headers['Set-Cookie'] = `${COOKIE}=; Path=/; HttpOnly; SameSite=Lax; Max-Age=0${url.protocol === 'https:' ? '; Secure' : ''}`;
      return { body: { success: true } };
    }
    if (route === 'account' && method === 'DELETE') {
      // Deleting an account needs its password again, not just a signed-in device.
      const auth = await this.authenticate(request);
      if (auth.method !== 'bearer') this.sameOrigin(request, url);
      const b = await this.body(request);
      const row = this.one(`SELECT id, name, hash FROM users WHERE id = ?`, auth.user.id);
      this.throttle(request, row ? row.name : '');
      if (!row || !plain(b) || typeof b.password !== 'string' || b.password.length > 256 || !await passwordMatches(b.password, row.hash))
        fail(401, 'Password is incorrect.');
      const drop = () => {
        for (const t of ['cloud_part', 'cloud_head', 'sessions']) this.sql.exec(`DELETE FROM ${t} WHERE user_id = ?`, row.id);
        this.sql.exec(`DELETE FROM users WHERE id = ?`, row.id);
      };
      if (this.transactionSync) this.transactionSync(drop); else drop();
      if (this.onDelete) await this.onDelete(row.id);
      headers['Set-Cookie'] = `${COOKIE}=; Path=/; HttpOnly; SameSite=Lax; Max-Age=0${url.protocol === 'https:' ? '; Secure' : ''}`;
      return { body: { deleted: true, username: row.name } };
    }
    if (route === 'profile' && method === 'GET') {
      const { user } = await this.authenticate(request);
      return { body: { id: user.id, username: user.name, rating: 1000 } };
    }
    if (route === 'cloud' && (method === 'GET' || method === 'HEAD')) {
      const { user } = await this.authenticate(request);
      return { body: this.readCloud(user.id) || { schema: 1, account_id: user.id, revision: 0, files: [], settings: {}, updated_at: null } };
    }
    if (route === 'cloud' && method === 'PUT') {
      const auth = await this.authenticate(request);
      if (auth.method !== 'bearer') this.sameOrigin(request, url);
      const b = await this.body(request, MAX_BODY_BYTES);
      if (!plain(b) || !Number.isSafeInteger(b.base_revision) || b.base_revision < 0) fail(400, 'A cloud base revision is required.');
      const head = this.head(auth.user.id), revision = head ? head.revision : 0;
      if (b.base_revision !== revision) fail(409, 'Another device has newer cloud changes. Reload the cloud snapshot before saving.');
      const snap = await validateSnapshot(b);
      const next = { schema: 1, account_id: auth.user.id, revision: revision + 1, files: snap.files, settings: snap.settings, updated_at: new Date(this.now()).toISOString() };
      const json = JSON.stringify(next);
      const used = this.one(`SELECT COALESCE(SUM(bytes), 0) AS n FROM cloud_head`).n;
      if (used + json.length > LIMITS.totalCloudBytes) fail(413, 'Cloud storage is full right now. Your local saves are untouched.');
      this.writeCloud(auth.user.id, next.revision, json, next.updated_at);
      return { body: { success: true, revision: next.revision, updated_at: next.updated_at, files: next.files.length, bytes: snap.bytes } };
    }
    fail(405, 'Method not allowed.');
  }

  head(userId) { return this.one(`SELECT revision, parts, bytes, updated_at FROM cloud_head WHERE user_id = ? ORDER BY revision DESC LIMIT 1`, userId); }
  readCloud(userId) {
    const head = this.head(userId); if (!head) return null;
    const parts = this.all(`SELECT data FROM cloud_part WHERE user_id = ? AND revision = ? ORDER BY part`, userId, head.revision);
    if (parts.length !== head.parts) fail(500, 'Cloud record is incomplete.');
    return JSON.parse(parts.map(p => p.data).join(''));
  }
  writeCloud(userId, revision, json, updatedAt) {
    const write = () => {
      let part = 0;
      for (let i = 0; i < json.length; i += CHUNK) {
        this.sql.exec(`INSERT INTO cloud_part(user_id, revision, part, data) VALUES (?, ?, ?, ?)`, userId, revision, part++, json.slice(i, i + CHUNK));
      }
      this.sql.exec(`INSERT INTO cloud_head(user_id, revision, parts, bytes, updated_at) VALUES (?, ?, ?, ?, ?)`, userId, revision, part, json.length, updatedAt);
      // Keep this revision and the one before it.
      this.sql.exec(`DELETE FROM cloud_part WHERE user_id = ? AND revision < ?`, userId, revision - 1);
      this.sql.exec(`DELETE FROM cloud_head WHERE user_id = ? AND revision < ?`, userId, revision - 1);
    };
    if (this.transactionSync) this.transactionSync(write); else write();
  }
}
