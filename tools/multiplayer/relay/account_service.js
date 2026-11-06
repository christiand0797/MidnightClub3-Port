'use strict';
// Account-scoped cloud files are opaque original save/config bytes. This service never edits game saves.
const crypto = require('crypto');
const fs = require('fs');
const path = require('path');
const { promisify, TextDecoder } = require('util');
const scrypt = promisify(crypto.scrypt);
const pbkdf2 = promisify(crypto.pbkdf2);
const COOKIE = 'mc3_device';
const MAX_BYTES = 16 * 1024 * 1024;
const MAX_JSON_BYTES = 24 * 1024 * 1024;
const ID = /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/i;
const digest = bytes => crypto.createHash('sha256').update(bytes).digest('hex');
const DEFAULT_LIMITS = Object.freeze({ users: 10000, sessions: 4096, sessionsPerUser: 32, historyFiles: 128,
  accountBytes: 256 * 1024 * 1024, sessionBytes: 4096, cloudBytes: MAX_JSON_BYTES,
  totalCloudBytes: 2 * 1024 * 1024 * 1024, totalCloudFiles: 32768,
  cloudRequests: 4, cloudRequestsPerUser: 1, cloudRequestMs: 30000 });

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
    return JSON.parse(new TextDecoder('utf-8', { fatal: true }).decode(bytes));
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
  for (const name of names) {
    let parent = name;
    while (parent.includes('/')) { parent = parent.slice(0, parent.lastIndexOf('/')); if (names.has(parent)) fail(400, 'Cloud paths conflict as files and directories.'); }
  }
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
  const header = req.headers.cookie;
  if (header !== undefined && (typeof header !== 'string' || header.length > 8192)) fail(400, 'Cookie header exceeds its limit.');
  const match = (header || '').match(/(?:^|;\s*)mc3_device=([A-Za-z0-9_-]{43})(?:;|$)/);
  return match ? match[1] : '';
}
function activeSession(session, userId, now = Date.now()) {
  return plainObject(session) && ID.test(session.user_id || '') && (userId === undefined || (typeof userId === 'string' && session.user_id === userId)) && !session.revoked &&
    Number.isSafeInteger(session.created) && Number.isSafeInteger(session.expires) && session.created >= 0 && session.created <= now + 300000 && session.expires > session.created &&
    session.expires - session.created <= 30 * 86400e3 && session.expires > now;
}
function installAccountRoutes(app, options) {
  const { db, dataDirectory, persist, makeToken, verifyToken } = options;
  const limits = { ...DEFAULT_LIMITS, ...options.limits }, limiter = options.kdfLimiter || passwordWork;
  for (const key of Object.keys(DEFAULT_LIMITS)) if (!Number.isSafeInteger(limits[key]) || limits[key] < 1 || limits[key] > DEFAULT_LIMITS[key]) throw Error('Invalid account storage limits.');
  if (!Array.isArray(db.users)) throw Error('The account database must have a users array.');
  const publicOrigin = options.publicOrigin || process.env.PUBLIC_ORIGIN || '';
  if (publicOrigin) { const url = new URL(publicOrigin); if (!['http:', 'https:'].includes(url.protocol) || url.origin !== publicOrigin) throw Error('PUBLIC_ORIGIN must be one exact HTTP(S) origin without a path.'); }
  const root = path.join(dataDirectory, 'accounts-v1'); fs.mkdirSync(root, { recursive: true });
  const devices = path.join(root, 'devices'); ordinaryPath(devices); fs.mkdirSync(devices, { recursive: true });
  const wrap = fn => (req, res, next) => Promise.resolve().then(() => fn(req, res, next)).catch(error => {
    if (res.headersSent) return next(error);
    res.status(error.status || 500).json({ error: error.status ? error.message : 'Account storage is temporarily unavailable.' });
  });
  const publicUser = user => ({ id: user.id, username: user.username, rating: user.rating || 1000 });
  const sessionFile = token => path.join(devices, digest(token) + '.json');
  const sameOrigin = req => {
    const supplied = req.headers.origin, site = req.headers['sec-fetch-site'];
    if (site !== undefined && site !== 'same-origin' && site !== 'none') fail(403, 'Account changes require the same origin.');
    if (supplied === undefined) {
      if ((req.headers.cookie || '').includes(COOKIE + '=') && site !== 'same-origin') fail(403, 'Cookie-authenticated changes require an Origin header.');
      return; // Explicit native credentials/Bearer do not use ambient browser cookies.
    }
    const expected = publicOrigin || `${req.protocol}://${req.headers.host}`;
    try { const url = new URL(supplied); if (!['http:', 'https:'].includes(url.protocol) || url.origin !== supplied || supplied !== new URL(expected).origin) fail(403, 'Account changes require the same origin.'); }
    catch (error) { if (error.status) throw error; fail(403, 'Account changes require the same origin.'); }
  };
  const sendCookie = (req, res, token, remember) => {
    const secure = req.secure || process.env.NODE_ENV === 'production';
    res.setHeader('Set-Cookie', `${COOKIE}=${token}; Path=/; HttpOnly; SameSite=Lax${secure ? '; Secure' : ''}${remember ? '; Max-Age=2592000' : ''}`);
    res.setHeader('Cache-Control', 'no-store');
  };
  const userFromCookie = req => {
    const token = cookieToken(req); if (!token) return null;
    const session = readJson(sessionFile(token), null, limits.sessionBytes);
    if (!activeSession(session)) return null;
    const user = db.users.find(u => u.id === session.user_id); return user && ID.test(user.id) ? { user, session, token } : null;
  };
  const authenticate = req => {
    if (req.headers.authorization !== undefined) {
      const header = req.headers.authorization;
      const bearer = typeof header === 'string' && header.length <= 8192 && header.match(/^Bearer ([A-Za-z0-9_.-]+)$/);
      const verified = bearer && verifyToken(bearer[1]);
      const user = plainObject(verified) && ID.test(verified.id || '') && db.users.find(u => u.id === verified.id);
      if (!user) fail(401, 'Invalid or expired access token.');
      return { user, method: 'bearer' }; // An invalid explicit credential NEVER falls back to an ambient cookie.
    }
    const saved = userFromCookie(req); if (saved && ID.test(saved.user.id)) return { user: saved.user, method: 'cookie' };
    fail(401, 'Sign in to access your cloud files.');
  };
  const identity = req => { const result = authenticate(req); return result.user || result; };
  const commitUsers = users => { const candidate = { ...db, users }; persist(candidate); db.users = users; };
  const prepareDevice = (user, remember) => {
    let count = 0, owned = 0, visited = 0;
    const directory = fs.opendirSync(devices);
    try { let entry; while ((entry = directory.readSync()) !== null) {
      if (++visited > limits.sessions * 2 + 32) fail(413, 'Device session directory exceeds its bound.');
      if (!entry.isFile() || entry.isSymbolicLink()) fail(500, 'Account storage is temporarily unavailable.');
      const file = path.join(devices, entry.name);
      const session = /^[0-9a-f]{64}\.json$/.test(entry.name) ? readJson(file, null, limits.sessionBytes) : null;
      if (plainObject(session) && ID.test(session.user_id || '') && (session.revoked === true || (Number.isSafeInteger(session.expires) && session.expires <= Date.now()))) {
        ordinaryPath(file); fs.unlinkSync(file); continue; // Only expired/revoked service session records; never cloud files.
      }
      count++; if (session && session.user_id === user.id) owned++;
    } } finally { directory.closeSync(); }
    if (count >= limits.sessions || owned >= limits.sessionsPerUser) fail(429, 'Device session quota reached. Sign out an existing device before signing in again.');
    const token = crypto.randomBytes(32).toString('base64url');
    const created = Date.now(), record = { user_id: user.id, created, expires: created + (remember ? 30 * 86400e3 : 12 * 3600e3), remember };
    const file = sessionFile(token); if (fs.existsSync(file)) fail(500, 'Account storage is temporarily unavailable.');
    atomicJson(file, record); return { token, file, record };
  };
  const discardDevice = device => {
    const saved = readJson(device.file, null, limits.sessionBytes);
    if (saved && saved.user_id === device.record.user_id && saved.created === device.record.created) fs.unlinkSync(device.file);
  };
  const finishDevice = (req, res, user, remember, device) => {
    const token = device.token;
    sendCookie(req, res, token, remember);
    return { token: makeToken(user, digest(token)), user: publicUser(user), remembered: remember };
  };
  const cloudWork = { active: 0, accounts: new Map() };
  const reserveCloud = (req, res, user) => {
    if (req.aborted) fail(400, 'Cloud request was aborted.');
    const accountCount = cloudWork.accounts.get(user.id) || 0;
    if (cloudWork.active >= limits.cloudRequests || accountCount >= limits.cloudRequestsPerUser) {
      res.setHeader('Retry-After', '1'); fail(429, 'A cloud transfer is already active. Please retry shortly.');
    }
    cloudWork.active++; cloudWork.accounts.set(user.id, accountCount + 1);
    let released = false, deadline;
    const release = () => {
      if (released) return; released = true;
      clearTimeout(deadline);
      req.removeListener('aborted', release); res.removeListener('finish', release); res.removeListener('close', release);
      cloudWork.active--;
      const remaining = (cloudWork.accounts.get(user.id) || 1) - 1;
      if (remaining) cloudWork.accounts.set(user.id, remaining); else cloudWork.accounts.delete(user.id);
    };
    req.once('aborted', release); res.once('finish', release); res.once('close', release);
    deadline = setTimeout(() => {
      // Destroy the streams before freeing their budget; a timed-out body parser must not keep
      // accumulating bytes outside the concurrency bound. The deadline also bounds slow readers.
      req.destroy(); res.destroy(); release();
    }, limits.cloudRequestMs);
    if (deadline.unref) deadline.unref();
  };
  // Cookie-authenticated mutations are same-site. Bearer-authenticated native clients do not use ambient cookies.
  app.use('/api/cloud', wrap((req, res, next) => {
    const auth = authenticate(req); req.mc3Account = auth.user || auth;
    if (!['GET', 'HEAD'].includes(req.method) && auth.method !== 'bearer') sameOrigin(req);
    reserveCloud(req, res, req.mc3Account); // Before the 24 MiB parser, and held through response finish/close.
    res.setHeader('Cache-Control', 'no-store'); next();
  }), options.cloudJsonParser || require('express').json({ limit: MAX_JSON_BYTES }));
  app.use('/api/cloud', (error, req, res, next) => {
    if (error.type === 'entity.too.large') return res.status(413).json({ error: 'Cloud request exceeds its limit.' });
    if (error instanceof SyntaxError) return res.status(400).json({ error: 'Malformed JSON cloud request.' });
    next(error);
  });
  app.post('/api/register', wrap(async (req, res) => {
    sameOrigin(req);
    if (!plainObject(req.body)) fail(400, 'A JSON object is required.');
    const { username, password, remember = true } = req.body;
    if (typeof username !== 'string' || !/^[A-Za-z0-9_ -]{3,20}$/.test(username.trim()) || typeof password !== 'string' || password.length < 8 || password.length > 256)
      fail(400, 'Use a 3–20 character name and an 8–256 character password.');
    const name = username.trim();
    if (db.users.length >= limits.users) fail(413, 'Account storage quota reached.');
    if (db.users.some(u => u.username.toLowerCase() === name.toLowerCase())) fail(409, 'That player name is already registered.');
    const password_hash = await passwordHash(password, limiter);
    if (db.users.some(u => u.username.toLowerCase() === name.toLowerCase())) fail(409, 'That player name is already registered.');
    if (db.users.length >= limits.users) fail(413, 'Account storage quota reached.');
    const user = { id: crypto.randomUUID(), username: name, password_hash, email: null, created_at: Math.floor(Date.now()/1000), rating: 1000, games_played: 0 };
    const device = prepareDevice(user, remember === true);
    try { commitUsers([...db.users, user]); }
    catch (error) { discardDevice(device); throw error; }
    res.status(201).json(finishDevice(req, res, user, remember === true, device));
  }));
  app.post('/api/login', wrap(async (req, res) => {
    sameOrigin(req);
    if (!plainObject(req.body)) fail(400, 'A JSON object is required.');
    const { username, password, remember = true } = req.body;
    if (typeof username !== 'string' || username.length < 3 || username.length > 20 || typeof password !== 'string' || !password.length || password.length > 256) fail(400, 'Enter a player name and password.');
    const user = db.users.find(u => u.username.toLowerCase() === username.trim().toLowerCase());
    const matched = await passwordMatches(password, user ? user.password_hash : dummyPassword, limiter);
    if (!user || !matched) fail(401, 'Player name or password is incorrect.');
    const password_hash = user.password_hash.startsWith('scrypt-v1$') ? user.password_hash : await passwordHash(password, limiter);
    const current = db.users.find(u => u.id === user.id); if (!current) fail(401, 'Player name or password is incorrect.');
    const updated = { ...current, password_hash, last_seen: Math.floor(Date.now()/1000) };
    const device = prepareDevice(updated, remember === true);
    try { commitUsers(db.users.map(u => u.id === updated.id ? updated : u)); }
    catch (error) { discardDevice(device); throw error; }
    res.json(finishDevice(req, res, updated, remember === true, device));
  }));
  app.post('/api/session', wrap((req, res) => {
    sameOrigin(req);
    if (req.headers.authorization !== undefined) authenticate(req); // No cookie fallback for an explicitly invalid token.
    const saved = userFromCookie(req); if (!saved) fail(401, 'Sign in to continue.');
    res.setHeader('Cache-Control', 'no-store'); res.json({ token: makeToken(saved.user, digest(saved.token)), user: publicUser(saved.user), remembered: saved.session.remember });
  }));
  app.post('/api/logout', wrap((req, res) => {
    sameOrigin(req);
    if (req.headers.authorization !== undefined) authenticate(req);
    const token = cookieToken(req); if (token) {
      const saved = readJson(sessionFile(token), null, limits.sessionBytes);
      if (saved && (!plainObject(saved) || !ID.test(saved.user_id || ''))) fail(500, 'Account storage is temporarily unavailable.');
      if (saved) atomicJson(sessionFile(token), { ...saved, revoked: true });
      if (saved && options.onSessionRevoked) options.onSessionRevoked(digest(token));
    }
    res.setHeader('Set-Cookie', `${COOKIE}=; Path=/; HttpOnly; SameSite=Lax; Max-Age=0${req.secure || process.env.NODE_ENV === 'production' ? '; Secure' : ''}`);
    res.setHeader('Cache-Control', 'no-store'); res.json({ success: true });
  }));
  const accountUsage = userId => {
    const directoryName = path.join(root, userId); ordinaryPath(directoryName);
    const usage = { bytes: 0, files: 0, history: 0 };
    if (!fs.existsSync(directoryName)) return usage;
    const inspect = (folder, history) => {
      ordinaryPath(folder); const directory = fs.opendirSync(folder); let visited = 0;
      try { let entry; while ((entry = directory.readSync()) !== null) {
        if (++visited > limits.historyFiles + 2) fail(413, 'Cloud account file count exceeds its bound.');
        const file = path.join(folder, entry.name);
        if (!history && entry.name === 'history' && entry.isDirectory()) { inspect(file, true); continue; }
        if (!entry.isFile() || entry.isSymbolicLink()) fail(500, 'Account storage is temporarily unavailable.');
        ordinaryPath(file); usage.bytes += fs.statSync(file).size; usage.files++; if (history) usage.history++;
      } } finally { directory.closeSync(); }
    };
    inspect(directoryName, false); return usage;
  };
  const totalUsage = () => {
    const totals = { bytes: 0, files: 0 }; const directory = fs.opendirSync(root); let visited = 0;
    try { let entry; while ((entry = directory.readSync()) !== null) {
      if (++visited > limits.users + 1) fail(413, 'Cloud account count exceeds its bound.');
      if (entry.name === 'devices') continue;
      if (!entry.isDirectory() || entry.isSymbolicLink() || !ID.test(entry.name)) fail(500, 'Account storage is temporarily unavailable.');
      const usage = accountUsage(entry.name); totals.bytes += usage.bytes; totals.files += usage.files;
      if (totals.bytes > limits.totalCloudBytes || totals.files > limits.totalCloudFiles) fail(413, 'Cloud storage quota reached.');
    } } finally { directory.closeSync(); }
    return totals;
  };
  const cloudFile = user => path.join(root, user.id, 'current.json');
  const readCloud = user => {
    const stored = readJson(cloudFile(user), null, limits.cloudBytes);
    if (stored && (!plainObject(stored) || stored.schema !== 1 || stored.account_id !== user.id || !Number.isSafeInteger(stored.revision) || stored.revision < 0 ||
        !Array.isArray(stored.files) || stored.files.length > 256 || !plainObject(stored.settings))) fail(500, 'Account storage is temporarily unavailable.');
    return stored;
  };
  app.get('/api/cloud', wrap((req, res) => {
    const user = req.mc3Account; res.json(readCloud(user) || { schema: 1, account_id: user.id, revision: 0, files: [], settings: {}, updated_at: null });
  }));
  app.put('/api/cloud', wrap((req, res) => {
    const user = req.mc3Account;
    if (!plainObject(req.body) || !Number.isSafeInteger(req.body.base_revision) || req.body.base_revision < 0) fail(400, 'A cloud base revision is required.');
    const previous = readCloud(user); const revision = previous ? previous.revision : 0;
    if (previous && (!Number.isSafeInteger(revision) || revision < 0 || revision >= Number.MAX_SAFE_INTEGER || previous.account_id !== user.id)) fail(500, 'Account storage is temporarily unavailable.');
    if (req.body.base_revision !== revision) fail(409, 'Another device has newer cloud changes. Reload the cloud snapshot before saving.');
    const snapshot = validateSnapshot(req.body);
    const next = { schema: 1, account_id: user.id, revision: revision + 1, files: snapshot.files, settings: snapshot.settings, updated_at: new Date().toISOString() };
    const nextBytes = Buffer.byteLength(JSON.stringify(next)); if (nextBytes > limits.cloudBytes) fail(413, 'Cloud record exceeds its storage bound.');
    const account = accountUsage(user.id), totals = totalUsage();
    const currentBytes = previous ? fs.statSync(cloudFile(user)).size : 0;
    const historyPath = path.join(root, user.id, 'history', `${revision}.json`);
    const needsHistory = previous && !fs.existsSync(historyPath), added = nextBytes - currentBytes + (needsHistory ? currentBytes : 0);
    if (account.bytes + added > limits.accountBytes || totals.bytes + added > limits.totalCloudBytes ||
        account.history + (needsHistory ? 1 : 0) > limits.historyFiles || totals.files + (previous ? 0 : 1) + (needsHistory ? 1 : 0) > limits.totalCloudFiles)
      fail(413, 'Cloud storage quota reached. Export a backup before managing older versions.');
    if (previous) {
      const history = path.join(root, user.id, 'history'); fs.mkdirSync(history, { recursive: true });
      if (needsHistory) atomicJson(historyPath, previous);
      else if (digest(JSON.stringify(readJson(historyPath, null, limits.cloudBytes))) !== digest(JSON.stringify(previous))) fail(500, 'Existing cloud history differs; no files were replaced.');
    }
    atomicJson(cloudFile(user), next); res.json({ success: true, revision: next.revision, updated_at: next.updated_at, files: next.files.length, bytes: snapshot.bytes });
  }));
  return { identity, userFromCookie, limits, kdfLimiter: limiter,
    cloudActivity: () => ({ active: cloudWork.active, accounts: cloudWork.accounts.size }) };
}
module.exports = { installAccountRoutes, validateSnapshot, cloudPath, passwordHash, passwordMatches, atomicJson, readJson, activeSession, KdfLimiter, DEFAULT_LIMITS };
