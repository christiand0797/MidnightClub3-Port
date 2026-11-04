'use strict';
const test = require('node:test');
const assert = require('node:assert/strict');
const { baseline, detect, rows } = require('./capabilities');
const environment = gpu => ({ isSecureContext: true, crossOriginIsolated: true, WebAssembly, SharedArrayBuffer,
  Worker: function () {}, AudioContext: function () {}, navigator: { gpu, getGamepads() { return []; } } });
test('uses actual Wasm validation and shared memory; no game or GPU readiness invented', () => {
  const result = baseline(environment());
  assert.equal(result.wasm, true); assert.equal(result.simd, true); assert.equal(result.sharedMemory, true);
  assert.equal(result.localGame, false); assert.equal(result.gpu, 'unchecked');
});
test('insecure context never requests an adapter', async () => {
  let called = false; const env = environment({ requestAdapter() { called = true; } }); env.isSecureContext = false;
  assert.equal((await detect(env)).gpu, 'insecure'); assert.equal(called, false);
});
test('missing and null adapters are distinguished', async () => {
  assert.equal((await detect(environment())).gpu, 'unavailable');
  assert.equal((await detect(environment({ requestAdapter: async () => null }))).gpu, 'no-adapter');
});
test('adapter availability does not claim a game build or hardware performance', async () => {
  let options; const result = await detect(environment({ async requestAdapter(value) { options = value; return {}; } }));
  assert.equal(options.powerPreference, 'high-performance'); assert.equal(result.gpu, 'available'); assert.equal(result.localGame, false);
  assert.match(rows(result)[0][1], /still in development/);
});
test('reports fallback adapters from current and older APIs', async () => {
  for (const adapter of [{ info: { isFallbackAdapter: true } }, { isFallbackAdapter: true }])
    assert.equal((await detect(environment({ requestAdapter: async () => adapter }))).gpu, 'fallback');
});
test('rejection and timeout are bounded and handled', async () => {
  assert.equal((await detect(environment({ requestAdapter: async () => { throw Error('driver failure'); } }))).gpu, 'error');
  assert.equal((await detect(environment({ requestAdapter: () => new Promise(() => {}) }), 5)).gpu, 'timeout');
});
test('cross-origin isolation is required even when SharedArrayBuffer exists', () => {
  const env = environment(); env.crossOriginIsolated = false;
  assert.equal(baseline(env).sharedMemory, false); assert.match(rows(baseline(env))[3][1], /isolation headers/);
});
test('unsupported APIs remain visible without crashing', () => {
  const result = baseline({}); assert.equal(result.wasm, false); assert.equal(result.sharedMemory, false);
  assert.equal(result.audio, false); assert.equal(result.gamepad, false); assert.equal(rows(result).length, 6);
});
