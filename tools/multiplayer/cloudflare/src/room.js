// One room's game traffic on a Durable Object of its own (docs/notes/online_scaling.md, step 2). The World object keeps
// sign-in, rooms, lists and the social layer; once a client is in a room the World hands it a one-use ticket and
// /ws/room/<room id>, and the client sends that room's System Link datagrams here, so rooms relay in parallel instead of
// on the World's one thread.
//
// Older clients still send datagrams to the World, which relays them to everyone in the room as before (clients with a
// room socket read both sockets). Datagrams from room sockets reach those older clients through World.forward, called
// only while the World says the room has some (setLegacy), so an up-to-date room costs the World nothing.
//
// Messages here: {"type":"input","data":...,"to"?} as on the World (relayed as {"type":"input","from","fromId","data",
// "ts"}), {"type":"ping"}. Same size and rate limits as the World.
import { DurableObject } from 'cloudflare:workers';
import { validateMessage } from '../../relay/transport_security.js';

export const WORLD_NAME = 'mc3-world-2';  // worker.js addresses the World by this name
const MAX_PAYLOAD = 16 * 1024, MAX_BUFFERED = 1024 * 1024;
const MSG_RATE = 120, MSG_BURST = 240, BYTE_RATE = 256 * 1024, BYTE_BURST = 512 * 1024;
const utf8 = new TextEncoder();

export class Room extends DurableObject {
  constructor(ctx, env) {
    super(ctx, env);
    this.cache = new Map();  // ws -> {id, userId, username, type}
    this.budgets = new Map();
    this.legacy = 0;         // clients in this room still sending through the World
    this.roomId = null;
    ctx.blockConcurrencyWhile(async () => {
      this.legacy = (await ctx.storage.get('legacy')) || 0;
      this.roomId = (await ctx.storage.get('id')) || null;
    });
  }

  world() { return this.env.WORLD.get(this.env.WORLD.idFromName(WORLD_NAME)); }
  info(ws) {
    let i = this.cache.get(ws);
    if (!i) { i = ws.deserializeAttachment() || {}; this.cache.set(ws, i); }
    return i;
  }

  async fetch(request) {
    const url = new URL(request.url);
    if (request.headers.get('Upgrade')?.toLowerCase() !== 'websocket') return new Response('WebSocket expected', { status: 426 });
    const origin = request.headers.get('Origin');
    if (origin !== null && origin !== url.origin) return new Response('Origin refused', { status: 403 });
    const roomId = decodeURIComponent(url.pathname.slice('/ws/room/'.length));
    const ticket = url.searchParams.get('t') || '';
    if (!roomId || roomId.length > 64 || !/^[0-9a-f]{32}$/.test(ticket)) return new Response('Bad room ticket', { status: 400 });
    const who = await this.world().claimTicket(ticket, roomId);
    if (!who) return new Response('Room ticket refused', { status: 403 });
    if (this.roomId !== roomId) { this.roomId = roomId; await this.ctx.storage.put('id', roomId); }
    const [client, server] = Object.values(new WebSocketPair());
    this.ctx.acceptWebSocket(server);
    const info = { id: who.id, userId: who.userId, username: who.username, type: who.type };
    server.serializeAttachment(info);
    this.cache.set(server, info);
    return new Response(null, { status: 101, webSocket: client });
  }

  // the World: how many clients in this room still send through it
  async setLegacy(n) {
    if (this.legacy === n) return;
    this.legacy = n;
    await this.ctx.storage.put('legacy', n);
  }

  allowed(ws, size) {
    const now = Date.now(), b = this.budgets.get(ws) || { tokens: MSG_BURST, bytes: BYTE_BURST, at: now };
    const elapsed = Math.max(0, now - b.at);
    b.tokens = Math.min(MSG_BURST, b.tokens + elapsed * MSG_RATE / 1000);
    b.bytes = Math.min(BYTE_BURST, b.bytes + elapsed * BYTE_RATE / 1000);
    b.at = now;
    this.budgets.set(ws, b);
    if (b.tokens < 1 || b.bytes < size) return false;
    b.tokens -= 1; b.bytes -= size;
    return true;
  }
  sendRaw(ws, raw) {
    try {
      if (Number.isFinite(ws.bufferedAmount) && ws.bufferedAmount > MAX_BUFFERED) { ws.close(1009, 'Outgoing queue budget'); return; }
      ws.send(raw);
    } catch (_) { try { ws.close(1011, 'Send failed'); } catch (_) { /* closed */ } }
  }

  async webSocketMessage(ws, raw) {
    const size = typeof raw !== 'string' ? Infinity : raw.length * 3 <= MAX_PAYLOAD ? raw.length : utf8.encode(raw).length;
    if (size > MAX_PAYLOAD) return ws.close(1009, 'Message too large');
    if (!this.allowed(ws, size)) return ws.close(1008, 'Message rate limit');
    let parsed;
    try { parsed = validateMessage(JSON.parse(raw), 0); } catch (_) { return ws.close(1007, 'Invalid JSON'); }
    if (!parsed.ok) return ws.close(parsed.code || 1008, parsed.reason);
    const data = parsed.message, me = this.info(ws);
    if (data.type === 'ping') return this.sendRaw(ws, JSON.stringify({ type: 'pong', ts: data.ts }));
    if (data.type !== 'input' || !me.id) return;
    const out = JSON.stringify({ type: 'input', from: me.username, fromId: me.id, data: data.data, ts: Date.now() });
    for (const other of this.ctx.getWebSockets()) {
      if (other === ws) continue;
      const o = this.info(other);
      if (o.type !== 'game' || (data.to && o.id !== data.to)) continue;
      this.sendRaw(other, out);
    }
    if (this.legacy > 0 && this.roomId) {
      try { await this.world().forward(this.roomId, out, data.to || null, me.id); } catch (_) { /* the World is busy: older clients miss one datagram */ }
    }
  }

  async webSocketClose(ws, code = 1000) {
    this.cache.delete(ws);
    this.budgets.delete(ws);
    const closeCode = [1000, 1001, 1002, 1003, 1007, 1008, 1009, 1010, 1011, 1012, 1013, 1014].includes(code) ||
      (Number.isInteger(code) && code >= 3000 && code <= 4999) ? code : 1000;
    try { ws.close(closeCode, 'Connection closed'); } catch (_) { /* already closed */ }
  }
  async webSocketError(ws) { await this.webSocketClose(ws); }
}
