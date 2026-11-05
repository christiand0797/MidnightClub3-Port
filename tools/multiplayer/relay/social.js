'use strict';
// The online world's social layer: who is online where, car clubs with their own chat, and rankings.
//
// Same for browser and local players: everything here rides the relay's WebSocket, which both the browser page and
// the game's tunnel (proxy.py) use.
//
// Rankings come only from what the game itself sends, confirmed by more than one racer. Every tunnel reports the
// game's race messages it sees (proxy/mcnet.py: RACE_STATE, RACE_CONFIG, CHECKPOINT, PLAYER_SCORE). The host
// broadcasts RACE_STATE 5 when a race goes green, so its tunnel (sending) and every other racer's tunnel (receiving)
// report the same start. A race counts once at least two different players in the room report it within
// RACE_WINDOW_MS; each of them earns RACE_POINTS. One tunnel alone cannot award itself anything. Finishing
// positions are not scored yet: the game's CHECKPOINT/PLAYER_SCORE bodies have not been decoded, so the raw messages
// players' tunnels report are kept (research log) to decode them from real races.
const crypto = require('crypto');
const { RaceResults } = require('./race_results');
// fs/path only for the Node relay's file storage; the Cloudflare worker passes load/store instead
const nodeFs = () => ({ fs: require('fs'), path: require('path') });

const RACE_POINTS = 10;
const RACE_WINDOW_MS = 30 * 1000;
const RACE_COOLDOWN_MS = 60 * 1000;
const CHAT_KEEP = 50;
const RESEARCH_KEEP = 2000;
const TYPES = ['presence', 'club_create', 'club_join', 'club_leave', 'club_list', 'club_info', 'club_chat',
  'leaderboard', 'race_event'];

const text = (v, min, max) => typeof v === 'string' && v.trim().length >= min && v.length <= max &&
  !/[\u0000-\u001f\u007f]/.test(v);
const id = v => typeof v === 'string' && /^[A-Za-z0-9_-]{1,64}$/.test(v);

// a message of one of TYPES in the shape the relay accepts, or null
function validateSocial(m) {
  if (!m || typeof m !== 'object' || !TYPES.includes(m.type)) return null;
  switch (m.type) {
    case 'presence': case 'club_leave': case 'club_list': return { type: m.type };
    case 'club_create':
      if (!text(m.name, 3, 32) || typeof m.tag !== 'string' || !/^[A-Za-z0-9]{2,5}$/.test(m.tag)) return null;
      return { type: m.type, name: m.name.trim(), tag: m.tag.toUpperCase() };
    case 'club_join': case 'club_info':
      return id(m.clubId) ? { type: m.type, clubId: m.clubId } : null;
    case 'club_chat':
      return text(m.text, 1, 300) ? { type: m.type, text: m.text.trim() } : null;
    case 'leaderboard':
      if (m.scope !== undefined && !['players', 'clubs'].includes(m.scope)) return null;
      return { type: m.type, scope: m.scope || 'players' };
    case 'race_event': {
      const e = m.event;
      if (!e || typeof e !== 'object' || !text(e.type, 1, 40) || typeof e.body !== 'string' ||
          !/^[0-9a-f]{0,2048}$/.test(e.body) || (e.dir !== undefined && !['in', 'out'].includes(e.dir)) ||
          (e.state !== undefined && (!Number.isSafeInteger(e.state) || e.state < 0 || e.state > 0xFFFFFFFF))) return null;
      return { type: m.type, event: { type: e.type, body: e.body, dir: e.dir || 'out', state: e.state } };
    }
  }
  return null;
}

class Social {
  // file: a JSON file (Node relay); or store(state) to persist elsewhere and state to start from (Cloudflare)
  constructor({ file, now = Date.now, state = null, store = null } = {}) {
    this.file = file;
    this.now = now;
    this.store = store;
    this.state = { version: 1, players: {}, clubs: {}, research: [] };
    if (state) Object.assign(this.state, state);
    if (file) {
      const { fs } = nodeFs();
      try { Object.assign(this.state, JSON.parse(fs.readFileSync(file, 'utf8'))); }
      catch (e) { if (e.code !== 'ENOENT') throw e; }
    }
    this.races = new Map();   // room -> { at, reporters: Set, confirmed }
    this.results = new RaceResults(now);  // finishing places from checkpoints (race_results.js)
    this.timer = null;
  }

  // ---------- identity and storage ----------
  key(client) {
    // a signed-in account keeps its standing everywhere; a guest is known by its display name
    return client.userId && !client.userId.startsWith('guest-') ? 'u:' + client.userId
      : 'g:' + String(client.username || 'guest').toLowerCase();
  }
  player(client) {
    const k = this.key(client);
    const p = this.state.players[k] || (this.state.players[k] = { name: client.username, points: 0, races: 0, club: null });
    p.name = client.username;
    p.lastSeen = this.now();
    return p;
  }
  save() {
    if (this.store) { this.store(this.state); return; }
    if (!this.file || this.timer) return;
    this.timer = setTimeout(() => {
      this.timer = null;
      const tmp = this.file + '.tmp';
      const { fs, path } = nodeFs();
      fs.mkdirSync(path.dirname(this.file), { recursive: true });
      fs.writeFileSync(tmp, JSON.stringify(this.state));
      fs.renameSync(tmp, this.file);
    }, 500);
    if (this.timer.unref) this.timer.unref();
  }
  flush() {
    if (!this.timer) return;
    clearTimeout(this.timer); this.timer = null;
    if (!this.file) return;
    const { fs } = nodeFs();
    fs.writeFileSync(this.file + '.tmp', JSON.stringify(this.state));
    fs.renameSync(this.file + '.tmp', this.file);
  }

  clubPoints(club) {
    return club.members.reduce((sum, k) => sum + ((this.state.players[k] || {}).points || 0), 0);
  }
  clubSummary(c) {
    return { id: c.id, name: c.name, tag: c.tag, members: c.members.length, points: this.clubPoints(c) };
  }

  // ---------- messages ----------
  // ctx: send(obj) to this client, rooms() -> [{ id, name, listed, players: [client] }], clients() -> [client],
  //      sendTo(client, obj)
  handle(client, m, ctx) {
    const me = this.player(client);
    const k = this.key(client);
    switch (m.type) {
      case 'presence': {
        const rooms = ctx.rooms().filter(r => r.listed || r.id === client.gameId).map(r => ({
          id: r.id, name: r.name, players: r.players.map(p => ({ name: p.username, club: this.tagOf(p) }))
        }));
        ctx.send({ type: 'presence', rooms, online: ctx.clients().length });
        break;
      }
      case 'club_create': {
        if (k.startsWith('g:')) return ctx.send({ type: 'error', error: 'Sign in to create a car club.' });
        if (me.club) return ctx.send({ type: 'error', error: 'Leave your current club first.' });
        const taken = Object.values(this.state.clubs).some(c => c.tag === m.tag || c.name.toLowerCase() === m.name.toLowerCase());
        if (taken) return ctx.send({ type: 'error', error: 'That club name or tag is taken.' });
        const club = { id: crypto.randomBytes(6).toString('hex'), name: m.name, tag: m.tag, owner: k, members: [k],
          created: this.now(), chat: [] };
        this.state.clubs[club.id] = club;
        me.club = club.id;
        this.save();
        ctx.send({ type: 'club', club: this.clubDetail(club) });
        break;
      }
      case 'club_join': {
        const club = this.state.clubs[m.clubId];
        if (!club) return ctx.send({ type: 'error', error: 'No such club.' });
        if (k.startsWith('g:')) return ctx.send({ type: 'error', error: 'Sign in to join a car club.' });
        if (me.club && me.club !== club.id) return ctx.send({ type: 'error', error: 'Leave your current club first.' });
        if (!club.members.includes(k)) club.members.push(k);
        me.club = club.id;
        this.save();
        this.clubBroadcast(club, ctx, { type: 'club_member', club: club.tag, name: me.name, joined: true });
        ctx.send({ type: 'club', club: this.clubDetail(club) });
        break;
      }
      case 'club_leave': {
        const club = me.club && this.state.clubs[me.club];
        me.club = null;
        if (club) {
          club.members = club.members.filter(x => x !== k);
          if (!club.members.length) delete this.state.clubs[club.id];
          else {
            if (club.owner === k) club.owner = club.members[0];
            this.clubBroadcast(club, ctx, { type: 'club_member', club: club.tag, name: me.name, joined: false });
          }
        }
        this.save();
        ctx.send({ type: 'club', club: null });
        break;
      }
      case 'club_list':
        ctx.send({ type: 'clubs', clubs: Object.values(this.state.clubs).map(c => this.clubSummary(c))
          .sort((a, b) => b.points - a.points).slice(0, 100) });
        break;
      case 'club_info': {
        const club = this.state.clubs[m.clubId];
        if (!club) return ctx.send({ type: 'error', error: 'No such club.' });
        ctx.send({ type: 'club', club: this.clubDetail(club, club.members.includes(k)) });
        break;
      }
      case 'club_chat': {
        const club = me.club && this.state.clubs[me.club];
        if (!club) return ctx.send({ type: 'error', error: 'Join a car club to use club chat.' });
        const line = { from: me.name, text: m.text, ts: this.now() };
        club.chat.push(line);
        if (club.chat.length > CHAT_KEEP) club.chat.splice(0, club.chat.length - CHAT_KEEP);
        this.save();
        this.clubBroadcast(club, ctx, { type: 'club_chat', club: club.tag, ...line });
        break;
      }
      case 'leaderboard':
        if (m.scope === 'clubs') {
          ctx.send({ type: 'leaderboard', scope: 'clubs', rows: Object.values(this.state.clubs)
            .map(c => this.clubSummary(c)).sort((a, b) => b.points - a.points).slice(0, 50) });
        } else {
          ctx.send({ type: 'leaderboard', scope: 'players', rows: Object.values(this.state.players)
            .filter(p => p.races > 0)
            .map(p => ({ name: p.name, points: p.points, races: p.races, club: this.tagOfPlayer(p) }))
            .sort((a, b) => b.points - a.points || b.races - a.races).slice(0, 50) });
        }
        break;
      case 'race_event':
        this.raceEvent(client, m.event, ctx);
        break;
    }
  }

  clubDetail(club, member = true) {
    return { ...this.clubSummary(club), owner: (this.state.players[club.owner] || {}).name,
      roster: club.members.map(k => ({ name: (this.state.players[k] || {}).name || '?', points: (this.state.players[k] || {}).points || 0 })),
      chat: member ? club.chat.slice(-20) : [] };
  }
  tagOfPlayer(p) { return p && p.club && this.state.clubs[p.club] ? this.state.clubs[p.club].tag : null; }
  tagOf(client) { return this.tagOfPlayer(this.state.players[this.key(client)]); }
  clubBroadcast(club, ctx, obj) {
    for (const c of ctx.clients()) if (club.members.includes(this.key(c))) ctx.sendTo(c, obj);
  }

  raceEvent(client, e, ctx) {
    if (client.type !== 'game' || !client.gameId) return;  // only a game's tunnel, inside a room
    const now = this.now();
    this.state.research.push({ t: now, room: client.gameId, by: this.key(client), ...e });
    if (this.state.research.length > RESEARCH_KEEP) this.state.research.splice(0, this.state.research.length - RESEARCH_KEEP);
    this.save();
    this.results.event(client.gameId, this.key(client), client, e, (key, c, place, of, earned) => {
      const p = c && this.player(c);
      if (!p) return;
      p.points += earned; p.wins = (p.wins || 0) + (place === 1 ? 1 : 0);
      ctx.sendTo(c, { type: 'race_result', place, of, earned, points: p.points, provisional: true });
      this.save();
    });
    if (e.type !== 'RACE_STATE' || e.state !== 5) return;
    let race = this.races.get(client.gameId);
    if (!race || now - race.at > (race.confirmed ? RACE_COOLDOWN_MS : RACE_WINDOW_MS)) {
      race = { at: now, reporters: new Map(), confirmed: false };
      this.races.set(client.gameId, race);
    }
    const k = this.key(client);
    if (race.reporters.has(k)) return;
    race.reporters.set(k, client);
    const award = c => {
      const p = this.player(c);
      p.points += RACE_POINTS;
      p.races += 1;
      ctx.sendTo(c, { type: 'points', points: p.points, races: p.races, earned: RACE_POINTS });
    };
    if (race.confirmed) award(client);  // a third racer reporting the same start
    else if (race.reporters.size >= 2) {
      race.confirmed = true;
      for (const [, c] of race.reporters) award(c);
    }
    this.save();
  }
}

module.exports = { Social, validateSocial, SOCIAL_TYPES: TYPES, RACE_POINTS };
