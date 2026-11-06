/* Host pairing is separate from account authentication. No account JWT goes to a stream host. */
(function (root, factory) {
  const api = factory();
  if (typeof module === 'object' && module.exports) module.exports = api;
  else root.MC3Stream = api;
})(typeof globalThis !== 'undefined' ? globalThis : this, function () {
  'use strict';
  function consumePairing(href, replace) {
    const url = new URL(href), fragment = new URLSearchParams(url.hash.slice(1));
    const token = fragment.get('pair');
    if (fragment.has('pair')) {
      fragment.delete('pair'); url.hash = fragment.toString();
      replace(url.href); // Remove the secret from the current history entry before opening sockets.
    }
    return typeof token === 'string' && /^[0-9a-f]{64}$/.test(token) ? token : null;
  }
  function handshake(token) {
    if (typeof token !== 'string' || !/^[0-9a-f]{64}$/.test(token)) throw Error('Host pairing required.');
    return JSON.stringify({ type: 'pair', token });
  }

  const MAX_FRAME_BYTES = 16 * 1024 * 1024;
  const MAX_TEXT = 4 * Math.ceil(MAX_FRAME_BYTES / 3) + 4096;
  const MAX_CONTROL_TEXT = 16 * 1024;
  function validDimensions(width, height) {
    return Number.isSafeInteger(width) && Number.isSafeInteger(height) && width > 0 && height > 0 &&
      width <= 8192 && height <= 8192 && width * height <= 16 * 1024 * 1024;
  }
  function pngDimensions(base64) {
    if (typeof base64 !== 'string' || base64.length < 44 || base64.length > MAX_TEXT - 4096 ||
        base64.length % 4 !== 0 || !/^[A-Za-z0-9+/]*={0,2}$/.test(base64)) return null;
    const alphabet = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/', header = [];
    for (let i = 0; i < 44; i += 4) {
      const a = alphabet.indexOf(base64[i]), b = alphabet.indexOf(base64[i + 1]);
      const c = alphabet.indexOf(base64[i + 2]), d = alphabet.indexOf(base64[i + 3]);
      if (Math.min(a, b, c, d) < 0) return null;
      header.push((a << 2) | (b >> 4), ((b & 15) << 4) | (c >> 2), ((c & 3) << 6) | d);
    }
    if (!header.slice(0, 16).every((v, i) => v === [137,80,78,71,13,10,26,10,0,0,0,13,73,72,68,82][i])) return null;
    const u32 = i => ((header[i] * 0x1000000) + (header[i + 1] << 16) + (header[i + 2] << 8) + header[i + 3]);
    return validDimensions(u32(16), u32(20)) ? { width: u32(16), height: u32(20) } : null;
  }
  function hasIDR(bytes) {
    for (let i = 0; i + 3 < bytes.length; ++i) {
      if (bytes[i] !== 0 || bytes[i + 1] !== 0) continue;
      const at = bytes[i + 2] === 1 ? i + 3 : bytes[i + 2] === 0 && bytes[i + 3] === 1 ? i + 4 : -1;
      if (at >= 0 && at < bytes.length && (bytes[at] & 31) === 5) return true;
    }
    return false; // An SPS alone is not an independently decodable picture.
  }

  // A connection owns every callback, timer and control grant. Retiring it first prevents an old
  // socket's delayed close/message from changing a new host connection or reopening after logout.
  class Session {
    constructor(options) {
      this.options = options;
      this.socketFactory = options.socketFactory || (url => new WebSocket(url));
      this.setTimer = options.setTimer || ((fn, ms) => setTimeout(fn, ms));
      this.clearTimer = options.clearTimer || (id => clearTimeout(id));
      this.connection = null; this.retryTimer = null; this.stopped = true; this.sequence = 0;
    }
    get socket() { return this.connection && this.connection.socket; }
    get paired() { return !!(this.connection && this.connection.paired); }
    get canControl() { return !!(this.connection && this.connection.paired && this.connection.control); }
    isCurrent(id) { return !!this.connection && this.connection.id === id; }
    connect() {
      const auth = handshake(this.options.token);
      if (this.connection) return this.connection.id;
      this.stopped = false;
      if (this.retryTimer !== null) this.clearTimer(this.retryTimer);
      this.retryTimer = null;
      const connection = { id: ++this.sequence, socket: null, paired: false, control: false, codec: null, timer: null };
      this.connection = connection;
      try { connection.socket = this.socketFactory(this.options.url); }
      catch (_) { this.finish(connection, 1006, 'Could not connect to the game host.'); return connection.id; }
      const ws = connection.socket;
      ws.binaryType = 'arraybuffer';
      connection.timer = this.setTimer(() => this.fail(1008, 'Host pairing timed out.'), 5000);
      ws.onopen = () => {
        if (this.connection !== connection) return;
        try { ws.send(auth); } catch (_) { this.fail(1011, 'Could not pair with the game host.'); }
      };
      ws.onmessage = event => {
        if (this.connection !== connection) return;
        try { this.receive(connection, event.data); }
        catch (_) { this.fail(1008, 'The game host sent an invalid stream message.'); }
      };
      ws.onclose = event => this.finish(connection, event.code, 'Game host disconnected.');
      ws.onerror = () => {}; // close or the bounded pairing timer owns connection state.
      if (this.options.onConnecting) this.options.onConnecting(connection.id);
      return connection.id;
    }
    receive(connection, data) {
      if (typeof data === 'string') {
        if (data.length > MAX_TEXT) return this.fail(1009, 'The game host frame is too large.');
        let msg;
        try { msg = JSON.parse(data); } catch (_) { return this.fail(1008, 'Invalid game host message.'); }
        if (!msg || typeof msg !== 'object' || Array.isArray(msg) || typeof msg.type !== 'string') return this.fail(1008, 'Invalid game host message.');
        if (!connection.paired) {
          if (data.length > MAX_CONTROL_TEXT || msg.type !== 'hello' || msg.paired !== true || typeof msg.control !== 'boolean') return this.fail(1008, 'Host pairing was refused.');
          if (msg.codec !== 'png' && msg.codec !== 'h264') return this.fail(1003, 'The game host uses an unsupported video format.');
          if ((msg.width || msg.height) && !validDimensions(msg.width, msg.height)) return this.fail(1009, 'The game host video dimensions are unsupported.');
          this.clearTimer(connection.timer); connection.timer = null;
          connection.paired = true; connection.control = msg.control; connection.codec = msg.codec;
          if (this.options.onReady) this.options.onReady(msg, connection.id);
          return;
        }
        if (msg.type === 'hello') return this.fail(1008, 'The game host repeated its pairing response.');
        if (msg.type === 'frame') {
          if (connection.codec !== 'png' || !pngDimensions(msg.data)) return this.fail(1009, 'The game host sent an invalid PNG frame.');
          if (this.options.onFrame) this.options.onFrame(msg, connection.id);
        } else {
          if (data.length > MAX_CONTROL_TEXT) return this.fail(1009, 'The game host message is too large.');
          if (this.options.onMessage) this.options.onMessage(msg, connection.id);
        }
      } else {
        const bytes = data instanceof ArrayBuffer ? new Uint8Array(data) : data instanceof Uint8Array ? data : null;
        if (!connection.paired || connection.codec !== 'h264') return this.fail(1008, 'Unexpected video before host pairing.');
        if (!bytes || !bytes.length || bytes.byteLength > MAX_FRAME_BYTES) return this.fail(1009, 'The game host video frame is too large.');
        if (this.options.onBinary) this.options.onBinary(bytes, connection.id);
      }
    }
    sendInput(report) {
      const ws = this.socket;
      if (!this.canControl || !ws || ws.readyState !== 1 || ws.bufferedAmount > 64 * 1024 ||
          !(report instanceof Uint8Array) || report.byteLength !== 24 ||
          report[0] !== 77 || report[1] !== 67 || report[2] !== 51 || report[3] !== 73) return false;
      try { ws.send(report); return true; } catch (_) { return false; }
    }
    finish(connection, code, reason) {
      if (this.connection !== connection) return;
      this.connection = null;
      if (connection.timer !== null) this.clearTimer(connection.timer);
      const terminal = code === 1008 || code === 1003 || code === 1009;
      if (!this.stopped && !terminal) {
        this.retryTimer = this.setTimer(() => {
          this.retryTimer = null;
          if (!this.stopped && (!this.options.shouldReconnect || this.options.shouldReconnect())) this.connect();
        }, 2000);
      }
      if (this.options.onDisconnect) this.options.onDisconnect({ code, reason, id: connection.id });
    }
    fail(code, reason) {
      const connection = this.connection;
      if (!connection) return;
      this.finish(connection, code, reason);
      // The browser API permits 1000 and private 3000..4999 codes, not server protocol codes.
      try { connection.socket.close(code >= 3000 || code === 1000 ? code : 4000 + code % 1000, reason); } catch (_) {}
    }
    disconnect() {
      this.stopped = true;
      if (this.retryTimer !== null) this.clearTimer(this.retryTimer);
      this.retryTimer = null;
      const connection = this.connection;
      if (!connection) return;
      this.finish(connection, 1000, 'Disconnected from game host.');
      try { connection.socket.close(1000, 'Player disconnected'); } catch (_) {}
    }
  }

  class AnnexBDecoder {
    constructor(options) {
      this.options = options;
      this.Decoder = options.Decoder || globalThis.VideoDecoder;
      this.Chunk = options.Chunk || globalThis.EncodedVideoChunk;
      this.setTimer = options.setTimer || ((fn, ms) => setTimeout(fn, ms)); this.clearTimer = options.clearTimer || (id => clearTimeout(id));
      this.now = options.now || (() => performance.now());
      this.decoder = null; this.generation = 0; this.timer = null; this.keySeen = false; this.timestamp = 0;
      this.config = { codec: 'avc1.640028' };
    }
    start() {
      this.stop();
      const generation = this.generation;
      const fail = () => {
        if (generation !== this.generation) return;
        this.stop();
        if (this.options.onError) this.options.onError('H.264 playback is unavailable. Ask the host to use PNG streaming.');
      };
      if (!this.Decoder || !this.Chunk || typeof this.Decoder.isConfigSupported !== 'function') { fail(); return; }
      this.timer = this.setTimer(fail, 5000);
      Promise.resolve().then(() => this.Decoder.isConfigSupported(this.config)).then(supported => {
        if (generation !== this.generation) return;
        if (!supported || !supported.supported) { fail(); return; }
        this.clearTimer(this.timer); this.timer = null;
        const decoder = new this.Decoder({
          output: frame => {
            try {
              if (generation !== this.generation) return;
              if (!validDimensions(frame.codedWidth, frame.codedHeight)) { fail(); return; }
              this.options.onFrame(frame);
            } catch (_) { fail(); }
            finally { frame.close(); }
          },
          error: fail
        });
        this.decoder = decoder; decoder.configure(this.config);
      }).catch(fail);
    }
    push(bytes) {
      const decoder = this.decoder;
      if (!decoder || decoder.state !== 'configured') return;
      if (!(bytes instanceof Uint8Array) || !bytes.length || bytes.byteLength > MAX_FRAME_BYTES) return;
      const key = hasIDR(bytes);
      try {
        if (decoder.decodeQueueSize >= 4) {
          decoder.reset(); decoder.configure(this.config); this.keySeen = false;
        }
        if (!this.keySeen && !key) return;
        this.keySeen = true;
        this.timestamp = Math.max(this.timestamp + 1, Math.floor(this.now() * 1000));
        decoder.decode(new this.Chunk({ type: key ? 'key' : 'delta', timestamp: this.timestamp, data: bytes }));
      } catch (_) {
        this.stop();
        if (this.options.onError) this.options.onError('H.264 playback failed. Ask the host to use PNG streaming.');
      }
    }
    stop() {
      ++this.generation;
      if (this.timer !== null) this.clearTimer(this.timer);
      this.timer = null;
      const decoder = this.decoder; this.decoder = null; this.keySeen = false; this.timestamp = 0;
      try { if (decoder && decoder.state !== 'closed') decoder.close(); } catch (_) {}
    }
  }
  return { consumePairing, handshake, Session, AnnexBDecoder, hasIDR, pngDimensions, validDimensions, MAX_FRAME_BYTES };
});
