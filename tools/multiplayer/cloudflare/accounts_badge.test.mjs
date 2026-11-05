// The creator badge (CREATOR_NAMES): only the account that first holds a creator name gets it, in every response that
// describes the player. Runs Accounts against node:sqlite in place of the Durable Object's SQLite.
//   node --test accounts_badge.test.mjs
import test from 'node:test';
import assert from 'node:assert/strict';
import { DatabaseSync } from 'node:sqlite';
import { Accounts } from './src/accounts.js';

function storage() {
  const db = new DatabaseSync(':memory:');
  return {
    exec(query, ...args) {
      const st = db.prepare(query);
      if (/^\s*(SELECT|WITH)/i.test(query)) { const rows = st.all(...args); return { toArray: () => rows }; }
      if (/^\s*(CREATE|INSERT|UPDATE|DELETE)/i.test(query)) st.run(...args); else db.exec(query);
      return { toArray: () => [] };
    }
  };
}

const URL_ = new URL('https://mc3.top/api/register');
function req(body, extra = {}) {
  const headers = new Headers({ Origin: 'https://mc3.top', 'Content-Type': 'application/json', ...extra });
  return new Request(URL_, { method: 'POST', headers, body: JSON.stringify(body) });
}
async function call(acc, route, body, extra) {
  const headers = {};
  return (await acc.route(route, req(body, extra), URL_, headers)).body;
}

test('the creator account gets the badge in register, login, token and profile', async () => {
  const acc = new Accounts(storage(), { creators: ['Creator_One'] });
  const reg = await call(acc, 'register', { username: 'creator_one', password: 'long-enough-1' });
  assert.equal(reg.user.badge, 'creator');
  const other = await call(acc, 'register', { username: 'someone', password: 'long-enough-2' });
  assert.equal(other.user.badge, null);
  const login = await call(acc, 'login', { username: 'CREATOR_ONE', password: 'long-enough-1' });
  assert.equal(login.user.badge, 'creator');
  const v = await acc.verifyToken(login.token);
  assert.equal(v.badge, 'creator');
  const prof = await acc.route('profile', new Request('https://mc3.top/api/profile', { headers: { Authorization: 'Bearer ' + login.token } }), new URL('https://mc3.top/api/profile'), {});
  assert.equal(prof.body.badge, 'creator');
});

test('a new account re-using a deleted creator name gets no badge', async () => {
  const acc = new Accounts(storage(), { creators: ['boss'] });
  const first = await call(acc, 'register', { username: 'boss', password: 'long-enough-1' });
  assert.equal(first.user.badge, 'creator');
  acc.sql.exec(`DELETE FROM users WHERE id = ?`, first.user.id);
  const second = await call(acc, 'register', { username: 'Boss', password: 'long-enough-3' });
  assert.equal(second.user.badge, null);
});

test('no creators configured: nobody has a badge; registered names are known case-insensitively', async () => {
  const acc = new Accounts(storage());
  const reg = await call(acc, 'register', { username: 'Player One', password: 'long-enough-1' });
  assert.equal(reg.user.badge, null);
  assert.equal(acc.isRegisteredName('player one'), true);
  assert.equal(acc.isRegisteredName('nobody'), false);
});
