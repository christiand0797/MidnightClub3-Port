/* The account relay and the native host stream have independent connections and credentials. */
(function (root, factory) {
  const api = factory();
  if (typeof module === 'object' && module.exports) module.exports = api;
  else root.MC3Relay = api;
})(typeof globalThis !== 'undefined' ? globalThis : this, function () {
  'use strict';
  const text = (v, max) => typeof v === 'string' && v.length > 0 && v.length <= max && !/[\u0000-\u001f\u007f]/.test(v);
  const id = v => text(v, 64) && /^[A-Za-z0-9_-]+$/.test(v);
  const count = v => Number.isSafeInteger(v) && v >= 0 && v <= 100000;
  const plain = v => !!v && typeof v === 'object' && !Array.isArray(v);

  function normalizeMessage(message) {
    if (!plain(message) || !text(message.type, 50)) throw Error('Invalid relay message.');
    switch (message.type) {
      case 'auth_ok':
        if (!plain(message.user) || !text(message.user.id, 128) || !text(message.user.username, 100)) throw Error('Invalid relay account.');
        return { type: 'auth_ok', user: { id: message.user.id, username: message.user.username } };
      case 'gameList':
        if (!Array.isArray(message.games) || message.games.length > 128) throw Error('Invalid relay room list.');
        return { type: 'gameList', games: message.games.map(game => {
          if (!plain(game) || !id(game.id) || !text(game.name || game.id, 100) ||
              !['waiting', 'playing'].includes(game.status) || !count(game.player_count) || !count(game.max_players)) throw Error('Invalid relay room.');
          return { id: game.id, name: game.name || game.id, status: game.status, player_count: game.player_count,
            max_players: game.max_players, open: game.open === true };
        }) };
      case 'game_joined': case 'game_created':
        if (!plain(message.game) || !id(message.game.id) || !['host', 'player', 'spectator'].includes(message.role)) throw Error('Invalid relay room membership.');
        return { type: 'room_joined', gameId: message.game.id, role: message.role, created: message.type === 'game_created',
          name: text(message.game.name, 100) ? message.game.name : message.game.id,
          color: typeof message.color === 'string' && /^#[0-9a-f]{6}$/i.test(message.color) ? message.color : null };
      case 'player_joined': case 'player_left':
        if (!text(message.username, 100)) throw Error('Invalid relay player.');
        return { type: message.type, username: message.username };
      case 'chat':
        if (!text(message.from, 100) || !text(message.text, 512)) throw Error('Invalid relay chat.');
        return { type: 'chat', from: message.from, text: message.text };
      case 'error':
        if (!text(message.error, 2048)) throw Error('Invalid relay error.');
        return { type: 'error', error: message.error };
      case 'game_started': return { type: 'game_started' };
      case 'host_transferred':
        return text(message.newHost, 100) ? { type: 'host_transferred', username: message.newHost } : null;
      default: return null; // Native input payloads and unimplemented events are not browser game state.
    }
  }

  class Client {
    constructor(options) {
      this.options = options;
      this.socketFactory = options.socketFactory || (url => new WebSocket(url));
      this.setTimer = options.setTimer || ((fn, ms) => setTimeout(fn, ms)); this.clearTimer = options.clearTimer || (id => clearTimeout(id));
      this.session = null; this.connection = null; this.retryTimer = null;
    }
    get ready() { return !!(this.connection && this.connection.authenticated); }
    setSession(session) {
      if (!session) { this.session = null; this.disconnect(); return; }
      if (!plain(session.user) || !text(session.user.id, 128) || !text(session.user.username, 100) ||
          !text(session.token, 8192) || !/^[A-Za-z0-9_.-]+$/.test(session.token)) throw Error('Invalid relay session.');
      if (this.session && this.session.token === session.token && this.session.user.id === session.user.id) { this.connect(); return; }
      this.disconnect();
      this.session = { token: session.token, user: { id: session.user.id, username: session.user.username } };
      this.connect();
    }
    connect() {
      if (!this.session || this.connection) return;
      if (this.retryTimer !== null) this.clearTimer(this.retryTimer);
      this.retryTimer = null;
      const connection = { socket: null, session: this.session, authenticated: false, timer: null };
      this.connection = connection;
      try { connection.socket = this.socketFactory(this.options.url); }
      catch (_) { this.finish(connection, 1006, 'Relay connection failed.'); return; }
      const ws = connection.socket; ws.binaryType = 'arraybuffer';
      connection.timer = this.setTimer(() => this.fail(connection, 4008, 'Relay authentication timed out.'), 5000);
      ws.onopen = () => {
        if (!this.current(connection)) return;
        try { ws.send(JSON.stringify({ type: 'auth', token: connection.session.token, clientType: 'browser' })); }
        catch (_) { this.fail(connection, 4011, 'Relay authentication could not be sent.'); }
      };
      ws.onmessage = event => {
        if (!this.current(connection)) return;
        try {
          if (typeof event.data !== 'string' || event.data.length > 128 * 1024) throw Error('Invalid relay message size.');
          const message = normalizeMessage(JSON.parse(event.data));
          if (!message) return;
          if (!connection.authenticated) {
            if (message.type === 'error') { if (this.options.onMessage) this.options.onMessage(message); this.fail(connection, 4008, 'Relay authentication was refused.'); return; }
            if (message.type !== 'auth_ok' || message.user.id !== connection.session.user.id || message.user.username !== connection.session.user.username) throw Error('Relay account did not match the signed-in account.');
            connection.authenticated = true; this.clearTimer(connection.timer); connection.timer = null;
            if (this.options.onReady) this.options.onReady(message.user);
          } else {
            if (message.type === 'auth_ok') throw Error('Repeated relay authentication response.');
            if (this.options.onMessage) this.options.onMessage(message);
          }
        } catch (_) { this.fail(connection, 4008, 'The relay sent an invalid or mismatched message.'); }
      };
      ws.onclose = event => this.finish(connection, event.code, typeof event.reason === 'string' ? event.reason.slice(0, 120) : 'Relay disconnected.');
      ws.onerror = () => {};
      if (this.options.onConnecting) this.options.onConnecting();
    }
    current(connection) { return this.connection === connection && this.session === connection.session; }
    send(message) {
      const connection = this.connection;
      if (!this.ready || !this.current(connection) || connection.socket.readyState !== 1 || connection.socket.bufferedAmount > 64 * 1024) return false;
      // Credentials are sent once during authentication, never attached to room, input or chat messages.
      if (!plain(message) || !['list', 'create_game', 'join_game', 'enter_room', 'leave_game', 'chat', 'set_ready', 'start_game', 'ping'].includes(message.type) || 'token' in message) return false;
      let data;
      try { data = JSON.stringify(message); if (data.length > 8192) return false; connection.socket.send(data); return true; } catch (_) { return false; }
    }
    finish(connection, code, reason) {
      if (this.connection !== connection) return;
      this.connection = null;
      if (connection.timer !== null) this.clearTimer(connection.timer);
      if (this.session && this.session === connection.session && ![1008, 1009, 4008, 4009].includes(code)) {
        this.retryTimer = this.setTimer(() => { this.retryTimer = null; if (this.session === connection.session) this.connect(); }, 3000);
      }
      if (this.options.onDisconnect) this.options.onDisconnect({ code, reason, userId: connection.session.user.id });
    }
    fail(connection, code, reason) {
      if (!this.current(connection)) return;
      this.finish(connection, code, reason);
      try { connection.socket.close(code, reason); } catch (_) {}
    }
    disconnect() {
      this.session = null;
      if (this.retryTimer !== null) this.clearTimer(this.retryTimer);
      this.retryTimer = null;
      const connection = this.connection;
      this.connection = null; // Invalidate callbacks before asking the old socket to close.
      if (!connection) return;
      if (connection.timer !== null) this.clearTimer(connection.timer);
      try { connection.socket.close(1000, 'Player session changed'); } catch (_) {}
    }
  }
  return { Client, normalizeMessage };
});
