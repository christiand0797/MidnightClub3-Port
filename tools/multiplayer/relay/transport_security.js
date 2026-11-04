'use strict';

// Transport limits, not authentication. Only markAuthenticated AFTER the account service verifies
// a token. Addresses come from the direct socket, never client-controlled forwarded headers.
const { isIP } = require('node:net');
const { TextDecoder } = require('node:util');
const { performance } = require('node:perf_hooks');

const DEFAULTS = Object.freeze({
  maxPayload: 16 * 1024, maxOutgoing: 128 * 1024, maxBufferedBytes: 1024 * 1024,
  maxConnections: 512, maxConnectionsPerIp: 16, maxConnectionsPerUser: 4,
  maxIpStates: 2048, ipIdleMs: 15 * 60 * 1000,
  messagesPerSecond: 120, messageBurst: 240, bytesPerSecond: 256 * 1024, byteBurst: 512 * 1024,
  ipMessagesPerSecond: 600, ipMessageBurst: 1200, ipBytesPerSecond: 1024 * 1024, ipByteBurst: 2 * 1024 * 1024,
  connectsPerSecond: 0.5, connectBurst: 8,
  authTimeoutMs: 15000, heartbeatMs: 30000, pongTimeoutMs: 30000, closeGraceMs: 5000,
  maxRooms: 128, maxRoomsPerUser: 2, maxPlayers: 8, maxSpectators: 16, maxRoomConnections: 48,
});
const ok = value => ({ ok: true, ...value });
const no = (reason, code = 1008) => ({ ok: false, reason, code });

class TokenBucket {
  constructor(rate, burst, now) { this.rate = rate; this.burst = burst; this.tokens = burst; this.last = now; }
  take(amount, now) {
    if (!Number.isFinite(amount) || amount < 0 || !Number.isFinite(now)) return false;
    const at = Math.max(now, this.last);
    this.tokens = Math.min(this.burst, this.tokens + (at - this.last) * this.rate / 1000);
    this.last = at;
    if (amount > this.tokens) return false;
    this.tokens -= amount;
    return true;
  }
}
function ipFromRequest(request) {
  let ip = request && request.socket && request.socket.remoteAddress;
  if (typeof ip !== 'string') return 'unknown';
  if (ip.startsWith('::ffff:') && isIP(ip.slice(7)) === 4) ip = ip.slice(7);
  return isIP(ip) ? ip.toLowerCase() : 'unknown';
}
function plain(value) { return value !== null && typeof value === 'object' && !Array.isArray(value) && Object.getPrototypeOf(value) === Object.prototype; }
function text(value, max, min = 1) { return typeof value === 'string' && value.length >= min && value.length <= max && !/[\u0000-\u0008\u000b\u000c\u000e-\u001f\u007f]/.test(value); }
function identifier(value) { return typeof value === 'string' && /^[A-Za-z0-9_-]{1,64}$/.test(value); }
function originAllowed(origin, host, configured = '', allowWildcard = false) {
  if (origin === undefined) return true; // Native clients have no browser Origin; authentication is still mandatory.
  if (typeof origin !== 'string' || origin.length > 2048 || typeof host !== 'string' || host.length > 255) return false;
  try {
    const url = new URL(origin);
    if (!['http:', 'https:'].includes(url.protocol) || url.origin !== origin || url.username || url.password) return false;
    const allowed = configured.split(',').map(item => item.trim()).filter(Boolean);
    return url.host.toLowerCase() === host.toLowerCase() || allowed.includes(origin) || (allowWildcard && allowed.includes('*'));
  } catch (_) { return false; }
}
function safeTree(value) {
  const stack = [{ value, depth: 0 }]; let nodes = 0;
  while (stack.length) {
    const row = stack.pop();
    if (++nodes > 1024 || row.depth > 8) return false;
    const v = row.value;
    if (v === null || typeof v === 'boolean') continue;
    if (typeof v === 'number') { if (!Number.isFinite(v)) return false; continue; }
    if (typeof v === 'string') { if (!text(v, 8192, 0)) return false; continue; }
    if (!Array.isArray(v) && !plain(v)) return false;
    const keys = Object.keys(v);
    if (keys.length > 128) return false;
    for (const key of keys) {
      if (key.length > 64 || ['__proto__', 'constructor', 'prototype'].includes(key)) return false;
      stack.push({ value: v[key], depth: row.depth + 1 });
    }
  }
  return true;
}
function validateRoomRequest(value, maxPlayers = DEFAULTS.maxPlayers) {
  if (!plain(value) || !text(value.name, 50) || !value.name.trim()) return no('Invalid room name');
  const count = value.maxPlayers === undefined ? maxPlayers : value.maxPlayers;
  const region = value.region === undefined ? 'global' : value.region;
  if (!Number.isInteger(count) || count < 1 || count > maxPlayers || !text(region, 32)) return no('Invalid room capacity or region');
  return ok({ room: { name: value.name.trim(), maxPlayers: count, region } });
}
function validateMessage(value, maxPlayers) {
  if (!plain(value) || !safeTree(value) || typeof value.type !== 'string') return no('Invalid message object');
  switch (value.type) {
    case 'auth':
      if (!text(value.token, 8192) || !/^[A-Za-z0-9_.-]+$/.test(value.token) ||
          (value.clientType !== undefined && !['browser', 'game'].includes(value.clientType))) return no('Invalid authentication message');
      return ok({ message: { type: 'auth', token: value.token, clientType: value.clientType || 'browser' } });
    case 'join':
      // Guest attachment for local adapters (game proxies, stream proxies) that have
      // no user account/token. Identifies the socket with a clientType ('game' for
      // proxies) and a display name; no JWT is involved.
      if (!text(value.name, 50) || !value.name.trim() ||
          (value.clientType !== undefined && !['browser', 'game'].includes(value.clientType))) return no('Invalid join message');
      return ok({ message: { type: 'join', name: value.name.trim(), clientType: value.clientType || 'browser' } });
    case 'create_game': {
      const checked = validateRoomRequest(value, maxPlayers);
      return checked.ok ? ok({ message: { type: value.type, ...checked.room } }) : checked;
    }
    case 'join_game':
      return identifier(value.gameId) ? ok({ message: { type: value.type, gameId: value.gameId } }) : no('Invalid room identifier');
    case 'leave_game': case 'list': return ok({ message: { type: value.type } });
    case 'set_ready':
      return typeof value.ready === 'boolean' ? ok({ message: { type: value.type, ready: value.ready } }) : no('Invalid ready flag');
    case 'start_game':
      if (value.seed !== undefined && (!Number.isSafeInteger(value.seed) || value.seed < 0 || value.seed > 0xFFFFFFFF)) return no('Invalid game seed');
      return ok({ message: { type: value.type, seed: value.seed } });
    case 'input':
      if ((!plain(value.data) && typeof value.data !== 'string') || JSON.stringify(value.data).length > 8192) return no('Invalid input payload');
      // optional `to`: one player's relay client id (a tunnelled datagram for that player only)
      if (value.to !== undefined && !identifier(value.to)) return no('Invalid input recipient');
      return ok({ message: { type: value.type, data: value.data, to: value.to } });
    case 'chat':
      return text(value.text, 512) && value.text.trim() ? ok({ message: { type: value.type, text: value.text } }) : no('Invalid chat message');
    case 'ping':
      if (value.ts !== undefined && (!Number.isSafeInteger(value.ts) || value.ts < 0)) return no('Invalid ping timestamp');
      return ok({ message: { type: value.type, ts: value.ts } });
    default: return no('Unsupported message type');
  }
}

class TransportSecurity {
  constructor(options = {}) {
    this.now = options.now || (() => performance.now());
    this.limits = { ...DEFAULTS };
    for (const key of Object.keys(DEFAULTS)) if (options[key] !== undefined) {
      if (!Number.isFinite(options[key]) || options[key] <= 0 || options[key] > DEFAULTS[key] * 16) throw new TypeError(`Invalid transport limit: ${key}`);
      this.limits[key] = options[key];
    }
    for (const key of ['maxConnections', 'maxConnectionsPerIp', 'maxConnectionsPerUser', 'maxIpStates', 'maxRooms', 'maxRoomsPerUser', 'maxPlayers', 'maxSpectators', 'maxRoomConnections', 'maxPayload', 'maxOutgoing']) {
      if (!Number.isInteger(this.limits[key])) throw new TypeError(`Transport limit must be integral: ${key}`);
    }
    this.sockets = new Map(); this.ips = new Map(); this.rooms = new Map(); this.users = new Map();
    this.decoder = new TextDecoder('utf-8', { fatal: true });
    this.wsOptions = Object.freeze({ maxPayload: this.limits.maxPayload, perMessageDeflate: false });
  }
  prune(at = this.now()) {
    for (const [ip, state] of this.ips) if (!state.connections && at - state.lastSeen >= this.limits.ipIdleMs) this.ips.delete(ip);
  }
  admit(ws, request) {
    if (!ws || this.sockets.has(ws)) return no('Connection already tracked');
    const at = this.now(), l = this.limits, ip = ipFromRequest(request);
    this.prune(at);
    if (this.sockets.size >= l.maxConnections) return no('Server connection quota');
    let shared = this.ips.get(ip);
    if (!shared) {
      if (this.ips.size >= l.maxIpStates) return no('Server address quota');
      shared = { connections: 0, lastSeen: at, connects: new TokenBucket(l.connectsPerSecond, l.connectBurst, at),
        messages: new TokenBucket(l.ipMessagesPerSecond, l.ipMessageBurst, at), bytes: new TokenBucket(l.ipBytesPerSecond, l.ipByteBurst, at),
        creates: new TokenBucket(1 / 30, 2, at) };
      this.ips.set(ip, shared);
    }
    shared.lastSeen = at;
    if (!shared.connects.take(1, at) || shared.connections >= l.maxConnectionsPerIp) return no('Address connection quota');
    shared.connections++;
    this.sockets.set(ws, { ip, shared, admitted: at, userId: null, roomId: null, spectator: false, closingAt: null,
      lastPing: at, awaitingPong: false, messages: new TokenBucket(l.messagesPerSecond, l.messageBurst, at),
      bytes: new TokenBucket(l.bytesPerSecond, l.byteBurst, at), chats: new TokenBucket(2, 6, at) });
    return ok({ ip });
  }
  read(ws, raw, isBinary = false) {
    const state = this.sockets.get(ws);
    if (!state || state.closingAt !== null) return no('Connection unavailable');
    const at = this.now(); state.shared.lastSeen = at;
    if (!state.userId && at - state.admitted >= this.limits.authTimeoutMs) return no('Authentication timed out');
    if (!state.messages.take(1, at) || !state.shared.messages.take(1, at)) return no('Message rate exceeded');
    try {
      let bytes;
      if (typeof raw === 'string') {
        if (Buffer.byteLength(raw, 'utf8') > this.limits.maxPayload) return no('Message too large', 1009);
        bytes = Buffer.from(raw, 'utf8');
      } else if (Buffer.isBuffer(raw)) bytes = raw;
      else if (raw instanceof ArrayBuffer) bytes = new Uint8Array(raw);
      else if (ArrayBuffer.isView(raw)) bytes = new Uint8Array(raw.buffer, raw.byteOffset, raw.byteLength);
      else if (Array.isArray(raw) && raw.every(Buffer.isBuffer)) {
        const size = raw.reduce((sum, part) => sum + part.length, 0);
        if (size > this.limits.maxPayload) return no('Message too large', 1009);
        bytes = Buffer.concat(raw, size);
      } else return no('Invalid message encoding', 1003);
      if (bytes.byteLength > this.limits.maxPayload) return no('Message too large', 1009);
      if (!state.bytes.take(bytes.byteLength, at) || !state.shared.bytes.take(bytes.byteLength, at)) return no('Message byte rate exceeded');
      const parsed = validateMessage(JSON.parse(this.decoder.decode(bytes)), this.limits.maxPlayers);
      if (!parsed.ok) return parsed;
      if (!state.userId && !['auth', 'ping', 'join'].includes(parsed.message.type)) return no('Authenticate first');
      if (state.userId && parsed.message.type === 'auth') return no('Connection is already authenticated');
      if (parsed.message.type === 'chat' && !state.chats.take(1, at)) return no('Chat rate exceeded');
      return parsed;
    } catch (_) { return no('Malformed JSON or UTF-8', 1007); }
  }
  markAuthenticated(ws, userId) {
    const state = this.sockets.get(ws);
    if (!state || state.closingAt !== null || state.userId || !identifier(userId) || this.now() - state.admitted >= this.limits.authTimeoutMs) return no('Invalid authentication state');
    let sockets = this.users.get(userId);
    if (sockets && sockets.size >= this.limits.maxConnectionsPerUser) return no('Account connection quota');
    if (!sockets) { sockets = new Set(); this.users.set(userId, sockets); }
    sockets.add(ws); state.userId = userId;
    return ok();
  }
  createRoom(ws, roomId, request) {
    const state = this.sockets.get(ws), checked = validateRoomRequest(request, this.limits.maxPlayers);
    if (!state || !state.userId || state.closingAt !== null || state.roomId || !identifier(roomId) || !checked.ok) return no('Invalid room creation');
    if (this.rooms.has(roomId) || this.rooms.size >= this.limits.maxRooms) return no('Room quota exceeded');
    let owned = 0; for (const room of this.rooms.values()) if (room.createdBy === state.userId) owned++;
    if (owned >= this.limits.maxRoomsPerUser || !state.shared.creates.take(1, this.now())) return no('Room creation quota exceeded');
    this.rooms.set(roomId, { ...checked.room, createdBy: state.userId, members: new Map([[ws, { userId: state.userId, spectator: false }]]) });
    state.roomId = roomId; return ok({ room: checked.room });
  }
  joinRoom(ws, roomId, { spectator = false } = {}) {
    const state = this.sockets.get(ws), room = this.rooms.get(roomId);
    if (!state || !state.userId || state.closingAt !== null || !room || typeof spectator !== 'boolean') return no('Room unavailable');
    if (state.roomId) return state.roomId === roomId && state.spectator === spectator ? ok() : no('Leave the current room first');
    if (room.members.size >= this.limits.maxRoomConnections) return no('Room connection quota exceeded');
    const users = new Set(), watchers = new Set();
    for (const row of room.members.values()) (row.spectator ? watchers : users).add(row.userId);
    if (spectator ? !watchers.has(state.userId) && watchers.size >= this.limits.maxSpectators : !users.has(state.userId) && users.size >= room.maxPlayers) return no('Room is full');
    room.members.set(ws, { userId: state.userId, spectator }); state.roomId = roomId; state.spectator = spectator; return ok();
  }
  leaveRoom(ws) {
    const state = this.sockets.get(ws); if (!state || !state.roomId) return;
    const room = this.rooms.get(state.roomId);
    if (room) { room.members.delete(ws); if (!room.members.size) this.rooms.delete(state.roomId); }
    state.roomId = null; state.spectator = false;
  }
  removeRoom(roomId) {
    const room = this.rooms.get(roomId); if (!room) return;
    for (const ws of room.members.keys()) { const state = this.sockets.get(ws); if (state) { state.roomId = null; state.spectator = false; } }
    this.rooms.delete(roomId);
  }
  release(ws) {
    const state = this.sockets.get(ws); if (!state) return;
    this.leaveRoom(ws); this.sockets.delete(ws); state.shared.connections--; state.shared.lastSeen = this.now();
    if (state.userId) { const sockets = this.users.get(state.userId); if (sockets) { sockets.delete(ws); if (!sockets.size) this.users.delete(state.userId); } }
  }
  terminate(ws) { try { ws.terminate(); } catch (_) {} this.release(ws); }
  reject(ws, result) {
    const state = this.sockets.get(ws);
    if (state) { if (state.closingAt !== null) return; state.closingAt = this.now(); }
    try { ws.close(result.code || 1008, result.reason || 'Rejected'); } catch (_) { this.terminate(ws); }
  }
  send(ws, value) {
    if (!ws || ws.readyState !== 1) return false;
    try {
      const payload = JSON.stringify(value);
      if (typeof payload !== 'string' || Buffer.byteLength(payload) > this.limits.maxOutgoing || !Number.isFinite(ws.bufferedAmount) || ws.bufferedAmount > this.limits.maxBufferedBytes) {
        this.reject(ws, no('Slow client or oversized server message')); return false;
      }
      ws.send(payload, error => { if (error) this.reject(ws, no('Send failed', 1011)); }); return true;
    } catch (_) { this.reject(ws, no('Send failed', 1011)); return false; }
  }
  attach(ws, request, handler) {
    const admitted = this.admit(ws, request);
    // Always install an error handler: an EventEmitter 'error' with no listener can terminate Node.
    ws.on('error', () => { this.terminate(ws); });
    ws.on('close', () => { this.release(ws); });
    if (!admitted.ok) { this.reject(ws, admitted); return admitted; }
    ws.on('pong', () => { const state = this.sockets.get(ws); if (state) state.awaitingPong = false; });
    ws.on('message', (raw, isBinary) => {
      const checked = this.read(ws, raw, isBinary);
      if (!checked.ok) { this.reject(ws, checked); return; }
      try {
        const promise = handler(checked.message);
        if (promise && typeof promise.catch === 'function') promise.catch(() => this.reject(ws, no('Message handler failed', 1011)));
      } catch (_) { this.reject(ws, no('Message handler failed', 1011)); }
    });
    return admitted;
  }
  tick() {
    const at = this.now(); this.prune(at);
    for (const [ws, state] of this.sockets) {
      if (state.closingAt !== null) { if (at - state.closingAt >= this.limits.closeGraceMs) this.terminate(ws); continue; }
      if (!state.userId && at - state.admitted >= this.limits.authTimeoutMs) { this.reject(ws, no('Authentication timed out')); continue; }
      if (state.awaitingPong) { if (at - state.lastPing >= this.limits.pongTimeoutMs) this.terminate(ws); continue; }
      if (at - state.lastPing >= this.limits.heartbeatMs) {
        state.lastPing = at; state.awaitingPong = true;
        try { ws.ping(); } catch (_) { this.terminate(ws); }
      }
    }
  }
}

module.exports = { TransportSecurity, TokenBucket, validateMessage, validateRoomRequest, ipFromRequest, originAllowed, DEFAULTS };
