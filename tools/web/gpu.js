// Browser port, step 4: runs the game's recorded GPU commands with WebGPU on the page's main thread
// (tools/web/webgpu_backend.cpp records them; docs/notes/browser_port_plan.md). Loaded by play.html.
//
// Ring (web_gpu_ring(), u32 words): 0 published frame, 1 consumed frame, 2-3 words in buffer 0/1, 4 overflowed
// frames, 5-7 reserved, 8-9 buffer 0/1 addresses. Commands: op, payload word count, payload.
'use strict';
window.MC3Gpu = (() => {
  const OP_CLEAR = 1, OP_PRESENT = 2;
  let device = null, context = null, format = null, M = null, ring = 0;
  let clear = { r: 0, g: 0, b: 0, a: 1 };
  let frames = 0;

  async function init(canvas) {
    if (!navigator.gpu) return 'This browser has no WebGPU';
    const adapter = await navigator.gpu.requestAdapter({ powerPreference: 'high-performance' });
    if (!adapter) return 'No WebGPU adapter';
    device = await adapter.requestDevice();
    device.lost.then(info => console.warn('webgpu: device lost:', info.message));
    format = navigator.gpu.getPreferredCanvasFormat();
    context = canvas.getContext('webgpu');
    context.configure({ device, format, alphaMode: 'opaque' });
    const info = adapter.info || {};
    console.log(`webgpu: ${info.vendor || '?'} ${info.architecture || ''} ${info.description || ''}`.trim());
    return null;
  }

  function attach(module) {
    M = module;
    ring = M._web_gpu_ring() >>> 2;
  }

  function run(words, start, count, last) {
    const end = start + count;
    let at = start;
    while (at + 2 <= end) {
      const op = words[at], n = words[at + 1], p = at + 2;
      if (op === OP_CLEAR) {
        const flags = words[p], color = words[p + 1], rects = words[p + 4];
        if ((flags & 0xF0) && rects === 0) {  // a whole-target colour clear sets the background
          clear = { r: ((color >>> 16) & 255) / 255, g: ((color >>> 8) & 255) / 255, b: (color & 255) / 255, a: 1 };
        }
      } else if (op === OP_PRESENT && last) {
        present(words[p], words[p + 1]);
      }
      at = p + n;
    }
  }

  function present(width, height) {
    const canvas = context.canvas;
    if (canvas.width !== width || canvas.height !== height) { canvas.width = width; canvas.height = height; }
    const encoder = device.createCommandEncoder();
    const pass = encoder.beginRenderPass({
      colorAttachments: [{ view: context.getCurrentTexture().createView(), loadOp: 'clear', storeOp: 'store', clearValue: clear }],
    });
    pass.end();
    device.queue.submit([encoder.finish()]);
    frames++;
  }

  // once per animation frame: run every frame the game has published since the last call, present the newest
  function frame() {
    if (!M || !device) return 0;
    const i32 = new Int32Array(M.HEAPU32.buffer), u32 = M.HEAPU32;
    const published = Atomics.load(i32, ring) >>> 0;
    let consumed = Atomics.load(i32, ring + 1) >>> 0;
    while (consumed < published) {
      const f = consumed + 1;
      const buffer = u32[ring + 8 + (f & 1)] >>> 2;
      run(u32, buffer, u32[ring + 2 + (f & 1)], f === published);
      consumed = f;
      Atomics.store(i32, ring + 1, consumed);
    }
    const shown = frames;
    frames = 0;
    return shown;
  }

  return { init, attach, frame };
})();
