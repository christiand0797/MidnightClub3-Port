'use strict';
// social.js: presence, car clubs, club chat, leaderboards and confirmed race points.
const test = require('node:test');
const assert = require('node:assert');
const fs = require('fs');
const os = require('os');
const path = require('path');
const { Social, validateSocial, RACE_POINTS } = require('./social');

function world(now = { t: 1000 }) {
  const file = path.join(fs.mkdtempSync(path.join(os.tmpdir(), 'mc3-social-')), 'social.json');
  const social = new Social({ file, now: () => now.t });
  const inbox = new Map();
  const all = [];
  const client = (name, { account = true, type = 'browser', gameId = null } = {}) => {
    const c = { id: name, username: name, userId: account ? 'user-' + name : 'guest-' + name, type, gameId };
    inbox.set(c, []); all.push(c); return c;
  };
  const rooms = () => [{ id: 'main', name: 'Main Lobby', listed: true, players: all.filter(c => c.gameId === 'main') },
    { id: 'r-secret', name: 'Friday', listed: false, players: all.filter(c => c.gameId === 'r-secret') }];
  const send = (c, m) => social.handle(c, validateSocial(m), {
    send: obj => inbox.get(c).push(obj), sendTo: (o, obj) => inbox.get(o).push(obj), clients: () => all, rooms });
  const last = (c, type) => inbox.get(c).filter(m => m.type === type).pop();
  return { social, file, client, send, last, now };
}

test('messages outside the social shapes are refused', () => {
  assert.equal(validateSocial({ type: 'club_create', name: 'ab', tag: 'X' }), null);
  assert.equal(validateSocial({ type: 'club_create', name: 'Night Runners', tag: 'n1ght!' }), null);
  assert.deepEqual(validateSocial({ type: 'club_create', name: ' Night Runners ', tag: 'nr' }), { type: 'club_create', name: 'Night Runners', tag: 'NR' });
  assert.equal(validateSocial({ type: 'club_chat', text: 'x'.repeat(301) }), null);
  assert.equal(validateSocial({ type: 'race_event', event: { type: 'RACE_STATE', body: 'zz' } }), null);
  assert.equal(validateSocial({ type: 'leaderboard', scope: 'everyone' }), null);
  assert.equal(validateSocial({ type: 'mystery' }), null);
});

test('car clubs: create, join, chat to members only, leave', () => {
  const w = world();
  const ana = w.client('ana'), ben = w.client('ben'), cat = w.client('cat'), guest = w.client('drifter', { account: false });
  w.send(guest, { type: 'club_create', name: 'Guests', tag: 'GST' });
  assert.match(w.last(guest, 'error').error, /Sign in/);
  w.send(ana, { type: 'club_create', name: 'Night Runners', tag: 'NR' });
  const club = w.last(ana, 'club').club;
  assert.equal(club.tag, 'NR');
  w.send(ben, { type: 'club_create', name: 'night runners', tag: 'XX' });
  assert.match(w.last(ben, 'error').error, /taken/);
  w.send(ben, { type: 'club_join', clubId: club.id });
  assert.equal(w.last(ben, 'club').club.members, 2);
  w.send(ben, { type: 'club_chat', text: 'meet at the docks' });
  assert.equal(w.last(ana, 'club_chat').text, 'meet at the docks');
  assert.equal(w.last(cat, 'club_chat'), undefined);  // not a member
  w.send(cat, { type: 'club_info', clubId: club.id });
  assert.deepEqual(w.last(cat, 'club').club.chat, []);  // outsiders see the roster, not the chat
  w.send(ana, { type: 'club_leave' });
  w.send(ben, { type: 'club_info', clubId: club.id });
  assert.equal(w.last(ben, 'club').club.owner, 'ben');  // ownership passes on
  w.send(ben, { type: 'club_leave' });
  w.send(ben, { type: 'club_list' });
  assert.deepEqual(w.last(ben, 'clubs').clubs, []);   // an empty club is gone
});

test('a race counts only when two racers report the same start, and points reach the leaderboard', () => {
  const w = world();
  const host = w.client('host', { type: 'game', gameId: 'main' });
  const guest = w.client('guest', { type: 'game', gameId: 'main' });
  const late = w.client('late', { type: 'game', gameId: 'main' });
  const browser = w.client('viewer', { gameId: 'main' });
  const start = { type: 'race_event', event: { type: 'RACE_STATE', body: '05000000', state: 5 } };
  w.send(browser, start);                       // not a game tunnel: ignored
  w.send(host, start);
  w.send(host, start);                          // the same racer twice is still one report
  assert.equal(w.last(host, 'points'), undefined);
  w.send(guest, { ...start, event: { ...start.event, dir: 'in' } });
  assert.equal(w.last(host, 'points').points, RACE_POINTS);
  assert.equal(w.last(guest, 'points').earned, RACE_POINTS);
  w.send(late, start);                          // a third racer of the same start
  assert.equal(w.last(late, 'points').points, RACE_POINTS);
  w.now.t += 5000;
  w.send(host, start);                          // a repeat inside the cooldown does not count again
  assert.equal(w.last(host, 'points').points, RACE_POINTS);
  w.now.t += 120000;
  w.send(host, start); w.send(guest, start);    // the next race
  assert.equal(w.last(host, 'points').points, 2 * RACE_POINTS);
  w.send(browser, { type: 'leaderboard' });
  const rows = w.last(browser, 'leaderboard').rows;
  assert.deepEqual(rows.slice(0, 2).map(r => [r.name, r.races]).sort(), [['guest', 2], ['host', 2]]);
  assert.ok(!rows.some(r => r.name === 'viewer'));   // no races, not ranked
  assert.ok(w.social.state.research.length >= 6);    // the raw messages are kept for decoding results
});

test('club points add up members and presence hides named rooms', () => {
  const w = world();
  const a = w.client('a', { type: 'game', gameId: 'main' }), b = w.client('b', { type: 'game', gameId: 'main' });
  const s = w.client('s', { gameId: 'r-secret' });
  w.send(a, { type: 'club_create', name: 'Apex Crew', tag: 'APX' });
  const start = { type: 'race_event', event: { type: 'RACE_STATE', body: '05000000', state: 5 } };
  w.send(a, start); w.send(b, start);
  w.send(b, { type: 'leaderboard', scope: 'clubs' });
  assert.equal(w.last(b, 'leaderboard').rows[0].points, RACE_POINTS);
  w.send(b, { type: 'presence' });
  const p = w.last(b, 'presence');
  assert.deepEqual(p.rooms.map(r => r.id), ['main']);
  assert.equal(p.rooms[0].players.find(x => x.name === 'a').club, 'APX');
  w.send(s, { type: 'presence' });
  assert.deepEqual(w.last(s, 'presence').rooms.map(r => r.id), ['main', 'r-secret']);  // your own room shows
});

test('standing survives a restart', () => {
  const w = world();
  const a = w.client('a', { type: 'game', gameId: 'main' }), b = w.client('b', { type: 'game', gameId: 'main' });
  const start = { type: 'race_event', event: { type: 'RACE_STATE', body: '05000000', state: 5 } };
  w.send(a, start); w.send(b, start);
  w.social.flush();
  const again = new Social({ file: w.file });
  assert.equal(again.state.players['u:user-a'].points, RACE_POINTS);
});
