'use strict';
// Feature checks, not browser-name guesses. No device allocation or rendering is performed here.
(function (root, factory) {
  const api = factory();
  if (typeof module === 'object' && module.exports) module.exports = api;
  else root.MC3Capabilities = api;
})(typeof globalThis === 'object' ? globalThis : this, function () {
  // Minimal valid module containing i32.const(0), i8x16.splat, drop.
  const SIMD = new Uint8Array([0,97,115,109,1,0,0,0,1,4,1,96,0,0,3,2,1,0,10,9,1,7,0,65,0,253,15,26,11]);
  function baseline(env) {
    let wasm = false, simd = false, sharedMemory = false;
    try {
      wasm = typeof env.WebAssembly?.validate === 'function' && env.WebAssembly.validate(new Uint8Array([0,97,115,109,1,0,0,0]));
      simd = wasm && env.WebAssembly.validate(SIMD);
      if (wasm && env.crossOriginIsolated === true && typeof env.SharedArrayBuffer === 'function') {
        const memory = new env.WebAssembly.Memory({ initial: 1, maximum: 1, shared: true });
        sharedMemory = memory.buffer instanceof env.SharedArrayBuffer;
      }
    } catch (_) { /* Unsupported browser feature remains unavailable. */ }
    return { secure: env.isSecureContext === true, wasm, simd, sharedMemory,
      isolated: env.crossOriginIsolated === true, workers: typeof env.Worker === 'function',
      audio: typeof (env.AudioContext || env.webkitAudioContext) === 'function',
      gamepad: typeof env.navigator?.getGamepads === 'function', gpu: 'unchecked',
      // There is currently no compiled browser game runtime. An adapter is not a game build.
      localGame: false };
  }
  async function detect(env = globalThis, timeoutMs = 4000) {
    const result = baseline(env);
    if (!result.secure) { result.gpu = 'insecure'; return result; }
    if (typeof env.navigator?.gpu?.requestAdapter !== 'function') { result.gpu = 'unavailable'; return result; }
    let timer;
    try {
      const timeout = new Promise(resolve => { timer = setTimeout(() => resolve('timeout'), Math.max(1, Math.min(timeoutMs, 10000))); });
      const request = Promise.resolve().then(() => env.navigator.gpu.requestAdapter({ powerPreference: 'high-performance' }))
        .then(adapter => adapter ? (adapter.info?.isFallbackAdapter === true || adapter.isFallbackAdapter === true ? 'fallback' : 'available') : 'no-adapter');
      result.gpu = await Promise.race([request, timeout]);
    } catch (_) { result.gpu = 'error'; }
    finally { clearTimeout(timer); }
    return result;
  }
  function rows(result) {
    const gpu = { unchecked: 'Not checked', insecure: 'HTTPS or localhost required', unavailable: 'Unavailable in this browser',
      available: 'Adapter available', fallback: 'Fallback adapter only', 'no-adapter': 'No compatible adapter',
      timeout: 'Check timed out — try again', error: 'Browser could not provide an adapter' };
    return [
      ['Game view', 'Native host stream; local browser game is still in development'],
      ['WebGPU', gpu[result.gpu] || 'Not checked'],
      ['WebAssembly', result.wasm ? (result.simd ? 'Available with SIMD' : 'Available; SIMD unavailable') : 'Unavailable'],
      ['Game workers', result.workers && result.sharedMemory ? 'Shared memory available' :
        !result.isolated ? 'Server isolation headers required' : 'Shared memory unavailable'],
      ['WebAudio', result.audio ? 'Available; sound starts after interaction' : 'Unavailable'],
      ['Controller API', result.gamepad ? 'Available; connect a controller and press a button' : 'Unavailable']
    ];
  }
  function mountPanel(parent, env = globalThis) {
    const doc = parent.ownerDocument;
    const section = doc.createElement('section'); section.className = 'hardware-panel';
    const title = doc.createElement('h3'); title.textContent = 'Browser hardware';
    const help = doc.createElement('p'); help.textContent = 'Firefox and Chromium are checked by their available features. Your browser and graphics driver choose the GPU.';
    const list = doc.createElement('dl'); list.setAttribute('aria-live', 'polite');
    const button = doc.createElement('button'); button.type = 'button'; button.textContent = 'Check hardware';
    function render(result) {
      list.replaceChildren();
      for (const [name, value] of rows(result)) {
        const term = doc.createElement('dt'), detail = doc.createElement('dd');
        term.textContent = name; detail.textContent = value; list.append(term, detail);
      }
    }
    render(baseline(env));
    button.addEventListener('click', async () => {
      if (button.disabled) return;
      button.disabled = true; button.textContent = 'Checking…';
      try { render(await detect(env)); }
      finally { button.disabled = false; button.textContent = 'Check hardware again'; }
    });
    section.append(title, help, list, button); parent.append(section);
    return section;
  }
  return { baseline, detect, rows, mountPanel };
});
