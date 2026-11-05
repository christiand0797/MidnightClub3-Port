'use strict';
// Finishing places, replayed from a real two-copy Track Race recording (fixtures_track_race.json: the race events
// both tunnels reported, in time order). The guest reached checkpoint 3 at 47.3 s; the host got to checkpoint 2.
const test = require('node:test');
const assert = require('node:assert');
const fs = require('fs');
const os = require('os');
const path = require('path');
const { RaceResults, PLACE_POINTS, checkpoint } = require('./race_results');
const { Social, validateSocial, RACE_POINTS } = require('./social');
const events = require('./fixtures_track_race.json');

test('checkpoint bodies decode to slot, index and race time', () => {
  assert.deepEqual(checkpoint('01000000be9fb64107320700'), { slot: 1, index: 0, time: checkpoint('01000000be9fb64107320700').time });
  assert.ok(Math.abs(checkpoint('01000000be9fb64107320700').time - 22.828) < 0.01);
  assert.equal(checkpoint('0100'), null);
});

test('the recorded race places the guest first and the host second', () => {
  const r = new RaceResults(() => 0), placed = [];
  for (const { by, event } of events) r.event('room', by, { id: by }, event, (key, c, place, of, pts) => placed.push([key, place, of, pts]));
  assert.deepEqual(placed, [['guest', 1, 2, PLACE_POINTS[0]], ['host', 2, 2, PLACE_POINTS[1]]]);
});

test('one tunnel alone cannot confirm checkpoints or claim a place', () => {
  const r = new RaceResults(() => 0), placed = [];
  for (const { by, event } of events) if (by === 'host') r.event('room', by, { id: by }, event, (...a) => placed.push(a));
  assert.deepEqual(placed, []);
});

test('places reach the leaderboard through Social', () => {
  let t = 1000;
  const social = new Social({ file: path.join(fs.mkdtempSync(path.join(os.tmpdir(), 'mc3-res-')), 's.json'), now: () => t });
  const clients = { host: { id: 'h', username: 'hosty', userId: 'user-h', type: 'game', gameId: 'main' },
    guest: { id: 'g', username: 'guesty', userId: 'user-g', type: 'game', gameId: 'main' } };
  const inbox = { h: [], g: [] };
  const ctx = c => ({ send: m => inbox[c.id].push(m), sendTo: (o, m) => inbox[o.id].push(m), clients: () => Object.values(clients), rooms: () => [] });
  for (const { t: at, by, event } of events) { t = 1000 + at * 1000; social.handle(clients[by], validateSocial({ type: 'race_event', event }), ctx(clients[by])); }
  const result = inbox.g.find(m => m.type === 'race_result');
  assert.deepEqual([result.place, result.of, result.earned], [1, 2, PLACE_POINTS[0]]);
  assert.equal(social.state.players['u:user-g'].points, RACE_POINTS + PLACE_POINTS[0]);
  assert.equal(social.state.players['u:user-h'].points, RACE_POINTS + PLACE_POINTS[1]);
  assert.equal(social.state.players['u:user-g'].wins, 1);
  social.flush && social.flush();
});
