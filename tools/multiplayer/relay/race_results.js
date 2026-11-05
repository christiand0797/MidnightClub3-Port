'use strict';
// Finishing places from the game's own race messages, as the tunnels report them (proxy/mcnet.py).
//
// Decoded from a recorded two-copy Track Race (2026-11-05):
//   CHECKPOINT (0x4C), 12 bytes: slot u8 | checkpoint index u8 | 2 bytes (not used) | race time f32 (seconds since
//     green) | game tick u32. A guest sends its own; the host sends its own and re-broadcasts the guests'.
//   PHASE (unnamed type 0x09, host -> guests), byte 0 = phase: 05 loading, 07 ready, 09 leaving the race,
//     03 back in the lobby. RACE_STATE 5 (racing) is sent by the host only, so its tunnel is slot 0.
//
// A checkpoint counts once two different racers' tunnels report it (the sender and a receiver), as race starts do.
// When the host leaves the race (phase 09 or 03) or starts the next one, racers are placed by furthest checkpoint,
// then by the time they reached it, and the top places earn bonus points on top of the start points. A racer who
// finished has passed the most checkpoints, so this matches the finish order without knowing a course's length.
// Like all rankings here it is provisional client-reported activity.

const PLACE_POINTS = [15, 10, 6, 3];       // 1st..4th, added to the confirmed-start points
const MAX_HEATS = 128, HEAT_TTL_MS = 30 * 60 * 1000, MAX_CHECKPOINTS = 512;

function checkpoint(body) {
  if (typeof body !== 'string' || body.length < 24) return null;
  const b = Buffer.from(body.slice(0, 24), 'hex');
  const time = b.readFloatLE(4);
  if (!Number.isFinite(time) || time < 0 || time > 24 * 3600) return null;
  return { slot: b[0], index: b[1], time };
}

class RaceResults {
  constructor(now = Date.now) { this.now = now; this.heats = new Map(); }

  // e: a validated race_event from a game tunnel `key` in room `room`. award(key, client, place, of, points) is called
  // for each placed racer when a heat settles.
  event(room, key, client, e, award) {
    const now = this.now();
    for (const [id, h] of this.heats) if (now - h.at > HEAT_TTL_MS) this.heats.delete(id);
    if (e.type === 'RACE_STATE' && e.state === 5) {
      const old = this.heats.get(room);
      if (old && now - old.at > 5000) this.settle(room, award);   // a new race: settle the last one
      if (!this.heats.has(room)) {
        if (this.heats.size >= MAX_HEATS) return;
        this.heats.set(room, { at: now, slots: new Map(), cps: new Map(), clients: new Map() });
      }
      const h = this.heats.get(room);
      h.clients.set(key, client);
      if (e.dir === 'out') h.slots.set(0, key);                  // only the host sends RACE_STATE
      return;
    }
    const h = this.heats.get(room);
    if (!h) return;
    h.clients.set(key, client);
    if (e.type === 'CHECKPOINT') {
      const cp = checkpoint(e.body);
      if (!cp) return;
      // A guest's own checkpoints are the only ones it sends; the host's own are slot 0.
      if (e.dir === 'out' && h.slots.get(0) !== key && !h.slots.has(cp.slot)) h.slots.set(cp.slot, key);
      const id = cp.slot + ':' + cp.index;
      let row = h.cps.get(id);
      if (!row) {
        if (h.cps.size >= MAX_CHECKPOINTS) return;
        row = { ...cp, reporters: new Set() };
        h.cps.set(id, row);
      }
      row.reporters.add(key);
    } else if (e.type === 'PHASE' && e.dir === 'out' && h.slots.get(0) === key) {
      const phase = parseInt(String(e.body).slice(0, 2), 16);
      if (phase === 0x09 || phase === 0x03) this.settle(room, award);
    }
  }

  // Places for a heat: [{ slot, key, index, time }] best first (confirmed checkpoints of mapped racers only).
  standings(h) {
    const best = new Map();
    for (const row of h.cps.values()) {
      if (row.reporters.size < 2 || !h.slots.has(row.slot)) continue;
      const cur = best.get(row.slot);
      if (!cur || row.index > cur.index || (row.index === cur.index && row.time < cur.time)) best.set(row.slot, row);
    }
    return [...best.values()].sort((a, b) => b.index - a.index || a.time - b.time)
      .map(r => ({ slot: r.slot, key: h.slots.get(r.slot), index: r.index, time: r.time }));
  }

  settle(room, award) {
    const h = this.heats.get(room);
    if (!h) return [];
    this.heats.delete(room);
    const places = this.standings(h);
    if (places.length < 2) return [];                            // a place needs someone to beat
    places.forEach((p, i) => award(p.key, h.clients.get(p.key), i + 1, places.length, PLACE_POINTS[i] || 0));
    return places;
  }
}

module.exports = { RaceResults, PLACE_POINTS, checkpoint };
