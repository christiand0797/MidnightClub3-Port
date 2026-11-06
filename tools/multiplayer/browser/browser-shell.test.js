'use strict';
// Execute the shipped inline shell against injected DOM/socket objects, without a browser or GPU.
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const { resolveEndpoints } = require('./account-ui');

function shell() {
  class Element {
    constructor() { this.children = []; this.style = {}; this.value = ''; this.hidden = false; this.disabled = false;
      this.classList = { add() {}, remove() {} }; }
    appendChild(child) { child.parent = this; this.children.push(child); return child; }
    append(...children) { children.forEach(child => this.appendChild(child)); }
    replaceChildren(...children) { this.children.forEach(child => { child.parent = null; }); this.children = []; this.append(...children); }
    remove() { if (this.parent) { this.parent.children.splice(this.parent.children.indexOf(this), 1); this.parent = null; } }
    get childElementCount() { return this.children.length; }
    get firstElementChild() { return this.children[0]; }
    set textContent(value) { this.content = value; this.replaceChildren(); }
    get textContent() { return this.content || ''; }
    removeAttribute(name) { delete this[name]; }
  }
  const elements = new Map(), sockets = [], timers = new Map(); let timerId = 0, hooks;
  const document = { getElementById(id) { if (!elements.has(id)) elements.set(id, new Element()); return elements.get(id); },
    createElement() { return new Element(); }, querySelectorAll() { return []; } };
  class Socket {
    constructor(url) { this.url = url; this.sent = []; this.readyState = 0; this.bufferedAmount = 0; sockets.push(this); }
    open() { this.readyState = 1; this.onopen(); }
    send(data) { this.sent.push(typeof data === 'string' ? JSON.parse(data) : data); }
    message(data) { this.onmessage({ data: JSON.stringify(data) }); }
    close(code) { this.closedCode = code; this.readyState = 3; }
  }
  const account = { client: { session: null, preferences: { deadzone: 0.15 } }, start() {}, pollMenus() { return false; } };
  const context = vm.createContext({ URL, URLSearchParams, Uint8Array, ArrayBuffer, DataView,
    document, navigator: { getGamepads() { return []; } }, WebSocket: Socket, history: { replaceState() {} },
    setTimeout(fn) { const id = ++timerId; timers.set(id, fn); return id; }, clearTimeout(id) { timers.delete(id); },
    requestAnimationFrame() {}, prompt() { return 'Synthetic room'; },
    window: { location: { href: 'http://127.0.0.1:8080/' }, addEventListener() {} },
    MC3Account: { resolveEndpoints, mount(options) { hooks = options; return account; } } });
  for (const file of ['stream-client.js', 'relay-client.js']) vm.runInContext(fs.readFileSync(path.join(__dirname, file), 'utf8'), context, { filename: file });
  const html = fs.readFileSync(path.join(__dirname, 'index.html'), 'utf8');
  const inline = [...html.matchAll(/<script>([\s\S]*?)<\/script>/g)].at(-1)[1];
  vm.runInContext(inline, context, { filename: 'index.html:inline' });
  const session = { token: 'synthetic.jwt.token', user: { id: '11111111-1111-4111-8111-111111111111', username: 'Synthetic Player' } };
  function login() { account.client.session = session; hooks.onSession(session); const ws = sockets.at(-1); ws.open(); ws.message({ type: 'auth_ok', user: session.user }); return ws; }
  function logout() { account.client.session = null; hooks.onSession(null); }
  return { elements, sockets, timers, context, session, login, logout };
}

test('the actual shell connects to /ws and renders both open and hosted uncapped rooms as joinable', () => {
  const f = shell(), ws = f.login(); assert.equal(ws.url, 'ws://127.0.0.1:8080/ws');
  ws.message({ type: 'gameList', games: [
    { id: 'main', name: 'Main Lobby', status: 'waiting', player_count: 18, max_players: 0, open: true },
    { id: 'hosted', name: 'Hosted Room', status: 'waiting', player_count: 10, max_players: 0, open: false }
  ] });
  const list = f.elements.get('gameList'); assert.equal(list.children.length, 2);
  for (const button of list.children) assert.equal(button.disabled, false);
  list.children[0].onclick(); assert.deepEqual(ws.sent.at(-1), { type: 'join_game', gameId: 'main' });
  ws.message({ type: 'game_joined', game: { id: 'main', name: 'Main Lobby' }, role: 'player' });
  assert.equal(vm.runInContext('currentGameId', f.context), 'main');
  assert.equal(vm.runInContext('myId', f.context), f.session.user.id);
  assert.equal(f.sockets.length, 1); // Room membership cannot substitute for an independent host pairing secret.
  assert.match(f.elements.get('notif').textContent, /pairing link/);
  f.logout();
});

test('shell logout clears prior-account chat/rooms and rejects delayed old relay callbacks', () => {
  const f = shell(), old = f.login();
  old.message({ type: 'chat', from: 'Friend', text: 'Prior-account conversation' });
  assert(f.elements.get('chatMessages').childElementCount > 0);
  f.logout();
  assert.equal(f.elements.get('chatMessages').childElementCount, 0);
  assert.equal(f.elements.get('gameList').childElementCount, 0);
  old.message({ type: 'game_joined', game: { id: 'old-room' }, role: 'player' });
  old.message({ type: 'chat', from: 'Friend', text: 'Late message' });
  assert.equal(f.elements.get('chatMessages').childElementCount, 0);
  assert.equal(vm.runInContext('currentGameId', f.context), null);
  assert.equal(vm.runInContext('myId', f.context), null);
  assert.equal(f.sockets.length, 1);
});

test('shell chat retains a bounded recent history without interpreting player text as HTML', () => {
  const f = shell(), ws = f.login();
  for (let i = 0; i < 140; i++) ws.message({ type: 'chat', from: 'Friend', text: '<img onerror=bad()> message ' + i });
  const chat = f.elements.get('chatMessages'); assert.equal(chat.childElementCount, 100);
  assert.match(chat.firstElementChild.textContent, /message 40$/);
  assert.match(chat.children.at(-1).textContent, /<img onerror=bad\(\)> message 139$/);
  f.logout();
});
