// The online world as one Durable Object: every WebSocket, the Main Lobby and named rooms, the game tunnels' race
// traffic, and the social layer (presence, car clubs, club chat, rankings, race points). It speaks the same protocol
// as tools/multiplayer/relay/server.js, so proxy.py, the launcher and the account page work unchanged.
//
// Uses the WebSocket Hibernation API: who each socket is lives in its attachment, so the object can sleep between
// messages (free plan friendly) and rebuild its maps on wake.
import { DurableObject } from 'cloudflare:workers';
import { validateMessage } from '../../relay/transport_security.js';
import { Social } from '../../relay/social.js';
import { Accounts } from './accounts.js';

const MAIN_ID = 'main';
const MAX_PER_ADDRESS = 16;          // connections from one address (as the Node relay)
const MAX_ROOMS = 1024, MAX_PAYLOAD = 16 * 1024;
// Scale (docs/notes/online_scaling.md): players signed in at once (env MAX_PLAYERS), then a queue; the Main Lobby is
// instances of MAIN_CAP players (a System Link race is 8 at most), a new player goes to the fullest one with room.
// Measured with load_test.mjs: one World object relays about 20,000 copies a second; 500 players in lobbies of 32 stay
// under that (a Durable Object per room is the next step, docs/notes/online_scaling.md)
const DEFAULT_MAX_PLAYERS = 500, MAX_QUEUE = 2000, MAIN_CAP = 32, LIST_EVERY_MS = 1000;
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
    this.maxPlayers = Math.max(1, Number(env.MAX_PLAYERS) || DEFAULT_MAX_PLAYERS);
    this.maxPerAddress = Math.max(1, Number(env.MAX_PER_ADDRESS) || MAX_PER_ADDRESS);  // raised only for local load tests
    // In memory, rebuilt from the sockets' attachments when the object wakes: who each socket is, and each room's
    // sockets, so relaying a datagram touches its room only (not every connection on the server)
    this.cache = new Map();   // ws -> info
    this.roomOf = new Map();  // ws -> room id
    this.members = null;      // room id -> Set(ws)
    this.listTimer = null;
    this.queueAt = 0;         // order for queued sign-ins
    // rooms on objects of their own (room.js): one-use tickets for /ws/room/<id>, and how many clients each room has
    // that still send through the World (told to the room so it forwards to them only when needed)
    this.tickets = new Map();     // ticket -> { roomId, id, userId, username, type, exp }
    this.legacySent = new Map();  // room id -> count last told to that room
    this.rooms = new Map();          // id -> { id, name, listed, open, maxPlayers, hostId, status }
    this.roomsDirty = false;
    this.social = null;
    // Accounts and cloud saves live in this object's SQLite (absent in the unit-test stub).
    const creators = String(env.CREATOR_NAMES || '').split(',').map(n => n.trim()).filter(Boolean);
    this.accounts = ctx.storage.sql ? new Accounts(ctx.storage.sql, { creators }) : null;
    if (this.accounts) {
      this.accounts.transactionSync = fn => ctx.storage.transactionSync(fn);
      this.accounts.onDelete = id => this.forgetAccount(id);
    }
    ctx.blockConcurrencyWhile(async () => {
      const rooms = await ctx.storage.get('rooms');
      if (rooms) for (const r of rooms) this.rooms.set(r.id, r);
      for (const r of [...this.rooms.values()]) if (r.main) this.rooms.delete(r.id);  // instances are not kept
      this.rooms.set(MAIN_ID, { id: MAIN_ID, name: this.mainName, listed: true, open: true, maxPlayers: MAIN_CAP, status: 'waiting', main: true });
      const state = await ctx.storage.get('social');
      this.social = new Social({ state, store: s => { this.pendingSocial = s; } });
    });
  }

  // ---------- connection bookkeeping (survives hibernation through attachments) ----------
  info(ws) {
    let i = this.cache.get(ws);
    if (!i) { i = ws.deserializeAttachment() || {}; this.cache.set(ws, i); }
    return i;
  }
  setInfo(ws, info) {
    ws.serializeAttachment(info);
    this.cache.set(ws, info);
    this.place(ws, info.gameId || null);
  }
  index() {
    if (!this.members) {
      this.members = new Map();
      for (const ws of this.ctx.getWebSockets()) this.place(ws, this.info(ws).gameId || null);
    }
    return this.members;
  }
  place(ws, to) {
    const m = this.index(), from = this.roomOf.get(ws) || null;
    if (from === to) return;
    if (from) { const set = m.get(from); if (set) { set.delete(ws); if (!set.size) m.delete(from); } this.roomOf.delete(ws); }
    if (to) { if (!m.has(to)) m.set(to, new Set()); m.get(to).add(ws); this.roomOf.set(ws, to); }
  }
  forgetSocket(ws) { this.place(ws, null); this.cache.delete(ws); }
  sockets() { return this.ctx.getWebSockets(); }
  clients() { return this.sockets().map(ws => this.info(ws)).filter(c => c.id && c.username); }
  inRoom(id) { return [...(this.index().get(id) || [])]; }
  playersOnline() { let n = 0; for (const ws of this.sockets()) if (this.info(ws).id) n++; return n; }
  queued() { return this.sockets().filter(ws => this.info(ws).queued).sort((a, b) => this.info(a).queuedAt - this.info(b).queuedAt); }
  // a signed-in client's welcome, or a place in the queue when the server is full
  admit(ws, me, reply) {
    if (this.playersOnline() < this.maxPlayers && !this.queued().length) { this.setInfo(ws, me); this.send(ws, reply); return; }
    const waiting = this.queued();
    if (waiting.length >= MAX_QUEUE) { this.send(ws, { type: 'error', error: 'Server full, try again soon.' }); ws.close(1013, 'Server full'); return; }
    const info = { ...this.info(ws), queued: true, queuedAt: ++this.queueAt + Date.now() * 1000, pending: { me, reply } };
    this.setInfo(ws, info);
    this.send(ws, { type: 'queue', pos: waiting.length + 1, of: waiting.length + 1, max: this.maxPlayers });
  }
  // places freed: admit from the front of the queue, tell the rest their place
  promote() {
    const waiting = this.queued();
    if (!waiting.length) return;
    let free = this.maxPlayers - this.playersOnline();
    while (free > 0 && waiting.length) {
      const ws = waiting.shift(), { pending } = this.info(ws);
      this.setInfo(ws, { ...pending.me, ip: this.info(ws).ip, at: this.info(ws).at });
      this.send(ws, pending.reply);
      free--;
    }
    waiting.forEach((ws, i) => this.send(ws, { type: 'queue', pos: i + 1, of: waiting.length, max: this.maxPlayers }));
  }
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
  sendRaw(ws, raw) {
    try {
      if (Number.isFinite(ws.bufferedAmount) && ws.bufferedAmount > MAX_BUFFERED) { ws.close(1009, 'Outgoing queue budget'); return; }
      ws.send(raw);
    } catch (_) { try { ws.close(1011, 'Send failed'); } catch (_) { /* closed */ } }
  }
  wsOf(client) { return this.sockets().find(ws => this.info(ws).id === client.id); }

  async persist() {
    const writes = {};
    if (this.roomsDirty) { writes.rooms = [...this.rooms.values()].filter(r => r.id !== MAIN_ID && !r.main); this.roomsDirty = false; }
    if (this.pendingSocial) { writes.social = this.pendingSocial; this.pendingSocial = null; }
    if (!Object.keys(writes).length) return;
    await this.ctx.storage.put(writes);
  }

  // ---------- HTTP ----------
  async fetch(request) {
    const url = new URL(request.url);
    if (url.pathname === '/api/games') return Response.json({ games: this.listing() });
    if (url.pathname === '/api/players') {
      const players = this.playersOnline(), queue = this.queued().length;
      return Response.json({ players, queue, max: this.maxPlayers, full: players >= this.maxPlayers,
        rooms: [...this.rooms.values()].filter(r => r.listed !== false).length,
        lobbies: [...this.rooms.values()].filter(r => r.main).map(r => ({ name: r.name, players: this.playerCount(r.id) })) });
    }
    if (url.pathname.startsWith('/api/')) {
      const answer = this.accounts && await this.accounts.handle(request);
      return answer || Response.json({ error: 'Not found.' }, { status: 404 });
    }
    if (url.pathname !== '/ws' || request.headers.get('Upgrade')?.toLowerCase() !== 'websocket') return new Response('WebSocket expected', { status: 426 });
    const origin = request.headers.get('Origin');
    if (origin !== null && origin !== url.origin) return new Response('Origin refused', { status: 403 });
    if (this.sockets().length >= this.maxPlayers + MAX_QUEUE) return new Response('Server connection quota', { status: 429 });
    const ip = request.headers.get('CF-Connecting-IP') || 'unknown';
    if (this.sockets().filter(ws => this.info(ws).ip === ip).length >= this.maxPerAddress) {
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
      id: r.id, name: r.name, main: !!r.main, host_username: r.main ? '' : r.hostName || r.name, host_badge: r.hostBadge || null, status: r.status || 'waiting',
      max_players: r.maxPlayers || 0, player_count: this.playerCount(r.id), region: 'global', open: !!r.open || !r.maxPlayers
    }));
  }
  // a client asking gets the list at once; room changes reach everyone at most once a second (each change used to send
  // the whole list to every connection)
  gameList(target) {
    if (target) return this.send(target, { type: 'gameList', games: this.listing() });
    if (this.listTimer) return;
    this.listTimer = setTimeout(() => {
      this.listTimer = null;
      const msg = { type: 'gameList', games: this.listing() };
      for (const ws of this.sockets()) if (this.info(ws).id) this.send(ws, msg);
    }, LIST_EVERY_MS);
  }
  broadcast(roomId, obj, except) { for (const ws of this.inRoom(roomId)) if (ws !== except) this.send(ws, obj); }

  allowed(ws, size) {
    const now = Date.now(), info = this.info(ws);
    if (!info.id && !info.queued && now - info.at >= AUTH_TIMEOUT) return false;
    const b = info.budget || { tokens: MSG_BURST, bytes: BYTE_BURST, at: now };
    const elapsed = Math.max(0, now - b.at);
    b.tokens = Math.min(MSG_BURST, b.tokens + elapsed * MSG_RATE / 1000);
    b.bytes = Math.min(BYTE_BURST, b.bytes + elapsed * BYTE_RATE / 1000); b.at = now;
    if (b.tokens < 1 || b.bytes < size) return false;
    b.tokens -= 1; b.bytes -= size;
    info.budget = b;
    // kept in the attachment to survive hibernation, written at most once a second per socket (not every datagram)
    if (!(now - (info.budgetSaved || 0) < 1000)) { info.budgetSaved = now; this.setInfo(ws, info); } else this.cache.set(ws, info);
    return true;
  }

  async alarm() {
    let next = Infinity;
    for (const ws of this.sockets()) {
      const info = this.info(ws);
      if (info.id || info.queued) continue;
      if (Date.now() - info.at >= AUTH_TIMEOUT) { try { ws.close(1008, 'Authentication timed out'); } catch (_) {} }
      else next = Math.min(next, info.at + AUTH_TIMEOUT);
    }
    if (Number.isFinite(next)) await this.ctx.storage.setAlarm(next);
  }

  async roomIdFor(name) {
    const key = (name || '').trim().toLowerCase();
    if (!key || key === this.mainName.toLowerCase()) return this.mainInstance();
    return 'r-' + (await sha256hex(key)).slice(0, 20);
  }

  // ---------- rooms on their own objects (room.js) ----------
  ticketFor(me, roomId) {
    if (!this.env.ROOM || me.type !== 'game') return undefined;
    const now = Date.now();
    if (this.tickets.size > 4096) for (const [k, t] of this.tickets) if (t.exp < now) this.tickets.delete(k);
    const bytes = crypto.getRandomValues(new Uint8Array(16));
    const ticket = [...bytes].map(b => b.toString(16).padStart(2, '0')).join('');
    this.tickets.set(ticket, { roomId, id: me.id, userId: me.userId, username: me.username, type: me.type, exp: now + 60000 });
    return '/ws/room/' + encodeURIComponent(roomId) + '?t=' + ticket;
  }
  // a room object checking a client's ticket (RPC): its identity once, and the client counts as on its room socket
  async claimTicket(ticket, roomId) {
    const t = this.tickets.get(ticket);
    this.tickets.delete(ticket);
    if (!t || t.roomId !== roomId || t.exp < Date.now()) return null;
    const ws = this.sockets().find(s => this.info(s).id === t.id);
    if (!ws || this.info(ws).gameId !== roomId) return null;  // left since
    this.setInfo(ws, { ...this.info(ws), roomSocket: roomId });
    this.tellLegacy(roomId);
    return { id: t.id, userId: t.userId, username: t.username, type: t.type };
  }
  legacyCount(roomId) {
    let n = 0;
    for (const ws of this.inRoom(roomId)) { const i = this.info(ws); if (i.type === 'game' && i.roomSocket !== roomId) n++; }
    return n;
  }
  tellLegacy(roomId) {
    if (!this.env.ROOM || !roomId) return;
    const n = this.legacyCount(roomId);
    if (this.legacySent.get(roomId) === n) return;
    this.legacySent.set(roomId, n);
    if (!n && !this.inRoom(roomId).length) this.legacySent.delete(roomId);
    this.env.ROOM.get(this.env.ROOM.idFromName(roomId)).setLegacy(n).catch(() => this.legacySent.delete(roomId));
  }
  // a room object's datagram for the room's clients without a room socket (RPC)
  async forward(roomId, raw, to, fromId) {
    for (const ws of this.inRoom(roomId)) {
      const i = this.info(ws);
      if (i.type !== 'game' || i.roomSocket === roomId || i.id === fromId || (to && i.id !== to)) continue;
      this.sendRaw(ws, raw);
    }
  }

  // the fullest Main Lobby instance with room (busy lobbies), else a new one
  mainInstance() {
    let best = null, most = -1;
    for (const r of this.rooms.values()) {
      if (!r.main) continue;
      const n = this.playerCount(r.id);
      if (n < MAIN_CAP && n > most) { best = r; most = n; }
    }
    if (best) return best.id;
    let k = 2;
    while (this.rooms.has('main-' + k)) k++;
    const id = 'main-' + k;
    this.rooms.set(id, { id, name: `${this.mainName} ${k}`, listed: true, open: true, maxPlayers: MAIN_CAP, status: 'waiting', main: true });
    return id;
  }

  // ---------- WebSocket ----------
  async webSocketMessage(ws, raw) {
    // UTF-8 size without encoding: at most 3 bytes per UTF-16 unit, exact only when it matters
    const size = typeof raw !== 'string' ? Infinity : raw.length * 3 <= MAX_PAYLOAD ? raw.length : utf8.encode(raw).length;
    if (size > MAX_PAYLOAD) return ws.close(1009, 'Message too large');
    if (!this.allowed(ws, size)) return ws.close(1008, 'Message rate or authentication limit');
    let parsed;
    try { parsed = validateMessage(JSON.parse(raw), 0); } catch (_) { return ws.close(1007, 'Invalid JSON'); }
    if (!parsed.ok) return ws.close(parsed.code || 1008, parsed.reason);
    const data = parsed.message;
    let me = this.info(ws);
    switch (data.type) {
      case 'join': {
        if (me.id) return this.send(ws, { type: 'error', error: 'Already authenticated' });
        // a guest never appears under a registered player's name (badges and rankings follow names)
        const name = this.accounts && this.accounts.isRegisteredName(data.name) ? data.name.trim().slice(0, 40) + ' (guest)' : data.name;
        const id = crypto.randomUUID();
        if (me.queued) return;
        me = { ...me, id, userId: 'guest-' + id, username: name, type: data.clientType || 'browser', gameId: null };
        this.admit(ws, me, { type: 'auth_ok', user: { id: me.userId, username: me.username } });
        break;
      }
      case 'auth': {
        if (me.id) return this.send(ws, { type: 'error', error: 'Already authenticated' });
        const v = this.accounts && await this.accounts.verifyToken(data.token);
        if (!v) { this.send(ws, { type: 'error', error: 'Invalid token' }); return ws.close(1008, 'Invalid token'); }
        if (me.queued) return;
        me = { ...this.info(ws), id: crypto.randomUUID(), userId: v.id, username: v.username, sid: v.sid,
          badge: v.badge || null, type: data.clientType || 'browser', gameId: null };
        this.admit(ws, me, { type: 'auth_ok', user: { id: v.id, username: v.username, badge: me.badge } });
        break;
      }
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
            hostId: me.userId, hostName: me.username, hostBadge: me.badge || null, status: 'waiting' });
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
        const roomWs = this.ticketFor(me, id);
        this.send(ws, { type: created ? 'game_created' : 'game_joined', role: created ? 'host' : 'player', color, roomWs,
          game: { id, name: room.name, maxPlayers: room.maxPlayers || 0, playerCount: this.playerCount(id), region: 'global',
            open: !room.maxPlayers || !!room.main, main: !!room.main, listed: room.listed !== false, hostUsername: room.hostName } });
        this.broadcast(id, { type: 'player_joined', username: me.username, badge: me.badge || null, color, playerCount: this.playerCount(id) }, ws);
        this.tellLegacy(id);
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
        // encoded once for the whole room: each copy only checks the receiver's queue
        const raw2 = JSON.stringify({ type: 'input', from: me.username, fromId: me.id, data: data.data, ts: Date.now() });
        if (raw2.length * 3 > MAX_OUTGOING && utf8.encode(raw2).length > MAX_OUTGOING) break;
        for (const other of this.inRoom(me.gameId)) {
          if (other === ws) continue;
          const o = this.info(other);
          if (o.type !== 'game' || (data.to && o.id !== data.to)) continue;
          this.sendRaw(other, raw2);
        }
        break;
      }
      case 'chat':
        if (me.gameId) this.broadcast(me.gameId, { type: 'chat', from: me.username, fromBadge: me.badge || null, text: data.text, ts: Date.now() });
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

  // A deleted account leaves the rankings and its club (ownership passes on; an empty club goes) and is disconnected.
  async forgetAccount(id) {
    const st = this.social.state, k = 'u:' + id, me = st.players[k];
    const club = me && me.club && st.clubs[me.club];
    if (club) {
      club.members = club.members.filter(x => x !== k);
      if (!club.members.length) delete st.clubs[club.id];
      else if (club.owner === k) club.owner = club.members[0];
    }
    delete st.players[k];
    this.social.save();
    for (const ws of this.sockets()) if (this.info(ws).userId === id) { try { ws.close(1008, 'Account deleted'); } catch (_) {} }
    await this.persist();
  }

  leave(ws) {
    const me = this.info(ws);
    const id = me.gameId;
    if (!id) return;
    me.gameId = null; me.roomSocket = null; this.setInfo(ws, me);
    this.tellLegacy(id);
    const room = this.rooms.get(id);
    const left = this.playerCount(id);
    this.broadcast(id, { type: 'player_left', username: me.username, id: me.id, playerCount: left });
    if (room && id !== MAIN_ID && left === 0) { this.rooms.delete(id); this.roomsDirty = true; }
    else if (room && room.hostId === me.userId) {
      const next = this.inRoom(id)[0];
      if (next) { const n = this.info(next); room.hostId = n.userId; room.hostName = n.username; room.hostBadge = n.badge || null;
        this.roomsDirty = true;
        this.broadcast(id, { type: 'host_transferred', newHost: n.username, newHostBadge: n.badge || null }); }
    }
  }

  async webSocketClose(ws) {
    const wasPlayer = !!this.info(ws).id, wasQueued = !!this.info(ws).queued;
    if (this.info(ws).gameId) { this.leave(ws); this.gameList(); }
    this.forgetSocket(ws);
    if (wasPlayer || wasQueued) this.promote();
    await this.persist();
  }
  async webSocketError(ws) { await this.webSocketClose(ws); }
}
