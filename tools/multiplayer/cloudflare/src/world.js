// The online world as one Durable Object: every WebSocket, the Main Lobby and named rooms, the game tunnels' race
// traffic, and the social layer (presence, car clubs, club chat, rankings, race points). It speaks the same protocol
// as tools/multiplayer/relay/server.js, so proxy.py, the launcher and the account page work unchanged.
//
// Uses the WebSocket Hibernation API: who each socket is lives in its attachment, so the object can sleep between
// messages (free plan friendly) and rebuild its maps on wake.
import { DurableObject } from 'cloudflare:workers';
import { validateMessage } from '../../relay/transport_security.js';
import { Social } from '../../relay/social.js';

const MAIN_ID = 'main';
const MAX_PER_ADDRESS = 16;          // connections from one address (as the Node relay)
const MAX_CONNECTIONS = 128, MAX_ROOMS = 128, MAX_PAYLOAD = 16 * 1024;
const AUTH_TIMEOUT = 15000, MAX_OUTGOING = 128 * 1024, MAX_BUFFERED = 1024 * 1024;
const MSG_RATE = 120, MSG_BURST = 240;
const BYTE_RATE = 256 * 1024, BYTE_BURST = 512 * 1024;
const utf8 = new TextEncoder();
const COLORS = ['#ff4444', '#44ff44', '#4444ff', '#ffff44', '#ff44ff', '#44ffff', '#ffaa44', '#aa44ff'];

async function sha256hex(text) {
  const d = await crypto.subtle.digest('SHA-256', new TextEncoder().encode(text));
  return [...new Uint8Array(d)].map(b => b.toString(16).padStart(2, '0')).join('');
}

export class World extends DurableObject {
  constructor(ctx, env) {
    super(ctx, env);
    this.mainName = (env.MAIN_LOBBY || 'Main Lobby').trim().slice(0, 50) || 'Main Lobby';
    this.rooms = new Map();          // id -> { id, name, listed, open, maxPlayers, hostId, status }
    this.roomsDirty = false;
    this.social = null;
    ctx.blockConcurrencyWhile(async () => {
      const rooms = await ctx.storage.get('rooms');
      if (rooms) for (const r of rooms) this.rooms.set(r.id, r);
      this.rooms.set(MAIN_ID, { id: MAIN_ID, name: this.mainName, listed: true, open: true, maxPlayers: 0, status: 'waiting' });
      const state = await ctx.storage.get('social');
      this.social = new Social({ state, store: s => { this.pendingSocial = s; } });
    });
  }

  // ---------- connection bookkeeping (survives hibernation through attachments) ----------
  info(ws) { return ws.deserializeAttachment() || {}; }
  setInfo(ws, info) { ws.serializeAttachment(info); }
  sockets() { return this.ctx.getWebSockets(); }
  clients() { return this.sockets().map(ws => this.info(ws)).filter(c => c.id && c.username); }
  inRoom(id) { return this.sockets().filter(ws => this.info(ws).gameId === id); }
  playerCount(id) { return new Set(this.inRoom(id).map(ws => this.info(ws).userId)).size; }
  send(ws, obj) {
    try {
      const raw = JSON.stringify(obj);
      if (utf8.encode(raw).length > MAX_OUTGOING || (Number.isFinite(ws.bufferedAmount) && ws.bufferedAmount > MAX_BUFFERED)) {
        ws.close(1009, 'Outgoing queue budget'); return;
      }
      ws.send(raw);
    } catch (_) { try { ws.close(1011, 'Send failed'); } catch (_) { /* closed */ } }
  }
  wsOf(client) { return this.sockets().find(ws => this.info(ws).id === client.id); }

  async persist() {
    const writes = {};
    if (this.roomsDirty) { writes.rooms = [...this.rooms.values()].filter(r => r.id !== MAIN_ID); this.roomsDirty = false; }
    if (this.pendingSocial) { writes.social = this.pendingSocial; this.pendingSocial = null; }
    if (!Object.keys(writes).length) return;
    await this.ctx.storage.put(writes);
  }

  // ---------- HTTP ----------
  async fetch(request) {
    const url = new URL(request.url);
    if (url.pathname === '/api/games') return Response.json({ games: this.listing() });
    if (url.pathname !== '/ws' || request.headers.get('Upgrade')?.toLowerCase() !== 'websocket') return new Response('WebSocket expected', { status: 426 });
    const origin = request.headers.get('Origin');
    if (origin !== null && origin !== url.origin) return new Response('Origin refused', { status: 403 });
    if (this.sockets().length >= MAX_CONNECTIONS) return new Response('Server connection quota', { status: 429 });
    const ip = request.headers.get('CF-Connecting-IP') || 'unknown';
    if (this.sockets().filter(ws => this.info(ws).ip === ip).length >= MAX_PER_ADDRESS) {
      return new Response('Address connection quota', { status: 429 });
    }
    const pair = new WebSocketPair();
    const [client, server] = Object.values(pair);
    this.ctx.acceptWebSocket(server);
    this.setInfo(server, { ip, at: Date.now() });
    // One alarm drains unauthenticated connections, including ones that never send a message.
    if (await this.ctx.storage.getAlarm() === null) await this.ctx.storage.setAlarm(Date.now() + AUTH_TIMEOUT);
    return new Response(null, { status: 101, webSocket: client });
  }

  listing() {
    return [...this.rooms.values()].filter(r => r.listed !== false).map(r => ({
      id: r.id, name: r.name, host_username: r.hostName || r.name, status: r.status || 'waiting',
      max_players: r.maxPlayers || 0, player_count: this.playerCount(r.id), region: 'global', open: !!r.open || !r.maxPlayers
    }));
  }
  gameList(target) {
    const msg = { type: 'gameList', games: this.listing() };
    if (target) return this.send(target, msg);
    for (const ws of this.sockets()) if (this.info(ws).id) this.send(ws, msg);
  }
  broadcast(roomId, obj, except) { for (const ws of this.inRoom(roomId)) if (ws !== except) this.send(ws, obj); }

  allowed(ws, size) {
    const now = Date.now(), info = this.info(ws);
    if (!info.id && now - info.at >= AUTH_TIMEOUT) return false;
    const b = info.budget || { tokens: MSG_BURST, bytes: BYTE_BURST, at: now };
    const elapsed = Math.max(0, now - b.at);
    b.tokens = Math.min(MSG_BURST, b.tokens + elapsed * MSG_RATE / 1000);
    b.bytes = Math.min(BYTE_BURST, b.bytes + elapsed * BYTE_RATE / 1000); b.at = now;
    if (b.tokens < 1 || b.bytes < size) return false;
    b.tokens -= 1; b.bytes -= size;
    info.budget = b; this.setInfo(ws, info); // Survives Durable Object hibernation.
    return true;
  }

  async alarm() {
    let next = Infinity;
    for (const ws of this.sockets()) {
      const info = this.info(ws);
      if (info.id) continue;
      if (Date.now() - info.at >= AUTH_TIMEOUT) { try { ws.close(1008, 'Authentication timed out'); } catch (_) {} }
      else next = Math.min(next, info.at + AUTH_TIMEOUT);
    }
    if (Number.isFinite(next)) await this.ctx.storage.setAlarm(next);
  }

  async roomIdFor(name) {
    const key = (name || '').trim().toLowerCase();
    if (!key || key === this.mainName.toLowerCase()) return MAIN_ID;
    return 'r-' + (await sha256hex(key)).slice(0, 20);
  }

  // ---------- WebSocket ----------
  async webSocketMessage(ws, raw) {
    if (typeof raw !== 'string' || utf8.encode(raw).length > MAX_PAYLOAD) return ws.close(1009, 'Message too large');
    if (!this.allowed(ws, utf8.encode(raw).length)) return ws.close(1008, 'Message rate or authentication limit');
    let parsed;
    try { parsed = validateMessage(JSON.parse(raw), 0); } catch (_) { return ws.close(1007, 'Invalid JSON'); }
    if (!parsed.ok) return ws.close(parsed.code || 1008, parsed.reason);
    const data = parsed.message;
    let me = this.info(ws);
    switch (data.type) {
      case 'join': {
        if (me.id) return this.send(ws, { type: 'error', error: 'Already authenticated' });
        const id = crypto.randomUUID();
        me = { ...me, id, userId: 'guest-' + id, username: data.name, type: data.clientType || 'browser', gameId: null };
        this.setInfo(ws, me);
        this.send(ws, { type: 'auth_ok', user: { id: me.userId, username: me.username } });
        break;
      }
      case 'auth':
        this.send(ws, { type: 'error', error: 'Accounts are not available on this server yet; play as a guest.' });
        ws.close(1008, 'Accounts unavailable');
        break;
      case 'list':
        if (me.id) this.gameList(ws);
        break;
      case 'enter_room': case 'join_game': case 'create_game': {
        if (!me.id) return;
        let id;
        if (data.type === 'enter_room') id = await this.roomIdFor(data.name);
        else if (data.type === 'join_game') id = data.gameId;
        else id = 'g-' + crypto.randomUUID().slice(0, 8);
        if (!this.rooms.has(id) && this.rooms.size >= MAX_ROOMS) return this.send(ws, { type: 'error', error: 'Room quota reached.' });
        if (data.type === 'create_game') {
          this.rooms.set(id, { id, name: data.name, listed: true, open: false, maxPlayers: data.maxPlayers || 0,
            hostId: me.userId, hostName: me.username, status: 'waiting' });
          this.roomsDirty = true;
        } else if (data.type === 'enter_room' && !this.rooms.has(id)) {
          this.rooms.set(id, { id, name: data.name, listed: false, open: true, maxPlayers: 0, status: 'waiting' });
          this.roomsDirty = true;
        }
        const room = this.rooms.get(id);
        if (!room) return this.send(ws, { type: 'error', error: 'Game not found or not waiting' });
        if (me.gameId === id) return this.send(ws, { type: 'error', error: 'Already in that room.' });
        if (room.maxPlayers && this.playerCount(id) >= room.maxPlayers) return this.send(ws, { type: 'error', error: 'Game is full' });
        if (me.gameId) this.leave(ws);
        me = this.info(ws);
        const color = COLORS[Math.floor(Math.random() * COLORS.length)];
        me.gameId = id; me.color = color; this.setInfo(ws, me);
        const created = data.type === 'create_game';
        this.send(ws, { type: created ? 'game_created' : 'game_joined', role: created ? 'host' : 'player', color,
          game: { id, name: room.name, maxPlayers: room.maxPlayers || 0, playerCount: this.playerCount(id), region: 'global',
            open: !room.maxPlayers, main: id === MAIN_ID, listed: room.listed !== false, hostUsername: room.hostName } });
        this.broadcast(id, { type: 'player_joined', username: me.username, color, playerCount: this.playerCount(id) }, ws);
        this.gameList();
        break;
      }
      case 'leave_game':
        if (me.gameId) { this.leave(ws); this.gameList(); }
        break;
      case 'start_game': {
        const room = me.gameId && this.rooms.get(me.gameId);
        if (!room || room.hostId !== me.userId) return;
        room.status = 'playing';
        this.roomsDirty = true;
        this.broadcast(room.id, { type: 'game_started', seed: data.seed || Date.now() });
        this.gameList();
        break;
      }
      case 'set_ready':
        if (me.gameId) this.broadcast(me.gameId, { type: 'player_ready', username: me.username, ready: data.ready !== false });
        break;
      case 'input': {
        if (!me.gameId) return;
        const msg = { type: 'input', from: me.username, fromId: me.id, data: data.data, ts: Date.now() };
        for (const other of this.inRoom(me.gameId)) {
          if (other === ws) continue;
          const o = this.info(other);
          if (o.type !== 'game' || (data.to && o.id !== data.to)) continue;
          this.send(other, msg);
        }
        break;
      }
      case 'chat':
        if (me.gameId) this.broadcast(me.gameId, { type: 'chat', from: me.username, text: data.text, ts: Date.now() });
        break;
      case 'ping':
        this.send(ws, { type: 'pong', ts: data.ts });
        break;
      default: {
        if (!me.id) return;
        const client = { ...me };
        this.social.handle(client, data, {
          send: obj => this.send(ws, obj),
          sendTo: (c, obj) => { const s = this.wsOf(c); if (s) this.send(s, obj); },
          clients: () => this.clients(),
          rooms: () => [...this.rooms.values()].map(r => ({ id: r.id, name: r.name, listed: r.listed !== false,
            players: this.inRoom(r.id).map(s => this.info(s)) }))
        });
      }
    }
    await this.persist();
  }

  leave(ws) {
    const me = this.info(ws);
    const id = me.gameId;
    if (!id) return;
    me.gameId = null; this.setInfo(ws, me);
    const room = this.rooms.get(id);
    const left = this.playerCount(id);
    this.broadcast(id, { type: 'player_left', username: me.username, id: me.id, playerCount: left });
    if (room && id !== MAIN_ID && left === 0) { this.rooms.delete(id); this.roomsDirty = true; }
    else if (room && room.hostId === me.userId) {
      const next = this.inRoom(id)[0];
      if (next) { const n = this.info(next); room.hostId = n.userId; room.hostName = n.username;
        this.roomsDirty = true;
        this.broadcast(id, { type: 'host_transferred', newHost: n.username }); }
    }
  }

  async webSocketClose(ws) { if (this.info(ws).gameId) { this.leave(ws); this.gameList(); } await this.persist(); }
  async webSocketError(ws) { await this.webSocketClose(ws); }
}
