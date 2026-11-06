const test = require('node:test');
const assert = require('node:assert/strict');
const { consumePairing, handshake, Session, AnnexBDecoder, hasIDR, pngDimensions, validDimensions, MAX_FRAME_BYTES } = require('./stream-client');
test('host token is removed from the URL and never stored or replaced by account credentials', () => {
  let replaced;
  const token = 'a'.repeat(64);
  assert.equal(consumePairing(`http://localhost:8080/?stream=ws://localhost:8766#pair=${token}&view=1`, u => replaced = u), token);
  assert.equal(replaced, 'http://localhost:8080/?stream=ws://localhost:8766#view=1');
  assert.deepEqual(JSON.parse(handshake(token)), { type: 'pair', token });
  for (const bad of ['account.jwt', '', null, 'a'.repeat(65)]) assert.throws(() => handshake(bad));
  assert.equal(consumePairing('http://localhost:8080/#pair=invalid', u => replaced = u), null);
  assert.equal(replaced, 'http://localhost:8080/');
});

function timers() {
  let id = 0;
  const pending = new Map();
  return { pending, setTimer(fn, delay) { const key = ++id; pending.set(key, { fn, delay }); return key; },
    clearTimer(key) { pending.delete(key); }, fire(delay) {
      const selected = [...pending].filter(([, item]) => item.delay === delay);
      for (const [key, item] of selected) { pending.delete(key); item.fn(); }
    } };
}
class Socket {
  constructor() { this.readyState = 0; this.bufferedAmount = 0; this.sent = []; this.closes = []; }
  open() { this.readyState = 1; this.onopen(); }
  send(data) { this.sent.push(data); }
  close(code, reason) {
    assert(code === 1000 || code >= 3000 && code <= 4999, 'Browser close code must be allowed by WebSocket.close.');
    this.readyState = 2; this.closes.push({ code, reason });
  }
  closed(code = 1006) { this.readyState = 3; this.onclose({ code }); }
  message(data) { this.onmessage({ data: typeof data === 'object' && !(data instanceof Uint8Array) && !(data instanceof ArrayBuffer) ? JSON.stringify(data) : data }); }
}
function fixture(extra = {}) {
  const clock = timers(), sockets = [], events = [], frames = [], binaries = [];
  const session = new Session({ token: 'b'.repeat(64), url: 'ws://localhost:8766', ...clock,
    socketFactory() { const socket = new Socket(); sockets.push(socket); return socket; },
    onReady: (msg, id) => events.push({ ready: id, control: msg.control }),
    onDisconnect: e => events.push(e), onFrame: (msg, id) => frames.push({ msg, id }), onBinary: data => binaries.push(data), ...extra });
  return { clock, sockets, events, frames, binaries, session };
}
function hello(socket, control = false, codec = 'png') {
  socket.message({ type: 'hello', paired: true, control, codec, width: 1920, height: 1080 });
}
function pad() { const p = new Uint8Array(24); p.set([77, 67, 51, 73]); return p; }
const png = 'iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+/hN8AAAAASUVORK5CYII=';

test('only the host token is sent and input requires a paired, explicit controller grant', () => {
  const f = fixture(); f.session.connect(); const ws = f.sockets[0]; ws.open();
  assert.deepEqual(ws.sent, [handshake('b'.repeat(64))]);
  assert.equal(f.session.sendInput(pad()), false);
  hello(ws, false); assert.equal(f.session.paired, true); assert.equal(f.session.canControl, false);
  assert.equal(f.session.sendInput(pad()), false); assert.equal(ws.sent.length, 1);
  f.session.disconnect(); f.session.connect(); const controller = f.sockets[1]; controller.open(); hello(controller, true);
  assert.equal(f.session.sendInput(pad()), true); assert.equal(controller.sent.length, 2);
  controller.bufferedAmount = 65537; assert.equal(f.session.sendInput(pad()), false);
  controller.bufferedAmount = 0; assert.equal(f.session.sendInput(new Uint8Array(24)), false);
  f.session.disconnect(); assert.equal(f.clock.pending.size, 0);
});

test('retired socket callbacks cannot change, send through, close or render into a replacement', () => {
  const f = fixture(); const firstId = f.session.connect(); const old = f.sockets[0]; old.open(); hello(old);
  old.closed(); f.session.connect(); const current = f.sockets[1]; const secondId = f.session.connection.id;
  current.open(); hello(current, true);
  const before = f.events.length;
  old.onopen(); old.message({ type: 'hello', paired: false }); old.message({ type: 'frame', data: png }); old.closed();
  assert.equal(f.events.length, before); assert.equal(current.closes.length, 0); assert.equal(f.frames.length, 0);
  assert.equal(f.session.isCurrent(firstId), false); assert.equal(f.session.isCurrent(secondId), true);
  f.clock.fire(2000); assert.equal(f.sockets.length, 2); // Explicit connect cancelled the old retry.
  f.session.disconnect();
});

test('logout cancels retries and delayed callbacks; a current transient close may reconnect only while authorized', () => {
  let allowed = true;
  const f = fixture({ shouldReconnect: () => allowed }); f.session.connect(); f.sockets[0].open(); hello(f.sockets[0]);
  f.sockets[0].closed(); assert.equal(f.clock.pending.size, 1);
  allowed = false; f.clock.fire(2000); assert.equal(f.sockets.length, 1);
  allowed = true; f.session.connect(); f.sockets[1].closed();
  const savedRetry = [...f.clock.pending.values()][0].fn;
  f.session.disconnect(); savedRetry(); assert.equal(f.sockets.length, 2); assert.equal(f.clock.pending.size, 0);
});

test('pairing is time bounded, refused pairing is terminal, and frames before pairing are rejected', () => {
  for (const data of [{ type: 'frame', data: png }, new Uint8Array([0, 0, 1, 101]), { type: 'hello', paired: true, control: 'yes', codec: 'png' }, 'bad-json']) {
    const f = fixture(); f.session.connect(); const ws = f.sockets[0]; ws.open(); ws.message(data);
    assert.equal(f.session.paired, false); assert.equal(f.events.at(-1).code, 1008); assert.equal(ws.closes[0].code, 4008);
    assert.equal(f.clock.pending.size, 0);
  }
  const f = fixture(); f.session.connect(); f.clock.fire(5000);
  assert.equal(f.events.at(-1).code, 1008); assert.equal(f.sockets[0].closes[0].code, 4008);
  assert.equal(f.clock.pending.size, 0);
});

test('PNG headers and codec-specific frame sizes are bounded before rendering', () => {
  assert.deepEqual(pngDimensions(png), { width: 1, height: 1 });
  for (const [width, height] of [[0, 1], [1, 0], [8193, 1], [8192, 8192], [1.5, 2], [Infinity, 1]]) assert.equal(validDimensions(width, height), false);
  assert.equal(validDimensions(3840, 2160), true);
  const huge = Buffer.from(png, 'base64'); huge.writeUInt32BE(0xffffffff, 16);
  assert.equal(pngDimensions(huge.toString('base64')), null);
  assert.equal(pngDimensions('iVBOR-not-a-png'), null);
  const f = fixture(); f.session.connect(); f.sockets[0].open(); hello(f.sockets[0]);
  f.sockets[0].message({ type: 'frame', data: png }); assert.equal(f.frames.length, 1);
  f.sockets[0].message({ type: 'frame', data: huge.toString('base64') }); assert.equal(f.events.at(-1).code, 1009);
  const h = fixture(); h.session.connect(); h.sockets[0].open(); hello(h.sockets[0], false, 'h264');
  h.sockets[0].message(new Uint8Array(MAX_FRAME_BYTES + 1)); assert.equal(h.events.at(-1).code, 1009);
  assert.equal(h.binaries.length, 0);
});

test('unexpected codecs, oversized metadata, duplicate hello and codec switches fail closed', () => {
  for (const malformed of [
    { type: 'hello', paired: true, control: true, codec: 'javascript' },
    { type: 'hello', paired: true, control: true, codec: 'png', width: 32768, height: 32768 }
  ]) {
    const f = fixture(); f.session.connect(); f.sockets[0].open(); f.sockets[0].message(malformed);
    assert.equal(f.session.paired, false); assert.equal(f.clock.pending.size, 0);
  }
  for (const data of [{ type: 'hello', paired: true, control: false, codec: 'png' }, { type: 'ping', payload: 'a'.repeat(16384) }, new Uint8Array([1])]) {
    const f = fixture(); f.session.connect(); f.sockets[0].open(); hello(f.sockets[0]); f.sockets[0].message(data);
    assert.equal(f.session.paired, false); assert.equal(f.clock.pending.size, 0);
  }
});

test('an SPS is not a key picture and truncated Annex-B delimiters do not qualify', () => {
  assert.equal(hasIDR(new Uint8Array([0, 0, 1, 103, 1])), false);
  assert.equal(hasIDR(new Uint8Array([0, 0, 0, 1])), false);
  assert.equal(hasIDR(new Uint8Array([0, 0, 1, 101, 1])), true);
  assert.equal(hasIDR(new Uint8Array([0, 0, 0, 1, 101, 1])), true);
});

function decoderFixture(extra = {}) {
  const clock = timers(), decoders = [], rendered = [], errors = [];
  class Decoder {
    static isConfigSupported() { return Promise.resolve({ supported: true }); }
    constructor(callbacks) { this.callbacks = callbacks; this.state = 'unconfigured'; this.decodeQueueSize = 0; this.chunks = []; this.resets = 0; decoders.push(this); }
    configure(config) { this.config = config; this.state = 'configured'; }
    decode(chunk) { this.chunks.push(chunk); }
    reset() { ++this.resets; this.decodeQueueSize = 0; this.state = 'unconfigured'; }
    close() { this.state = 'closed'; }
  }
  const receiver = new AnnexBDecoder({ Decoder, Chunk: class { constructor(value) { Object.assign(this, value); } }, ...clock,
    now: () => 7, onFrame: frame => rendered.push(frame), onError: e => errors.push(e), ...extra });
  return { clock, decoders, rendered, errors, receiver, Decoder };
}
const settle = () => new Promise(resolve => setImmediate(resolve));

test('decoder initialization and output belong to the current connection and every frame is closed', async () => {
  const f = decoderFixture(); f.receiver.start(); await settle();
  const old = f.decoders[0]; f.receiver.stop();
  const frame = { codedWidth: 1920, codedHeight: 1080, closed: 0, close() { ++this.closed; } };
  old.callbacks.output(frame); assert.equal(frame.closed, 1); assert.equal(f.rendered.length, 0);
  f.receiver.start(); await settle(); f.decoders[1].callbacks.output(frame);
  assert.equal(frame.closed, 2); assert.equal(f.rendered.length, 1);
  old.callbacks.error(new Error('stale')); assert.equal(f.errors.length, 0); assert.equal(f.decoders[1].state, 'configured');
  f.receiver.stop(); assert.equal(f.clock.pending.size, 0);
});

test('late codec support checks cannot create a decoder after disconnect and stalled checks time out', async () => {
  let complete;
  class Deferred { static isConfigSupported() { return new Promise(resolve => complete = resolve); } }
  const f = decoderFixture({ Decoder: Deferred }); f.receiver.start(); await settle(); f.receiver.stop();
  complete({ supported: true }); await settle(); assert.equal(f.decoders.length, 0); assert.equal(f.errors.length, 0);
  f.receiver.start(); await settle(); f.clock.fire(5000); assert.equal(f.errors.length, 1);
  complete({ supported: true }); await settle(); assert.equal(f.decoders.length, 0);
});

test('decoder backlog is bounded and dropping a reference chain waits for a fresh IDR', async () => {
  const f = decoderFixture(); f.receiver.start(); await settle(); const decoder = f.decoders[0];
  const key = new Uint8Array([0, 0, 1, 101, 1]), delta = new Uint8Array([0, 0, 1, 65, 1]);
  f.receiver.push(delta); assert.equal(decoder.chunks.length, 0);
  f.receiver.push(key); f.receiver.push(delta); assert.equal(decoder.chunks.length, 2);
  assert(decoder.chunks[1].timestamp > decoder.chunks[0].timestamp);
  decoder.decodeQueueSize = 4; f.receiver.push(delta); assert.equal(decoder.resets, 1); assert.equal(decoder.chunks.length, 2);
  f.receiver.push(delta); assert.equal(decoder.chunks.length, 2);
  f.receiver.push(key); assert.equal(decoder.chunks.at(-1).type, 'key');
  f.receiver.stop();
});

test('invalid decoded dimensions close frames and unsupported H.264 reports an honest error', async () => {
  const f = decoderFixture(); f.receiver.start(); await settle();
  const frame = { codedWidth: 32768, codedHeight: 32768, closed: false, close() { this.closed = true; } };
  f.decoders[0].callbacks.output(frame); assert.equal(frame.closed, true); assert.equal(f.rendered.length, 0); assert.equal(f.errors.length, 1);
  class Unsupported { static isConfigSupported() { return Promise.resolve({ supported: false }); } }
  const unsupported = decoderFixture({ Decoder: Unsupported }); unsupported.receiver.start(); await settle();
  assert.match(unsupported.errors[0], /Ask the host to use PNG/); assert.equal(unsupported.clock.pending.size, 0);
});
