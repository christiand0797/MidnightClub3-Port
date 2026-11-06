// Browser port, step 4: runs the game's recorded GPU commands with WebGPU on the page's main thread
// (tools/web/webgpu_backend.cpp records them; docs/notes/browser_port_plan.md). Loaded by play.html.
//
// Ring (web_gpu_ring(), u32 words): 0 published frame, 1 consumed frame, 2-3 words in buffer 0/1, 4 overflowed
// frames, 5-7 reserved, 8-9 buffer 0/1 addresses. Commands: op, payload word count, payload.
// Draws arrive as transformed triangles (screen x y, z, w, colours, four texture coordinates, fog); this side
// rasterizes them into one texture per game render target, runs the fixed function texture stages in WGSL, alpha tests
// and blends, then shows the target the game presents.
'use strict';
window.MC3Gpu = (() => {
  const OP_CLEAR = 1, OP_PRESENT = 2, OP_TEXTURE = 3, OP_DRAW = 4, OP_COPY = 5;
  const DRAW_HEADER = 98, VERTEX_FLOATS = 32, UNIFORM_STRIDE = 512, MAX_DRAWS = 16384;

  const SHADER = /* wgsl */ `
struct Draw {
  ff: array<vec4<u32>, 9>,   // 4 stages x 9: colorop arg1 arg2 arg0 alphaop arg1 arg2 arg0 result
  tfactor: vec4f,
  fogc: vec4f,
  p: vec4<u32>,              // alphakill bits, alpha test on, alpha func (GL), alpha ref
  tex: array<vec4f, 4>,      // width, height, rect, bound
  tgt: vec4f,                // target width, height, zscale, programmable
};
@group(0) @binding(0) var<uniform> D: Draw;
@group(0) @binding(1) var T0: texture_2d<f32>;
@group(0) @binding(2) var T1: texture_2d<f32>;
@group(0) @binding(3) var T2: texture_2d<f32>;
@group(0) @binding(4) var T3: texture_2d<f32>;
@group(0) @binding(5) var S0: sampler;
@group(0) @binding(6) var S1: sampler;
@group(0) @binding(7) var S2: sampler;
@group(0) @binding(8) var S3: sampler;

struct VIn {
  @location(0) pos: vec4f, @location(1) d0: vec4f, @location(2) d1: vec4f,
  @location(3) t0: vec4f, @location(4) t1: vec4f, @location(5) t2: vec4f, @location(6) t3: vec4f, @location(7) fog: f32,
};
struct VOut {
  @builtin(position) pos: vec4f, @location(0) d0: vec4f, @location(1) d1: vec4f,
  @location(2) t0: vec4f, @location(3) t1: vec4f, @location(4) t2: vec4f, @location(5) t3: vec4f, @location(6) fog: f32,
};

@vertex fn vs(v: VIn) -> VOut {
  var o: VOut;
  let w = v.pos.w;
  let x = v.pos.x / D.tgt.x * 2.0 - 1.0;
  let y = 1.0 - v.pos.y / D.tgt.y * 2.0;
  let z = v.pos.z / max(D.tgt.z, 1e-6);
  o.pos = vec4f(x * w, y * w, z * w, w);
  o.d0 = v.d0; o.d1 = v.d1; o.t0 = v.t0; o.t1 = v.t1; o.t2 = v.t2; o.t3 = v.t3; o.fog = v.fog;
  return o;
}

fn ffw(k: u32) -> u32 { return D.ff[k / 4u][k % 4u]; }
fn uvof(i: u32, t: vec4f) -> vec2f {
  if (D.tex[i].z > 0.5) { return t.xy / max(D.tex[i].xy, vec2f(1.0)); }
  return t.xy;
}
fn arg(a: u32, v0: vec4f, v1: vec4f, cur: vec4f, tmp: vec4f, tex: vec4f) -> vec4f {
  var x = cur;
  switch (a & 15u) {
    case 0u: { x = v0; }
    case 2u: { x = tex; }
    case 3u: { x = D.tfactor; }
    case 4u: { x = v1; }
    case 5u: { x = tmp; }
    default: { x = cur; }
  }
  if ((a & 0x20u) != 0u) { x = vec4f(x.w); }
  if ((a & 0x10u) != 0u) { x = vec4f(1.0) - x; }
  return x;
}
fn op(o: u32, a1: vec4f, a2: vec4f, a0: vec4f, j: u32, alpha: bool, v0: vec4f, cur: vec4f, tex: vec4f) -> f32 {
  let x = a1[j]; let y = a2[j]; let z = a0[j];
  switch (o) {
    case 2u: { return x; }
    case 3u: { return y; }
    case 4u: { return x * y; }
    case 5u: { return x * y * 2.0; }
    case 6u: { return x * y * 4.0; }
    case 7u: { return x + y; }
    case 8u: { return x + y - 0.5; }
    case 9u: { return (x + y - 0.5) * 2.0; }
    case 10u: { return x - y; }
    case 11u: { return x + y * (1.0 - x); }
    case 12u: { return y + (x - y) * v0.w; }
    case 13u: { return y + (x - y) * cur.w; }
    case 14u: { return y + (x - y) * tex.w; }
    case 15u: { return y + (x - y) * D.tfactor.w; }
    case 16u: { return x + y * (1.0 - tex.w); }
    case 18u: { if (alpha) { return x; } return x + y * a1.w; }
    case 19u: { if (alpha) { return x; } return x * y + a1.w; }
    case 20u: { if (alpha) { return x; } return x + y * (1.0 - a1.w); }
    case 21u: { if (alpha) { return x; } return (1.0 - x) * y + a1.w; }
    case 22u: { return clamp(dot((a1.xyz - 0.5) * 2.0, (a2.xyz - 0.5) * 2.0), 0.0, 1.0); }
    case 23u: { return z + x * y; }
    case 24u: { return y + (x - y) * z; }
    default: { if (alpha) { return cur.w; } return cur[j]; }
  }
}
fn passes(f: u32, a: u32, b: u32) -> bool {
  switch (f) {
    case 0x200u: { return false; }
    case 0x201u: { return a < b; }
    case 0x202u: { return a == b; }
    case 0x203u: { return a <= b; }
    case 0x204u: { return a > b; }
    case 0x205u: { return a != b; }
    case 0x206u: { return a >= b; }
    default: { return true; }
  }
}

@fragment fn fs(v: VOut) -> @location(0) vec4f {
  var texs = array<vec4f, 4>(
    textureSample(T0, S0, uvof(0u, v.t0)), textureSample(T1, S1, uvof(1u, v.t1)),
    textureSample(T2, S2, uvof(2u, v.t2)), textureSample(T3, S3, uvof(3u, v.t3)));
  let v0 = clamp(v.d0, vec4f(0.0), vec4f(1.0));
  let v1 = clamp(v.d1, vec4f(0.0), vec4f(1.0));
  var cur = v0;
  var tmp = vec4f(0.0);
  if (D.tgt.w > 0.5) {
    // pixel shader combiners: not translated yet, stage 0's texture by the diffuse colour
    if (D.tex[0].w > 0.5) { cur = texs[0] * v0; }
  } else {
    for (var i = 0u; i < 4u; i++) {
      let b = i * 9u;
      let cop = ffw(b);
      if (cop <= 1u) { break; }
      var tex = vec4f(1.0);
      if (D.tex[i].w > 0.5) { tex = texs[i]; }
      if (((D.p.x >> i) & 1u) != 0u && tex.w == 0.0) { discard; }
      let c1 = arg(ffw(b + 1u), v0, v1, cur, tmp, tex);
      let c2 = arg(ffw(b + 2u), v0, v1, cur, tmp, tex);
      let c0 = arg(ffw(b + 3u), v0, v1, cur, tmp, tex);
      var r: vec4f;
      for (var j = 0u; j < 3u; j++) { r[j] = clamp(op(cop, c1, c2, c0, j, false, v0, cur, tex), 0.0, 1.0); }
      let aop = ffw(b + 4u);
      if (aop <= 1u) {
        r.w = cur.w;
      } else {
        let a1 = arg(ffw(b + 5u), v0, v1, cur, tmp, tex);
        let a2 = arg(ffw(b + 6u), v0, v1, cur, tmp, tex);
        let a0 = arg(ffw(b + 7u), v0, v1, cur, tmp, tex);
        r.w = clamp(op(aop, a1, a2, a0, 3u, true, v0, cur, tex), 0.0, 1.0);
      }
      if (ffw(b + 8u) == 5u) { tmp = r; } else { cur = r; }
    }
  }
  var lit = clamp(cur.rgb + v1.rgb, vec3f(0.0), vec3f(1.0));
  if (D.tgt.w > 0.5) { lit = clamp(cur.rgb, vec3f(0.0), vec3f(1.0)); }  // combiners: v1 is theirs to use
  let f = clamp(v.fog, 0.0, 1.0);
  let outc = vec4f(D.fogc.rgb * (1.0 - f) + lit * f, clamp(cur.w, 0.0, 1.0));
  if (D.p.y != 0u && !passes(D.p.z, u32(round(outc.w * 255.0)), D.p.w)) { discard; }
  return outc;
}
`;

  const BLIT = /* wgsl */ `
@group(0) @binding(0) var T: texture_2d<f32>;
@group(0) @binding(1) var S: sampler;
struct O { @builtin(position) p: vec4f, @location(0) uv: vec2f };
@vertex fn vs(@builtin(vertex_index) i: u32) -> O {
  let q = array<vec2f, 3>(vec2f(-1.0, -1.0), vec2f(3.0, -1.0), vec2f(-1.0, 3.0));
  var o: O; o.p = vec4f(q[i], 0.0, 1.0); o.uv = vec2f((q[i].x + 1.0) * 0.5, (1.0 - q[i].y) * 0.5); return o;
}
@fragment fn fs(o: O) -> @location(0) vec4f { return vec4f(textureSample(T, S, o.uv).rgb, 1.0); }
`;

  const FACTOR = { 0: 'zero', 1: 'one', 0x300: 'src', 0x301: 'one-minus-src', 0x302: 'src-alpha', 0x303: 'one-minus-src-alpha',
    0x304: 'dst-alpha', 0x305: 'one-minus-dst-alpha', 0x306: 'dst', 0x307: 'one-minus-dst', 0x308: 'src-alpha-saturated',
    0x8001: 'constant', 0x8002: 'one-minus-constant', 0x8003: 'constant', 0x8004: 'one-minus-constant' };
  const BLEND_OP = { 0x8006: 'add', 0x800A: 'subtract', 0x800B: 'reverse-subtract', 0x8007: 'min', 0x8008: 'max' };
  const COMPARE = { 0x200: 'never', 0x201: 'less', 0x202: 'equal', 0x203: 'less-equal', 0x204: 'greater', 0x205: 'not-equal',
    0x206: 'greater-equal', 0x207: 'always' };
  const ADDRESS = { 1: 'repeat', 2: 'mirror-repeat' };

  let device, context, canvasFormat, M = null, ring = 0;
  let module, blitModule, layout, pipelineLayout, blitPipeline, white;
  const pipelines = new Map(), samplers = new Map(), textures = new Map(), targets = new Map(), groups = new Map();
  let vbuf = null, ibuf = null, ubuf = null, vcap = 0, icap = 0;
  let frames = 0, lastFront = 0;
  const stats = { draws: 0, textures: 0, programmable: 0, missing: 0, targetTex: 0, bound: 0 };

  async function init(canvas) {
    if (!navigator.gpu) return 'This browser has no WebGPU';
    const adapter = await navigator.gpu.requestAdapter({ powerPreference: 'high-performance' });
    if (!adapter) return 'No WebGPU adapter';
    device = await adapter.requestDevice();
    device.lost.then(info => console.warn('webgpu: device lost:', info.message));
    device.addEventListener?.('uncapturederror', e => console.warn('webgpu:', e.error.message));
    canvasFormat = navigator.gpu.getPreferredCanvasFormat();
    context = canvas.getContext('webgpu');
    context.configure({ device, format: canvasFormat, alphaMode: 'opaque' });
    const info = adapter.info || {};
    console.log(`webgpu: ${info.vendor || '?'} ${info.architecture || ''} ${info.description || ''}`.trim());

    module = device.createShaderModule({ code: SHADER });
    blitModule = device.createShaderModule({ code: BLIT });
    const entries = [{ binding: 0, visibility: GPUShaderStage.VERTEX | GPUShaderStage.FRAGMENT, buffer: { type: 'uniform', hasDynamicOffset: true } }];
    for (let i = 0; i < 4; i++) entries.push({ binding: 1 + i, visibility: GPUShaderStage.FRAGMENT, texture: {} });
    for (let i = 0; i < 4; i++) entries.push({ binding: 5 + i, visibility: GPUShaderStage.FRAGMENT, sampler: {} });
    layout = device.createBindGroupLayout({ entries });
    pipelineLayout = device.createPipelineLayout({ bindGroupLayouts: [layout] });
    blitPipeline = device.createRenderPipeline({ layout: 'auto', vertex: { module: blitModule, entryPoint: 'vs' },
      fragment: { module: blitModule, entryPoint: 'fs', targets: [{ format: canvasFormat }] }, primitive: { topology: 'triangle-list' } });
    white = device.createTexture({ size: [1, 1], format: 'rgba8unorm', usage: GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.COPY_DST });
    device.queue.writeTexture({ texture: white }, new Uint8Array([255, 255, 255, 255]), { bytesPerRow: 4 }, [1, 1]);
    ubuf = device.createBuffer({ size: UNIFORM_STRIDE * MAX_DRAWS, usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST });
    return null;
  }

  function attach(module_) { M = module_; ring = M._web_gpu_ring() >>> 2; }

  function sampler(u, v, linear) {
    const key = `${u}|${v}|${linear}`;
    let s = samplers.get(key);
    if (!s) {
      s = device.createSampler({ addressModeU: ADDRESS[u] || 'clamp-to-edge', addressModeV: ADDRESS[v] || 'clamp-to-edge',
        magFilter: linear ? 'linear' : 'nearest', minFilter: linear ? 'linear' : 'nearest' });
      samplers.set(key, s);
    }
    return s;
  }

  function target(key, w, h) {
    let t = targets.get(key);
    if (!t || t.w !== w || t.h !== h) {
      t?.color.destroy(); t?.depth.destroy();
      t = { w, h,
        color: device.createTexture({ size: [w, h], format: 'rgba8unorm', usage: GPUTextureUsage.RENDER_ATTACHMENT | GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.COPY_SRC | GPUTextureUsage.COPY_DST }),
        depth: device.createTexture({ size: [w, h], format: 'depth24plus-stencil8', usage: GPUTextureUsage.RENDER_ATTACHMENT }) };
      t.view = t.color.createView(); t.dview = t.depth.createView();
      targets.set(key, t);
    }
    return t;
  }

  function pipeline(h) {
    const blend = h[11] !== 0, src = h[12], dst = h[13], bop = h[14], mask = h[16];
    const ztest = h[17] !== 0, zwrite = h[18] !== 0, zfunc = h[19], cull = h[20];
    const key = `${blend ? `${src}.${dst}.${bop}` : '-'}|${mask}|${ztest ? zfunc : 0}|${zwrite}|${cull}`;
    let p = pipelines.get(key);
    if (p) return p;
    const writeMask = ((mask >>> 16) & 1 ? 1 : 0) | ((mask >>> 8) & 1 ? 2 : 0) | (mask & 1 ? 4 : 0) | ((mask >>> 24) & 1 ? 8 : 0);
    let blendState;
    if (blend) {
      const operation = BLEND_OP[bop] || 'add', minmax = operation === 'min' || operation === 'max';
      const c = { operation, srcFactor: minmax ? 'one' : (FACTOR[src] || 'one'), dstFactor: minmax ? 'one' : (FACTOR[dst] || 'zero') };
      blendState = { color: c, alpha: c };
    }
    // screen y runs down, clip y up: a triangle clockwise on screen is clockwise in clip space too (GL_CW 0x900)
    const primitive = { topology: 'triangle-list' };
    if (cull === 0x900) { primitive.frontFace = 'ccw'; primitive.cullMode = 'back'; }
    else if (cull === 0x901) { primitive.frontFace = 'cw'; primitive.cullMode = 'back'; }
    const attrs = [];
    for (let i = 0; i < 7; i++) attrs.push({ shaderLocation: i, offset: i * 16, format: 'float32x4' });
    attrs.push({ shaderLocation: 7, offset: 112, format: 'float32' });
    p = device.createRenderPipeline({
      layout: pipelineLayout,
      vertex: { module, entryPoint: 'vs', buffers: [{ arrayStride: VERTEX_FLOATS * 4, attributes: attrs }] },
      fragment: { module, entryPoint: 'fs', targets: [{ format: 'rgba8unorm', blend: blendState, writeMask }] },
      primitive,
      depthStencil: { format: 'depth24plus-stencil8', depthWriteEnabled: zwrite, depthCompare: ztest ? (COMPARE[zfunc] || 'always') : 'always' },
    });
    pipelines.set(key, p);
    return p;
  }

  function texture(key, w, h, data) {
    let t = textures.get(key);
    if (!t || t.w !== w || t.h !== h) {
      t?.tex.destroy();
      t = { w, h, tex: device.createTexture({ size: [w, h], format: 'rgba8unorm', usage: GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.COPY_DST }) };
      t.view = t.tex.createView();
      t.gen = (textures.get(key)?.gen || 0) + 1;
      textures.set(key, t);
    }
    device.queue.writeTexture({ texture: t.tex }, data, { bytesPerRow: w * 4 }, [w, h]);
    stats.textures++;
  }

  function bindGroup(h) {
    const views = [], samps = [];
    let key = '';
    for (let i = 0; i < 4; i++) {
      const o = 66 + i * 8, isTarget = (h[o + 7] & 2) !== 0;
      // a render target drawn on the GPU is sampled straight from its texture
      const t = !h[o] ? null : isTarget ? targets.get(h[o]) : textures.get(h[o]);
      if (h[o] && !t) stats.missing++; else if (t) { stats.bound++; if (isTarget) stats.targetTex++; }
      const gen = t ? (isTarget ? 'rt' + t.w : t.gen) : 0;
      views.push(t ? t.view : white.createView());
      samps.push(sampler(h[o + 5], h[o + 6], h[o + 7] & 1));
      key += t ? `${h[o]}:${gen}:${h[o + 5]}${h[o + 6]}${h[o + 7]},` : '-,';
    }
    let g = groups.get(key);
    if (!g) {
      const entries = [{ binding: 0, resource: { buffer: ubuf, size: 272 } }];
      for (let i = 0; i < 4; i++) entries.push({ binding: 1 + i, resource: views[i] });
      for (let i = 0; i < 4; i++) entries.push({ binding: 5 + i, resource: samps[i] });
      g = device.createBindGroup({ layout, entries });
      if (groups.size > 4096) groups.clear();
      groups.set(key, g);
    }
    return g;
  }

  // one frame's commands: uploads first (queued before the submit), then the passes
  function run(u32, start, count, present) {
    const f32 = new Float32Array(u32.buffer);
    const draws = [], clears = [];
    let vfloats = 0, iwords = 0;
    // pass 1: textures, and the sizes of the vertex and index data
    for (let at = start, end = start + count; at + 2 <= end;) {
      const op = u32[at], n = u32[at + 1], p = at + 2;
      if (op === OP_TEXTURE) {
        const w = u32[p + 1], h = u32[p + 2];
        texture(u32[p], w, h, new Uint8Array(u32.buffer, (p + 3) * 4, w * h * 4).slice());
      } else if (op === OP_DRAW) {
        vfloats += u32[p + 9] * VERTEX_FLOATS; iwords += u32[p + 10];
      }
      at = p + n;
    }
    const verts = new Float32Array(vfloats), idx = new Uint32Array(iwords), uni = new ArrayBuffer(UNIFORM_STRIDE * MAX_DRAWS);
    const uniU = new Uint32Array(uni), uniF = new Float32Array(uni);
    let vat = 0, iat = 0, nd = 0;
    const encoder = device.createCommandEncoder();
    let pass = null, passKey = 0;
    const begin = (key, w, h, clearColor, clearDepth) => {
      pass?.end();
      const t = target(key, w, h);
      pass = encoder.beginRenderPass({
        colorAttachments: [{ view: t.view, loadOp: clearColor ? 'clear' : 'load', storeOp: 'store', clearValue: clearColor || [0, 0, 0, 0] }],
        depthStencilAttachment: { view: t.dview, depthLoadOp: clearDepth ? 'clear' : 'load', depthStoreOp: 'store',
          depthClearValue: clearDepth ? clearDepth.z : 1, stencilLoadOp: clearDepth ? 'clear' : 'load', stencilStoreOp: 'store',
          stencilClearValue: clearDepth ? clearDepth.s : 0 },
      });
      pass.setVertexBuffer(0, vbuf);
      pass.setIndexBuffer(ibuf, 'uint32');
      passKey = key;
      return t;
    };
    // the frame's vertex and index data go up before the passes run
    if (!vbuf || vfloats * 4 > vcap) { vbuf?.destroy(); vcap = Math.max(vfloats * 4, 4 << 20) * 2; vbuf = device.createBuffer({ size: vcap, usage: GPUBufferUsage.VERTEX | GPUBufferUsage.COPY_DST }); }
    if (!ibuf || iwords * 4 > icap) { ibuf?.destroy(); icap = Math.max(iwords * 4, 1 << 20) * 2; ibuf = device.createBuffer({ size: icap, usage: GPUBufferUsage.INDEX | GPUBufferUsage.COPY_DST }); }
    let front = 0, fw = 0, fh = 0;
    for (let at = start, end = start + count; at + 2 <= end;) {
      const op = u32[at], n = u32[at + 1], p = at + 2;
      if (op === OP_CLEAR) {
        const flags = u32[p], color = u32[p + 1], rt = u32[p + 4];
        const t = targets.get(rt);
        const w = t ? t.w : u32[p + 6], h = t ? t.h : u32[p + 7];
        const c = (flags & 0xF0) ? [((color >>> 16) & 255) / 255, ((color >>> 8) & 255) / 255, (color & 255) / 255, ((color >>> 24) & 255) / 255] : null;
        const zf = f32[p + 2];
        const d = (flags & 3) ? { z: Math.min(1, Math.max(0, zf > 1 ? 1 : zf)), s: u32[p + 3] & 255 } : null;
        if (rt && (c || d)) begin(rt, w, h, c, d);
      } else if (op === OP_DRAW && nd < MAX_DRAWS) {
        const h = u32.subarray(p, p + DRAW_HEADER);
        const rt = h[0], tw = h[1], th = h[2], vc = h[9], ic = h[10];
        const left = h[5] | 0, top = h[6] | 0, right = h[7] | 0, bottom = h[8] | 0;
        if (rt && ic && right > left && bottom > top) {
          if (!pass || passKey !== rt || targets.get(rt)?.w !== tw) begin(rt, tw, th, null, null);
          // a texture that is the target being drawn cannot be sampled in the same pass: leave it unbound
          for (let i = 0; i < 4; i++) if ((h[66 + i * 8 + 7] & 2) && h[66 + i * 8] === rt) h[66 + i * 8] = 0;
          verts.set(f32.subarray(p + DRAW_HEADER, p + DRAW_HEADER + vc * VERTEX_FLOATS), vat);
          const base = vat / VERTEX_FLOATS;
          idx.set(u32.subarray(p + DRAW_HEADER + vc * VERTEX_FLOATS, p + DRAW_HEADER + vc * VERTEX_FLOATS + ic), iat);
          // uniforms
          const uo = nd * UNIFORM_STRIDE / 4;
          for (let k = 0; k < 36; k++) uniU[uo + k] = h[30 + k];
          const argb = (v, o) => { uniF[o] = ((v >>> 16) & 255) / 255; uniF[o + 1] = ((v >>> 8) & 255) / 255; uniF[o + 2] = (v & 255) / 255; uniF[o + 3] = ((v >>> 24) & 255) / 255; };
          argb(h[28], uo + 36); argb(h[29], uo + 40);
          uniU[uo + 44] = h[25]; uniU[uo + 45] = h[22]; uniU[uo + 46] = h[23]; uniU[uo + 47] = h[24];
          for (let i = 0; i < 4; i++) {
            const o = 66 + i * 8;
            uniF[uo + 48 + i * 4] = h[o + 1]; uniF[uo + 49 + i * 4] = h[o + 2]; uniF[uo + 50 + i * 4] = h[o + 4]; uniF[uo + 51 + i * 4] = h[o] ? 1 : 0;
          }
          uniF[uo + 64] = tw; uniF[uo + 65] = th; uniF[uo + 66] = f32[p + 4]; uniF[uo + 67] = h[26] ? 1 : 0;
          if (h[26]) stats.programmable++;
          pass.setPipeline(pipeline(h));
          pass.setBindGroup(0, bindGroup(h), [nd * UNIFORM_STRIDE]);
          const bc = h[15];
          pass.setBlendConstant([((bc >>> 16) & 255) / 255, ((bc >>> 8) & 255) / 255, (bc & 255) / 255, ((bc >>> 24) & 255) / 255]);
          pass.setScissorRect(Math.max(0, left), Math.max(0, top), Math.min(tw, right) - Math.max(0, left), Math.min(th, bottom) - Math.max(0, top));
          pass.drawIndexed(ic, 1, iat, base);
          vat += vc * VERTEX_FLOATS; iat += ic; nd++;
          stats.draws++;
        }
      } else if (op === OP_COPY) {
        // CopyRects between targets, outside any pass
        const src = targets.get(u32[p]);
        if (src) {
          pass?.end(); pass = null; passKey = 0;
          const dst = target(u32[p + 3], u32[p + 4], u32[p + 5]);
          const i32 = new Int32Array(u32.buffer);
          for (let r = 0, n = u32[p + 6]; r < n; r++) {
            const q = p + 7 + r * 6;
            let x0 = i32[q], y0 = i32[q + 1], x1 = i32[q + 2], y1 = i32[q + 3], dx = i32[q + 4], dy = i32[q + 5];
            // clip to both textures
            if (x0 < 0) { dx -= x0; x0 = 0; } if (y0 < 0) { dy -= y0; y0 = 0; }
            if (dx < 0) { x0 -= dx; dx = 0; } if (dy < 0) { y0 -= dy; dy = 0; }
            const w = Math.min(x1, src.w) - x0, h = Math.min(y1, src.h) - y0;
            const cw = Math.min(w, dst.w - dx), ch = Math.min(h, dst.h - dy);
            if (cw > 0 && ch > 0) encoder.copyTextureToTexture({ texture: src.color, origin: [x0, y0] }, { texture: dst.color, origin: [dx, dy] }, [cw, ch]);
          }
          stats.copies = (stats.copies || 0) + 1;
        }
      } else if (op === OP_PRESENT) {
        front = u32[p]; fw = u32[p + 1]; fh = u32[p + 2];
      }
      at = p + n;
    }
    pass?.end();
    if (vat) device.queue.writeBuffer(vbuf, 0, verts, 0, vat);
    if (iat) device.queue.writeBuffer(ibuf, 0, idx, 0, iat);
    if (nd) device.queue.writeBuffer(ubuf, 0, uni, 0, nd * UNIFORM_STRIDE);
    if (present && front) {
      const t = targets.get(front);
      if (t) {
        const canvas = context.canvas;
        if (canvas.width !== t.w || canvas.height !== t.h) { canvas.width = t.w; canvas.height = t.h; }
        const bp = encoder.beginRenderPass({ colorAttachments: [{ view: context.getCurrentTexture().createView(), loadOp: 'clear', storeOp: 'store', clearValue: [0, 0, 0, 1] }] });
        bp.setPipeline(blitPipeline);
        bp.setBindGroup(0, device.createBindGroup({ layout: blitPipeline.getBindGroupLayout(0), entries: [{ binding: 0, resource: t.view }, { binding: 1, resource: sampler(0, 0, 1) }] }));
        bp.draw(3);
        bp.end();
        frames++;
        lastFront = front;
      }
    }
    device.queue.submit([encoder.finish()]);
  }

  // once per animation frame: run every frame the game has published since the last call, present the newest
  function frame() {
    if (!M || !device) return 0;
    const i32 = new Int32Array(M.HEAPU32.buffer), u32 = M.HEAPU32;
    const published = Atomics.load(i32, ring) >>> 0;
    let consumed = Atomics.load(i32, ring + 1) >>> 0;
    while (consumed < published) {
      const f = consumed + 1;
      run(u32, u32[ring + 8 + (f & 1)] >>> 2, u32[ring + 2 + (f & 1)], f === published);
      consumed = f;
      Atomics.store(i32, ring + 1, consumed);
    }
    const shown = frames;
    frames = 0;
    return shown;
  }

  // diagnostics: the last presented target as a PNG data URL (testing; no effect on the game)
  async function snapshot() {
    const t = targets.get(lastFront);
    if (!t) return null;
    const bpr = Math.ceil(t.w * 4 / 256) * 256;
    const buf = device.createBuffer({ size: bpr * t.h, usage: GPUBufferUsage.COPY_DST | GPUBufferUsage.MAP_READ });
    const enc = device.createCommandEncoder();
    enc.copyTextureToBuffer({ texture: t.color }, { buffer: buf, bytesPerRow: bpr }, [t.w, t.h]);
    device.queue.submit([enc.finish()]);
    await buf.mapAsync(GPUMapMode.READ);
    const src = new Uint8Array(buf.getMappedRange()), img = new ImageData(t.w, t.h);
    for (let y = 0; y < t.h; y++) img.data.set(src.subarray(y * bpr, y * bpr + t.w * 4), y * t.w * 4);
    buf.unmap(); buf.destroy();
    for (let i = 3; i < img.data.length; i += 4) img.data[i] = 255;
    const c = document.createElement('canvas'); c.width = t.w; c.height = t.h;
    c.getContext('2d').putImageData(img, 0, 0);
    return c.toDataURL('image/jpeg', 0.7);
  }

  return { init, attach, frame, stats, snapshot };
})();
