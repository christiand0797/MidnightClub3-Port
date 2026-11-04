'use strict';
// Account-scoped cloud files are opaque original save/config bytes. This service never edits game saves.
const crypto = require('crypto');
const fs = require('fs');
const path = require('path');
const { promisify } = require('util');
const scrypt = promisify(crypto.scrypt);
const pbkdf2 = promisify(crypto.pbkdf2);
const COOKIE = 'mc3_device';
const MAX_BYTES = 16 * 1024 * 1024;
const MAX_JSON_BYTES = 24 * 1024 * 1024;
const ID = /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/i;
const digest = bytes => crypto.createHash('sha256').update(bytes).digest('hex');
const DEFAULT_LIMITS = Object.freeze({ users: 10000, sessions: 4096, sessionsPerUser: 32, historyFiles: 128,
  accountBytes: 256 * 1024 * 1024, sessionBytes: 4096, cloudBytes: MAX_JSON_BYTES });

class KdfLimiter {
  constructor(concurrency = 2, queued = 8) {
    if (!Number.isInteger(concurrency) || concurrency < 1 || concurrency > 8 || !Number.isInteger(queued) || queued < 0 || queued > 64) throw Error('Invalid password-work limits.');
    this.concurrency = concurrency; this.maximumQueued = queued; this.active = 0; this.queue = [];
  }
  run(operation) {
    if (this.active >= this.concurrency && this.queue.length >= this.maximumQueued) return Promise.reject(Object.assign(Error('Sign-in service is busy. Please retry shortly.'), { status: 429 }));
    return new Promise((resolve, reject) => {
      const start = () => {
        this.active++;
        Promise.resolve().then(operation).then(resolve, reject).finally(() => {
          this.active--; const next = this.queue.shift(); if (next) next();
        });
      };
      if (this.active < this.concurrency) start(); else this.queue.push(start);
    });
  }
}
const passwordWork = new KdfLimiter();
const dummyPassword = `scrypt-v1$${crypto.randomBytes(16).toString('hex')}$${'0'.repeat(128)}`;
function ordinaryPath(file) {
  let scan = path.resolve(file);
  while (true) {
    try { if (fs.lstatSync(scan).isSymbolicLink()) throw Error('Account storage contains a link.'); }
    catch (error) { if (error.code !== 'ENOENT') throw error; }
    const parent = path.dirname(scan); if (parent === scan) break; scan = parent;
  }
}

function atomicJson(file, value) {
  ordinaryPath(file);
  const data = JSON.stringify(value);
  if (Buffer.byteLength(data) > 64 * 1024 * 1024) throw Error('Account database exceeds its storage bound.');
  fs.mkdirSync(path.dirname(file), { recursive: true });
  const temporary = file + '.' + crypto.randomUUID() + '.tmp';
  const handle = fs.openSync(temporary, 'wx', 0o600);
  try { fs.writeFileSync(handle, data); fs.fsyncSync(handle); }
  finally { fs.closeSync(handle); }
  fs.renameSync(temporary, file);
}
function readJson(file, fallback, maxBytes = MAX_JSON_BYTES) {
  let handle;
  try {
    ordinaryPath(file); handle = fs.openSync(file, 'r');
    const info = fs.fstatSync(handle);
    if (!info.isFile() || info.size > maxBytes) throw Error('Account storage record exceeds its bound.');
    const bytes = Buffer.alloc(info.size); let offset = 0;
    while (offset < bytes.length) { const count = fs.readSync(handle, bytes, offset, bytes.length - offset, offset); if (!count) throw Error('Account storage record was truncated.'); offset += count; }
    if (fs.fstatSync(handle).size !== info.size) throw Error('Account storage record changed during reading.');
    return JSON.parse(bytes.toString('utf8'));
  }
  catch (error) { if (error.code === 'ENOENT') return fallback; throw error; }
  finally { if (handle !== undefined) fs.closeSync(handle); }
}
function fail(status, message) { const error = new Error(message); error.status = status; throw error; }
function plainObject(value) { return value && !Array.isArray(value) && Object.getPrototypeOf(value) === Object.prototype; }
function cloudPath(value) {
  // Portable virtual paths, independent of a machine's real HDD instance and account folder.
  if (typeof value !== 'string' || value.length > 180 || /[\\:\x00-\x1f<>"|?*]/.test(value) ||
      Buffer.from(value, 'utf8').toString('utf8') !== value || value.normalize('NFC') !== value) return false;
  const parts = value.split('/');
  if (parts.some(p => !p || p === '.' || p === '..' || /[. ]$/.test(p) || /^(?:CON|PRN|AUX|NUL|CLOCK\$|CONIN\$|CONOUT\$|(?:COM|LPT)[1-9\u00b9\u00b2\u00b3])(?:\.|$)/i.test(p))) return false;
  if (value === 'settings.toml' || value === 'settings.ini') return true;
  if (parts[0] === 'config') return parts.length <= 4 && /\.(toml|ini|json|cfg)$/i.test(value);
  return parts[0] === 'hdd' && parts.length <= 8 && parts.length >= 2;
}
function validateSnapshot(body) {
  if (!plainObject(body) || !Number.isSafeInteger(body.base_revision) || body.base_revision < 0) fail(400, 'A cloud base revision is required.');
  if (!Array.isArray(body.files) || body.files.length > 256 || !plainObject(body.settings)) fail(400, 'Invalid save/config snapshot.');
  const pending = [{ value: body.settings, depth: 0 }]; let nodes = 0;
  while (pending.length) {
    const { value, depth } = pending.pop();
    if (++nodes > 8192 || depth > 16) fail(400, 'Settings nesting exceeds its bound.');
    if (value === null || typeof value === 'boolean' || typeof value === 'string') continue;
    if (typeof value === 'number') { if (!Number.isFinite(value)) fail(400, 'Settings contain an invalid number.'); continue; }
    if (!Array.isArray(value) && !plainObject(value)) fail(400, 'Settings contain unsupported values.');
    for (const key of Object.keys(value)) {
      if (key.length > 128 || ['__proto__', 'constructor', 'prototype'].includes(key)) fail(400, 'Settings contain an unsafe key.');
      pending.push({ value: value[key], depth: depth + 1 });
    }
  }
  if (Buffer.byteLength(JSON.stringify(body.settings)) > 128 * 1024) fail(413, 'Settings exceed the cloud limit.');
  const names = new Set(); let total = 0;
  const files = body.files.map(file => {
    if (!plainObject(file) || !cloudPath(file.path) || typeof file.data !== 'string' || file.encoding !== 'base64' ||
        !/^[0-9a-f]{64}$/.test(file.sha256 || '')) fail(400, 'Invalid cloud file path, encoding or digest.');
    const key = file.path.toLowerCase(); if (names.has(key)) fail(400, 'Duplicate cloud file path.'); names.add(key);
    if (file.data.length > Math.ceil(MAX_BYTES / 3) * 4 || file.data.length % 4 || /[^A-Za-z0-9+/=]/.test(file.data)) fail(400, 'Invalid file encoding.');
    const bytes = Buffer.from(file.data, 'base64'); total += bytes.length;
    if(bytes.toString('base64')!==file.data)fail(400,'File encoding is not canonical base64.');
    if (total > MAX_BYTES) fail(413, 'Save/config files exceed 16 MiB.');
    if (digest(bytes) !== file.sha256) fail(400, 'Cloud file digest mismatch.');
    return { path: file.path, encoding: 'base64', data: file.data, sha256: file.sha256, size: bytes.length };
  });
  const ordered = [...names].sort();
  for (let i = 1; i < ordered.length; i++) if (ordered[i].startsWith(ordered[i - 1] + '/')) fail(400, 'Cloud paths conflict as files and directories.');
  return { files, settings: body.settings, bytes: total };
}
async function passwordHash(password, limiter = passwordWork) {
  const salt = crypto.randomBytes(16); const key = await limiter.run(() => scrypt(password, salt, 64));
  return `scrypt-v1$${salt.toString('hex')}$${key.toString('hex')}`;
}
async function passwordMatches(password, stored, limiter = passwordWork) {
  let actual, expected;
  if (/^scrypt-v1\$[0-9a-f]{32}\$[0-9a-f]{128}$/.test(stored || '')) {
    const [, salt, hash] = stored.split('$'); actual = await limiter.run(() => scrypt(password, Buffer.from(salt, 'hex'), 64)); expected = Buffer.from(hash, 'hex');
  } else if (/^[0-9a-f]{128}$/.test(stored || '')) {
    // Existing accounts retain their password; successful sign-in upgrades the former shared-salt format.
    actual = await limiter.run(() => pbkdf2(password, 'mc3-salt', 100000, 64, 'sha512')); expected = Buffer.from(stored, 'hex');
  } else return false;
  return expected.length === actual.length && crypto.timingSafeEqual(actual, expected);
}
function cookieToken(req) {
  const match = (req.headers.cookie || '').match(/(?:^|;\s*)mc3_device=([A-Za-z0-9_-]{43})(?:;|$)/);
  return match ? match[1] : '';
}
function installAccountRoutes(app, options) {
  const { db, dataDirectory, persist, makeToken, verifyToken } = options;
  const root = path.join(dataDirectory, 'accounts-v1'); fs.mkdirSync(root, { recursive: true });
  const wrap = fn => (req, res, next) => Promise.resolve().then(() => fn(req, res)).catch(error => {
    if (res.headersSent) return next(error);
    res.status(error.status || 500).json({ error: error.status ? error.message : 'Account storage is temporarily unavailable.' });
  });
  const publicUser = user => ({ id: user.id, username: user.username, rating: user.rating || 1000 });
  const sessionFile = token => path.join(root, 'devices', digest(token) + '.json');
  const sendCookie = (req, res, token, remember) => {
    const secure = req.secure || process.env.NODE_ENV === 'production';
    res.setHeader('Set-Cookie', `${COOKIE}=${token}; Path=/; HttpOnly; SameSite=Lax${secure ? '; Secure' : ''}${remember ? '; Max-Age=2592000' : ''}`);
    res.setHeader('Cache-Control', 'no-store');
  };
  const userFromCookie = req => {
    const token = cookieToken(req); if (!token) return null;
    const session = readJson(sessionFile(token), null);
    if (!session || session.revoked || session.expires <= Date.now()) return null;
    const user = db.users.find(u => u.id === session.user_id); return user ? { user, session, token } : null;
  };
  const identity = req => {
    const bearer = (req.headers.authorization || '').match(/^Bearer (.+)$/);
    const verified = bearer && verifyToken(bearer[1]);
    const user = verified && db.users.find(u => u.id === verified.id);
    if (user && ID.test(user.id)) return user;
    const saved = userFromCookie(req); if (saved && ID.test(saved.user.id)) return saved.user;
    fail(401, 'Sign in to access your cloud files.');
  };
  const newDevice = (req, res, user, remember) => {
    const token = crypto.randomBytes(32).toString('base64url');
    atomicJson(sessionFile(token), { user_id: user.id, created: Date.now(), expires: Date.now() + (remember ? 30 * 86400e3 : 12 * 3600e3), remember });
    sendCookie(req, res, token, remember);
    return { token: makeToken(user), user: publicUser(user), remembered: remember };
  };
  // Cookie-authenticated mutations are same-site. Bearer-authenticated native clients do not use ambient cookies.
  app.use('/api/cloud', (req, res, next) => {
    if (!['GET', 'HEAD'].includes(req.method) && !req.headers.authorization && req.headers['sec-fetch-site'] === 'cross-site')
      return res.status(403).json({ error: 'Cross-site cloud changes are not allowed.' });
    res.setHeader('Cache-Control', 'no-store'); next();
  });
  app.post('/api/register', wrap(async (req, res) => {
    const { username, password, remember = true } = req.body;
    if (typeof username !== 'string' || !/^[A-Za-z0-9_ -]{3,20}$/.test(username.trim()) || typeof password !== 'string' || password.length < 8 || password.length > 256)
      fail(400, 'Use a 3–20 character name and an 8–256 character password.');
    const name = username.trim();
    if (db.users.some(u => u.username.toLowerCase() === name.toLowerCase())) fail(409, 'That player name is already registered.');
    const password_hash = await passwordHash(password);
    if (db.users.some(u => u.username.toLowerCase() === name.toLowerCase())) fail(409, 'That player name is already registered.');
    const user = { id: crypto.randomUUID(), username: name, password_hash, email: null, created_at: Math.floor(Date.now()/1000), rating: 1000, games_played: 0 };
    db.users.push(user); persist(db); res.status(201).json(newDevice(req, res, user, remember === true));
  }));
  app.post('/api/login', wrap(async (req, res) => {
    const { username, password, remember = true } = req.body;
    if (typeof username !== 'string' || typeof password !== 'string' || password.length > 256) fail(400, 'Enter a player name and password.');
    const user = db.users.find(u => u.username.toLowerCase() === username.trim().toLowerCase());
    if (!user || !(await passwordMatches(password, user.password_hash))) fail(401, 'Player name or password is incorrect.');
    if (!user.password_hash.startsWith('scrypt-v1$')) user.password_hash = await passwordHash(password);
    user.last_seen = Math.floor(Date.now()/1000); persist(db); res.json(newDevice(req, res, user, remember === true));
  }));
  app.post('/api/session', wrap((req, res) => {
    const saved = userFromCookie(req); if (!saved) fail(401, 'Sign in to continue.');
    res.setHeader('Cache-Control', 'no-store'); res.json({ token: makeToken(saved.user), user: publicUser(saved.user), remembered: saved.session.remember });
  }));
  app.post('/api/logout', wrap((req, res) => {
    const token = cookieToken(req); if (token) {
      const saved = readJson(sessionFile(token), null); if (saved) atomicJson(sessionFile(token), { ...saved, revoked: true });
    }
    res.setHeader('Set-Cookie', `${COOKIE}=; Path=/; HttpOnly; SameSite=Lax; Max-Age=0${req.secure || process.env.NODE_ENV === 'production' ? '; Secure' : ''}`);
    res.setHeader('Cache-Control', 'no-store'); res.json({ success: true });
  }));
  const cloudFile = user => path.join(root, user.id, 'current.json');
  app.get('/api/cloud', wrap((req, res) => {
    const user = identity(req); res.json(readJson(cloudFile(user), { schema: 1, account_id: user.id, revision: 0, files: [], settings: {}, updated_at: null }));
  }));
  app.put('/api/cloud', wrap((req, res) => {
    const user = identity(req), snapshot = validateSnapshot(req.body);
    const previous = readJson(cloudFile(user), null); const revision = previous ? previous.revision : 0;
    if (req.body.base_revision !== revision) fail(409, 'Another device has newer cloud changes. Reload the cloud snapshot before saving.');
    if (previous) {
      const history = path.join(root, user.id, 'history'); fs.mkdirSync(history, { recursive: true });
      const used = fs.readdirSync(history).reduce((sum, name) => sum + fs.statSync(path.join(history, name)).size, 0);
      if (used + Buffer.byteLength(JSON.stringify(previous)) > 256 * 1024 * 1024) fail(413, 'Cloud history storage is full. Export a backup before managing older versions.');
      atomicJson(path.join(history, `${revision}.json`), previous);
    }
    const next = { schema: 1, account_id: user.id, revision: revision + 1, files: snapshot.files, settings: snapshot.settings, updated_at: new Date().toISOString() };
    atomicJson(cloudFile(user), next); res.json({ success: true, revision: next.revision, updated_at: next.updated_at, files: next.files.length, bytes: snapshot.bytes });
  }));
  return { identity, userFromCookie };
}
module.exports = { installAccountRoutes, validateSnapshot, cloudPath, passwordHash, passwordMatches, atomicJson };
