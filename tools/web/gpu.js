// Browser port, step 4: runs the game's recorded GPU commands with WebGPU on the page's main thread
// (tools/web/webgpu_backend.cpp records them; docs/notes/browser_port_plan.md). Loaded by play.html.
//
// Ring (web_gpu_ring(), u32 words): 0 published frame, 1 consumed frame, 2-3 words in buffer 0/1, 4 overflowed
// frames, 5-7 reserved, 8-9 buffer 0/1 addresses. Commands: op, payload word count, payload.
// Draws take the local build's path: OP_SHADER brings each WGSL module the D3D12 generator wrote (gpu/shaders.cpp),
// OP_IMAGE textures as gpu/texture.cpp decodes them, OP_DRAW2 the raw vertices, the vertex constants, DrawConstants
// and the pipeline state of each draw (webgpu_backend.cpp DrawShared). The older OP_DRAW (RENDERER=reference) brings
// transformed triangles that this side's stand in shader colours. One texture per game render target, one depth
// texture per depth surface; the presented target is shown.
'use strict';
window.MC3Gpu = (() => {
  const OP_CLEAR = 1, OP_PRESENT = 2, OP_TEXTURE = 3, OP_DRAW = 4, OP_COPY = 5, OP_SHADER = 6, OP_IMAGE = 7, OP_DRAW2 = 8;
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

  let device, context, canvasFormat, M = null, ring = 0, features = 0;
  let module, blitModule, layout, pipelineLayout, blitPipeline, white;
  const pipelines = new Map(), samplers = new Map(), textures = new Map(), targets = new Map(), groups = new Map();
  let vbuf = null, ibuf = null, ubuf = null, vcap = 0, icap = 0;
  let frames = 0, lastFront = 0;
  const stats = { draws: 0, textures: 0, programmable: 0, missing: 0, targetTex: 0, bound: 0 };

  async function init(canvas) {
    if (!navigator.gpu) return 'This browser has no WebGPU';
    const adapter = await navigator.gpu.requestAdapter({ powerPreference: 'high-performance' });
    if (!adapter) return 'No WebGPU adapter';
    const wanted = ['texture-compression-bc', 'float32-filterable'].filter(f => adapter.features.has(f));
    device = await adapter.requestDevice({ requiredFeatures: wanted });
    features = (device.features.has('texture-compression-bc') ? 1 : 0);
    if (!(features & 1)) console.warn('webgpu: no texture-compression-bc: compressed textures will be missing');
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
    const ub = { type: 'uniform', hasDynamicOffset: true }, vf = GPUShaderStage.VERTEX | GPUShaderStage.FRAGMENT;
    layout0 = device.createBindGroupLayout({ entries: [{ binding: 0, visibility: vf, buffer: ub }, { binding: 1, visibility: vf, buffer: ub }] });
    return null;
  }

  function attach(module_) { M = module_; ring = M._web_gpu_ring() >>> 2; M.HEAPU32[ring + 5] = features; }

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
      t?.color.destroy();
      t = { w, h,
        color: device.createTexture({ size: [w, h], format: 'rgba8unorm', usage: GPUTextureUsage.RENDER_ATTACHMENT | GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.COPY_SRC | GPUTextureUsage.COPY_DST }) };
      t.view = t.color.createView();
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


  // ---- the local build's draw path (OP_SHADER, OP_IMAGE, OP_DRAW2; webgpu_backend.cpp DrawShared) ----
  // DXGI vertex formats gpu/vertex.cpp hands over (D3DCOLOR's bytes arrive already turned to R G B A)
  const VFORMAT = { 2: 'float32x4', 6: 'float32x3', 13: 'snorm16x4', 14: 'sint16x4', 16: 'float32x2', 28: 'unorm8x4',
    37: 'snorm16x2', 38: 'sint16x2', 41: 'float32', 42: 'uint32', 49: 'unorm8x2', 58: 'snorm16', 59: 'sint16', 61: 'unorm8',
    87: 'unorm8x4' };
  // gpu::Host
  const HOST = ['bgra8unorm', 'bc1-rgba-unorm', 'bc2-rgba-unorm', 'bc3-rgba-unorm', 'r32float', 'r32float'];
  const STENCIL_OP = { 0: 'zero', 0x1E00: 'keep', 0x1E01: 'replace', 0x1E02: 'increment-clamp', 0x1E03: 'decrement-clamp',
    0x150A: 'invert', 0x8507: 'increment-wrap', 0x8508: 'decrement-wrap' };
  const CMP_INDEX = ['never', 'less', 'equal', 'less-equal', 'greater', 'not-equal', 'greater-equal', 'always'];
  // the console tests texel OP r, WebGPU r OP texel: the same test with the sides swapped
  const CMP_SWAPPED = ['never', 'greater', 'equal', 'greater-equal', 'less', 'not-equal', 'less-equal', 'always'];
  const DRAW2_HEADER = 96;
  const shaders = new Map(), images = new Map(), layouts1 = new Map(), pipelines2 = new Map(), groups1 = new Map();
  const depths = new Map(), dummies = {};
  let layout0 = null, group0 = null, group0Buf = null, ucap = 0, ugpu = null, acap = 0, agpu = null;
  const decoder = new TextDecoder();
  let dcBytes = 0;

  const idOf = (lo, hi) => `${hi.toString(16)}:${lo.toString(16)}`;

  function shaderModule(id, stage, text) {
    const module = device.createShaderModule({ code: text, label: id });
    module.getCompilationInfo().then(info => {
      for (const m of info.messages) if (m.type === 'error') console.warn(`webgpu: shader ${id}: ${m.lineNum}:${m.linePos} ${m.message}`);
    });
    shaders.set(id, { module, stage });
    stats.shaders = (stats.shaders || 0) + 1;
  }

  function image(u32, p) {
    const id = idOf(u32[p], u32[p + 1]);
    let w = u32[p + 2], h = u32[p + 3], depth = u32[p + 4];
    const levels = u32[p + 5], kind = u32[p + 6], format = HOST[u32[p + 7]] || 'bgra8unorm';
    const bc = format.startsWith('bc');
    const W = bc ? Math.ceil(w / 4) * 4 : w, H = bc ? Math.ceil(h / 4) * 4 : h;
    const layers = kind === 3 ? 6 : kind === 2 ? depth : 1;
    let t = images.get(id);
    if (!t || t.w !== W || t.h !== H || t.layers !== layers || t.levels !== levels || t.format !== format || t.kind !== kind) {
      t?.tex.destroy();
      t = { w: W, h: H, layers, levels, format, kind,
        tex: device.createTexture({ size: [W, H, layers], format, mipLevelCount: levels, dimension: kind === 2 ? '3d' : '2d',
          usage: GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.COPY_DST }) };
      t.view = t.tex.createView({ dimension: kind === 3 ? 'cube' : kind === 2 ? '3d' : '2d' });
      t.gen = (images.get(id)?.gen || 0) + 1;
      images.set(id, t);
    }
    let at = p + 8;
    const faces = kind === 3 ? 6 : 1;
    for (let f = 0; f < faces; f++) {
      for (let l = 0; l < levels; l++) {
        const pitch = u32[at], bytes = u32[at + 1];
        const lw = Math.max(1, W >> l), lh = Math.max(1, H >> l), ld = kind === 2 ? Math.max(1, depth >> l) : 1;
        const cw = bc ? Math.ceil(lw / 4) * 4 : lw, ch = bc ? Math.ceil(lh / 4) * 4 : lh;
        const rows = bc ? ch / 4 : ch;
        if (bytes && pitch && bytes >= pitch * rows * ld)
          device.queue.writeTexture({ texture: t.tex, mipLevel: l, origin: [0, 0, kind === 3 ? f : 0] },
            new Uint8Array(u32.buffer, (at + 2) * 4, bytes).slice(), { bytesPerRow: pitch, rowsPerImage: rows }, [cw, ch, ld]);
        at += 2 + Math.ceil(bytes / 4);
      }
    }
    stats.textures++;
  }

  // what PixelWgsl declares for a stage: (kind, shadow) -> texture and sampler binding layouts
  function stageKind(flags) {
    const bound = flags & 1, kind = bound ? (flags >>> 4) & 15 : 1, shadow = bound ? (flags >>> 8) & 3 : 0;
    return shadow ? 'depth' : kind === 2 ? '3d' : kind === 3 ? 'cube' : '2d';
  }

  function layout1(kinds) {
    const key = kinds.join(',');
    let l = layouts1.get(key);
    if (!l) {
      const entries = [];
      kinds.forEach((k, i) => {
        entries.push({ binding: i * 2, visibility: GPUShaderStage.FRAGMENT, texture: k === 'depth'
          ? { sampleType: 'depth', viewDimension: '2d' } : { sampleType: 'float', viewDimension: k } });
        entries.push({ binding: i * 2 + 1, visibility: GPUShaderStage.FRAGMENT, sampler: { type: k === 'depth' ? 'comparison' : 'filtering' } });
      });
      entries.push({ binding: 8, visibility: GPUShaderStage.FRAGMENT, texture: {} });
      entries.push({ binding: 9, visibility: GPUShaderStage.FRAGMENT, texture: {} });
      l = device.createBindGroupLayout({ entries });
      layouts1.set(key, l);
    }
    return l;
  }

  function dummy(kind) {
    if (dummies[kind]) return dummies[kind];
    let tex;
    if (kind === 'depth') tex = device.createTexture({ size: [1, 1], format: 'depth32float', usage: GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.RENDER_ATTACHMENT });
    else {
      tex = device.createTexture({ size: [1, 1, kind === 'cube' ? 6 : 1], dimension: kind === '3d' ? '3d' : '2d', format: 'rgba8unorm',
        usage: GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.COPY_DST });
      for (let f = 0; f < (kind === 'cube' ? 6 : 1); f++)
        device.queue.writeTexture({ texture: tex, origin: [0, 0, f] }, new Uint8Array([255, 255, 255, 255]), { bytesPerRow: 4 }, [1, 1, 1]);
    }
    dummies[kind] = tex.createView({ dimension: kind === 'depth' ? '2d' : kind });
    return dummies[kind];
  }

  function sampler2(word, shadow, levels) {
    const key = `${word}|${shadow}|${levels > 1}`;
    let s = samplers.get(key);
    if (!s) {
      const addr = a => a === 1 ? 'repeat' : a === 2 ? 'mirror-repeat' : 'clamp-to-edge';
      const min = (word >>> 9) & 3, mag = (word >>> 11) & 3, mip = (word >>> 13) & 3, aniso = (word >>> 15) & 31, maxmip = (word >>> 20) & 15;
      const d = { addressModeU: addr(word & 7), addressModeV: addr((word >>> 3) & 7), addressModeW: addr((word >>> 6) & 7),
        minFilter: min >= 2 ? 'linear' : 'nearest', magFilter: mag >= 2 ? 'linear' : 'nearest', mipmapFilter: mip === 2 ? 'linear' : 'nearest',
        lodMinClamp: maxmip, lodMaxClamp: mip ? 32 : maxmip };
      if (d.lodMaxClamp < d.lodMinClamp) d.lodMaxClamp = d.lodMinClamp;
      if (aniso > 1 && d.minFilter === 'linear' && d.magFilter === 'linear' && d.mipmapFilter === 'linear') d.maxAnisotropy = aniso;
      if (shadow) { d.compare = CMP_SWAPPED[(word >>> 24) & 7]; d.maxAnisotropy = 1; }
      s = device.createSampler(d);
      samplers.set(key, s);
    }
    return s;
  }

  function depthTarget(key, w, h) {
    const k = `${key}:${w}x${h}`;
    let d = depths.get(k);
    if (!d) {
      d = { tex: device.createTexture({ size: [w, h], format: 'depth24plus-stencil8', usage: GPUTextureUsage.RENDER_ATTACHMENT | GPUTextureUsage.TEXTURE_BINDING }) };
      d.view = d.tex.createView();
      d.depthOnly = d.tex.createView({ aspect: 'depth-only' });
      depths.set(k, d);
      depths.set(`latest:${key}`, d);
    }
    return d;
  }

  function pipeline2(h, vs, ps, kinds) {
    const fields = [h[0], h[1], h[2], h[3], h[4] ? 1 : 0, h[7] ? 1 : 0, ...h.subarray(10, 20), h[19] ? h.subarray(20, 26).join('.') : '',
      h[26], h[27], h[28], h[35], h[36], (h[10] >>> 8) & 1 ? h[38] : '', ...h.subarray(41, 73)];
    const key = fields.join(',') + '|' + kinds.join(',');
    let p = pipelines2.get(key);
    if (p) return p;
    const regs = h[35], nb = h[36], topo = h[10] & 0xFF, strip = (h[10] >>> 8) & 1;
    const buffers = [];
    for (let b = 0; b < nb; b++) buffers.push({ arrayStride: h[57 + b], attributes: [] });
    for (let r = 0; r < 16; r++) {
      if (!((regs >>> r) & 1)) continue;
      const e = h[41 + r], fmt = VFORMAT[e & 0xFF], b = (e >>> 8) & 0xFF;
      if (!fmt || !buffers[b]) { console.warn(`webgpu: vertex format ${e & 0xFF} for v${r}`); continue; }
      buffers[b].attributes.push({ shaderLocation: r, offset: e >>> 16, format: fmt });
    }
    const blend = h[11], src = h[12], dst = h[13], bop = h[14];
    let blendState;
    if (blend) {
      const operation = BLEND_OP[bop] || 'add', minmax = operation === 'min' || operation === 'max';
      const c = { operation, srcFactor: minmax ? 'one' : (FACTOR[src] || 'one'), dstFactor: minmax ? 'one' : (FACTOR[dst] || 'zero') };
      blendState = { color: c, alpha: c };
    }
    const primitive = { topology: topo === 0 ? 'point-list' : topo === 1 ? (strip ? 'line-strip' : 'line-list') : (strip ? 'triangle-strip' : 'triangle-list') };
    if (strip && h[37]) primitive.stripIndexFormat = h[38] ? 'uint16' : 'uint32';
    if (h[26]) { primitive.cullMode = 'back'; primitive.frontFace = h[27] ? 'ccw' : 'cw'; }
    const zen = h[16], sten = h[19];
    const face = { compare: sten ? CMP_INDEX[(h[20] - 0x200) & 7] : 'always', failOp: STENCIL_OP[h[23]] || 'keep',
      depthFailOp: STENCIL_OP[h[24]] || 'keep', passOp: STENCIL_OP[h[25]] || 'keep' };
    const desc = {
      layout: device.createPipelineLayout({ bindGroupLayouts: [layout0, layout1(kinds)] }),
      vertex: { module: vs, entryPoint: 'main', buffers },
      fragment: { module: ps, entryPoint: 'main', targets: [{ format: 'rgba8unorm', blend: blendState, writeMask: h[15] }] },
      primitive,
      depthStencil: { format: 'depth24plus-stencil8', depthWriteEnabled: !!(zen && h[17]),
        depthCompare: zen ? (CMP_INDEX[(h[18] - 0x200) & 7] || 'always') : 'always',
        stencilFront: face, stencilBack: face, stencilReadMask: sten ? h[21] : 0xFF, stencilWriteMask: sten ? h[22] : 0 },
    };
    try { p = device.createRenderPipeline(desc); } catch (e) { console.warn('webgpu: pipeline', e.message); p = null; }
    pipelines2.set(key, p);
    return p;
  }

  // one frame's commands: uploads first (queued before the submit), then the passes
  function run(u32, start, count, present) {
    const f32 = new Float32Array(u32.buffer);
    const draws = [], clears = [];
    let vfloats = 0, iwords = 0, ubytes = 0, abytes = 0;
    // pass 1: textures, and the sizes of the vertex and index data
    for (let at = start, end = start + count; at + 2 <= end;) {
      const op = u32[at], n = u32[at + 1], p = at + 2;
      if (op === OP_TEXTURE) {
        const w = u32[p + 1], h = u32[p + 2];
        texture(u32[p], w, h, new Uint8Array(u32.buffer, (p + 3) * 4, w * h * 4).slice());
      } else if (op === OP_DRAW) {
        vfloats += u32[p + 9] * VERTEX_FLOATS; iwords += u32[p + 10];
      } else if (op === OP_SHADER) {
        shaderModule(idOf(u32[p], u32[p + 1]), u32[p + 2], decoder.decode(new Uint8Array(u32.buffer, (p + 4) * 4, u32[p + 3]).slice()));
      } else if (op === OP_IMAGE) {
        image(u32, p);
      } else if (op === OP_DRAW2) {
        const fl = u32[p + 89], dcw = u32[p + 92];
        dcBytes = dcw * 4;
        if (fl & 1) ubytes += 3072;
        if (fl & 2) ubytes += Math.ceil(dcBytes / 256) * 256;
        let q = p + DRAW2_HEADER + (fl & 1 ? 768 : 0) + (fl & 2 ? dcw : 0);
        const lens = [];
        for (let b = 0, nb = u32[p + 36]; b <= nb; b++) { lens.push(u32[q]); abytes += Math.ceil(u32[q] / 4) * 4 + 4; q += 1 + Math.ceil(u32[q] / 4); }
        stats.lastDraw2 = { n, fl, dcw, nb: u32[p + 36], lens, abytes, ubytes };
        if (q !== p + n) stats.badDraw2 = { n, used: q - p, fl, dcw, nb: u32[p + 36], lens };
      }
      at = p + n;
    }
    // the frame's uniform blocks (256 aligned) and vertex and index bytes, staged here and written before the submit
    if (!ugpu || ubytes > ucap) {
      ugpu?.destroy(); ucap = Math.max(ubytes, 1 << 20) * 2;
      ugpu = device.createBuffer({ size: ucap, usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST });
      group0 = null;
    }
    if (!agpu || abytes > acap) {
      agpu?.destroy(); acap = Math.max(abytes, 4 << 20) * 2;
      agpu = device.createBuffer({ size: acap, usage: GPUBufferUsage.VERTEX | GPUBufferUsage.INDEX | GPUBufferUsage.COPY_DST });
    }
    if (dcBytes && (!group0 || group0Buf !== ugpu)) {
      group0 = device.createBindGroup({ layout: layout0, entries: [{ binding: 0, resource: { buffer: ugpu, size: 3072 } },
        { binding: 1, resource: { buffer: ugpu, size: dcBytes } }] });
      group0Buf = ugpu;
    }
    const ustage = new Uint8Array(Math.max(ubytes, 4)), astage = new Uint8Array(Math.max(abytes, 4));
    let uat = 0, aat = 0, vcOff = 0, dcOff = 0;
    const verts = new Float32Array(vfloats), idx = new Uint32Array(iwords), uni = new ArrayBuffer(UNIFORM_STRIDE * MAX_DRAWS);
    const uniU = new Uint32Array(uni), uniF = new Float32Array(uni);
    let vat = 0, iat = 0, nd = 0;
    const encoder = device.createCommandEncoder();
    let pass = null, passKey = 0, passZs = 0;
    const begin = (key, w, h, clearColor, clearDepth, zs = 0) => {
      pass?.end();
      const t = target(key || `depthonly:${zs}`, w, h);
      const d = depthTarget(zs || `own:${key}`, t.w, t.h);
      pass = encoder.beginRenderPass({
        colorAttachments: [{ view: t.view, loadOp: clearColor ? 'clear' : 'load', storeOp: 'store', clearValue: clearColor || [0, 0, 0, 0] }],
        depthStencilAttachment: { view: d.view, depthLoadOp: clearDepth ? 'clear' : 'load', depthStoreOp: 'store',
          depthClearValue: clearDepth ? clearDepth.z : 1, stencilLoadOp: clearDepth ? 'clear' : 'load', stencilStoreOp: 'store',
          stencilClearValue: clearDepth ? clearDepth.s : 0 },
      });
      pass.setVertexBuffer(0, vbuf);
      pass.setIndexBuffer(ibuf, 'uint32');
      passKey = key;
      passZs = zs;
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
        const zs = u32[p + 5];
        if ((rt || zs) && (c || d)) begin(rt, w, h, c, d, d ? zs : (passKey === rt ? passZs : 0));
      } else if (op === OP_DRAW && nd < MAX_DRAWS) {
        const h = u32.subarray(p, p + DRAW_HEADER);
        const rt = h[0], tw = h[1], th = h[2], vc = h[9], ic = h[10];
        const left = h[5] | 0, top = h[6] | 0, right = h[7] | 0, bottom = h[8] | 0;
        if (rt && ic && right > left && bottom > top) {
          if (!pass || passKey !== rt || passZs !== h[3] || targets.get(rt)?.w !== tw) begin(rt, tw, th, null, null, h[3]);
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
      } else if (op === OP_DRAW2) {
        const h = u32.subarray(p, p + DRAW2_HEADER);
        const fl = h[89], dcw = h[92];
        let q = p + DRAW2_HEADER;
        if (fl & 1) { vcOff = uat; ustage.set(new Uint8Array(u32.buffer, q * 4, 3072), uat); uat += 3072; q += 768; }
        if (fl & 2) { dcOff = uat; ustage.set(new Uint8Array(u32.buffer, q * 4, dcw * 4), uat); uat += Math.ceil(dcw * 4 / 256) * 256; q += dcw; }
        const nb = h[36], vb = [];
        for (let b = 0; b <= nb; b++) {
          const bytes = u32[q];
          vb.push({ off: aat, bytes });
          astage.set(new Uint8Array(u32.buffer, (q + 1) * 4, bytes), aat);
          aat += Math.ceil(bytes / 4) * 4 + 4;
          q += 1 + Math.ceil(bytes / 4);
        }
        const vs = shaders.get(idOf(h[0], h[1])), ps = shaders.get(idOf(h[2], h[3]));
        const rt = h[4], zs = h[7], tw = rt ? h[5] : h[8], th = rt ? h[6] : h[9];
        const x0 = h[31], y0 = h[32], x1 = Math.min(h[33], tw), y1 = Math.min(h[34], th);
        if (!vs || !ps) { stats.missing++; at = p + n; continue; }
        if (x1 <= x0 || y1 <= y0) { at = p + n; continue; }
        if (!pass || passKey !== rt || passZs !== zs || (rt && targets.get(rt)?.w !== tw)) begin(rt, tw, th, null, null, zs);
        const kinds = [], views = [], samps = [];
        let gkey = '';
        for (let i = 0; i < 4; i++) {
          const f = h[75 + i * 3], kind = stageKind(f);
          kinds.push(kind);
          let view = null, levels = 1;
          if (f & 1) {
            if (f & 2) {
              const key = h[73 + i * 3];
              if (key !== rt && key !== zs) {
                if (kind === 'depth') view = depths.get(`latest:${key}`)?.depthOnly;
                else view = targets.get(key)?.view;
              }
              gkey += `t${key}:${targets.get(key)?.w},`;
            } else {
              const t = images.get(idOf(h[73 + i * 3], h[74 + i * 3]));
              if (t) { view = t.view; levels = t.levels; gkey += `${h[73 + i * 3]}.${h[74 + i * 3]}:${t.gen},`; }
            }
            if (!view) stats.missing++;
          }
          if (!view) { view = dummy(kind); gkey += `-${kind},`; }
          views.push(view);
          samps.push(sampler2(h[85 + i], kind === 'depth', levels));
          gkey += `${h[85 + i]}|${levels > 1};`;
        }
        const pl = pipeline2(h, vs.module, ps.module, kinds);
        if (!pl) { at = p + n; continue; }
        let g1 = groups1.get(gkey + kinds.join());
        if (!g1) {
          const entries = [];
          for (let i = 0; i < 4; i++) { entries.push({ binding: i * 2, resource: views[i] }); entries.push({ binding: i * 2 + 1, resource: samps[i] }); }
          entries.push({ binding: 8, resource: dummy('2d') }, { binding: 9, resource: dummy('2d') });
          g1 = device.createBindGroup({ layout: layout1(kinds), entries });
          if (groups1.size > 8192) groups1.clear();
          groups1.set(gkey + kinds.join(), g1);
        }
        pass.setPipeline(pl);
        pass.setBindGroup(0, group0, [vcOff, dcOff]);
        pass.setBindGroup(1, g1);
        for (let b = 0; b < nb; b++) if (vb[b].bytes) pass.setVertexBuffer(b, agpu, vb[b].off, vb[b].bytes);
        const bc = h[29];
        pass.setBlendConstant([((bc >>> 16) & 255) / 255, ((bc >>> 8) & 255) / 255, (bc & 255) / 255, ((bc >>> 24) & 255) / 255]);
        pass.setStencilReference(h[30]);
        pass.setScissorRect(x0, y0, x1 - x0, y1 - y0);
        if (h[37]) {
          const ix = vb[nb];
          pass.setIndexBuffer(agpu, h[38] ? 'uint16' : 'uint32', ix.off, ix.bytes);
          pass.drawIndexed(h[37], 1, 0, h[40] | 0);
        } else if (h[39]) {
          pass.draw(h[39], 1, 0, 0);
        }
        stats.draws++;
        if (!(h[75] & 1) || h[0]) stats.draws2 = (stats.draws2 || 0) + 1;
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
    if (uat) device.queue.writeBuffer(ugpu, 0, ustage, 0, uat);
    if (aat) device.queue.writeBuffer(agpu, 0, astage, 0, aat);
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
