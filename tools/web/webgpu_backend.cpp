// Browser port, step 4: the game's draws on the player's GPU through WebGPU (docs/notes/browser_port_plan.md).
//
// The game's threads cannot own WebGPU: getting an adapter and device are promises, and those threads block. So, as
// nfsu2-web does (runtime/web/gpu_proto.h), the backend records commands into shared memory and the page's main thread
// runs them with WebGPU (tools/web/gpu.js) once per animation frame:
//
//   game threads --(gpu::Backend calls)--> GpuRecorder --(frame command buffer, shared memory)--> gpu.js --> WebGPU
//
// Draws take the local build's path (docs/notes/browser_renderer_parity.md): the input assembler's raw vertices
// (gpu/vertex.cpp), the shader choice, pipeline state and DrawConstants from gpu/draw_setup.cpp, and the WGSL the
// D3D12 backend's generator writes (gpu/shaders.cpp), so vertex programs, combiners and fixed function run on the GPU
// as the local build has them. Textures go up as gpu/texture.cpp decodes them (BC blocks stay compressed).
// RENDERER=reference keeps the older path, the CPU reference renderer's vertex stage and a WGSL stand in for the
// stages (harness/reference_renderer.cpp), which stays as the comparison oracle (DUAL=1).
//
// Two command buffers alternate by frame. Present publishes the frame's buffer and waits while the page is still
// running the one before. Commands are u32 words: op, payload word count, payload:
//   1 clear:   flags, color (ARGB), z (float bits), stencil, rt, zs, rt width, rt height, rect count, rects (4 x i32)
//   2 present: front buffer (guest address), width, height
//   3 texture: key, width, height, then width * height RGBA8 texels
//   4 draw:    kDrawHeader words (see Draw), vertices (reference::kVertexFloats floats each), u32 indices
//   5 copy:    src, src width, src height, dst, dst width, dst height, rect count, rects (x0 y0 x1 y1 dx dy, i32)
//   6 shader:  id (2 words), stage (0 vertex, 1 pixel), byte length, WGSL text (padded to words)
//   7 image:   id (2 words), width, height, depth, levels, kind (1 2D, 2 3D, 3 cube), host format (gpu::Host), then per
//              face and level: row pitch, byte length, bytes (padded to words)
//   8 draw2:   kDraw2Header words (see DrawShared), vertex constants (if flagged), DrawConstants (if flagged), per
//              vertex buffer: byte length, bytes; then the indices (byte length, bytes)
// The recorder wraps the CPU stand in (harness/null_backend.cpp) for copies and the console-memory effects.
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <array>
#include <map>
#include <set>
#include <string>
#include <memory>
#include <thread>
#include <vector>

#include <dxgiformat.h>

#include "core/log.h"
#include "gpu/backend.h"
#include "gpu/draw_setup.h"
#include "gpu/fast_hash.h"
#include "gpu/shaders.h"
#include "gpu/texture.h"
#include "gpu/vertex.h"
#include "kernel/kernel.h"
#include "reference_renderer.h"

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#else
#define EMSCRIPTEN_KEEPALIVE
#endif

namespace {

enum Op : uint32_t { kOpClear = 1, kOpPresent = 2, kOpTexture = 3, kOpDraw = 4, kOpCopy = 5, kOpShader = 6, kOpImage = 7, kOpDraw2 = 8, kOpDepthCopy = 9 };
constexpr uint32_t kDraw2Header = 96;
constexpr uint32_t kDrawHeader = 98;
constexpr uint32_t kBufferWords = 8u << 20;  // 32 MiB per frame buffer

// Shared with gpu.js; the layout is part of the protocol (web_gpu_ring returns its address)
struct Ring {
    std::atomic<uint32_t> published{0};  // last frame whose buffer is complete
    std::atomic<uint32_t> consumed{0};   // last frame the page has run
    uint32_t words[2] = {0, 0};          // length of each buffer, in words
    uint32_t overflow = 0;               // frames cut short for exceeding a buffer
    uint32_t features = 0;  // the page's: bit 0 texture-compression-bc
    uint32_t reserved[2] = {};
    uint32_t* buffers[2] = {nullptr, nullptr};
};
Ring g_ring;
std::vector<uint32_t> g_store[2];

uint32_t Word(float f) {
    uint32_t u;
    std::memcpy(&u, &f, 4);
    return u;
}

uint64_t TargetKey(uint32_t key, bool depth, uint32_t w, uint32_t h) {
    return uint64_t(w & 0x7FFF) << 48 | uint64_t(h & 0x7FFF) << 33 | uint64_t(depth ? 1 : 0) << 32 | key;
}

// MemSig hashes a surface up to 64 KiB whole, a bigger one as 64 evenly spaced 64-byte samples (d3d12.cpp)
uint64_t MemSig(uint32_t data, uint32_t total) {
    constexpr uint32_t kWholeMax = 65536, kSamples = 64, kSampleBytes = 64;
    const uint8_t* p = static_cast<const uint8_t*>(kernel::HostPtr(data));
    uint64_t sig = gpu::Hash(&total, sizeof(total));
    if (total <= kWholeMax) return gpu::Hash(p, total, sig);
    for (uint32_t i = 0; i < kSamples; i++) {
        const uint8_t* s = p + uint64_t(total - kSampleBytes) * i / (kSamples - 1);
        sig = gpu::Hash(s, kSampleBytes, sig);
    }
    return sig;
}

bool SpriteOnStage3(const gpu::State& s, uint32_t prim) {
    bool prog = (s.vs_handle & 1) && s.vs && !s.vs->program.empty();
    return prim == 1 && prog && !s.ps && s.rs[gpu::kRsPointSpriteEnable] && s.tss[3][gpu::kTssColorOp] != 1;
}

gpu::State SpriteStageZero(const gpu::State& s) {
    gpu::State moved = s;
    moved.tss[0] = s.tss[3];
    moved.textures[0] = s.textures[3];
    moved.palettes[0] = s.palettes[3];
    for (int i = 1; i < 4; i++) {
        moved.tss[i][gpu::kTssColorOp] = 1;
        moved.tss[i][gpu::kTssAlphaOp] = 1;
        moved.textures[i] = 0;
    }
    return moved;
}

}  // namespace

extern "C" EMSCRIPTEN_KEEPALIVE Ring* web_gpu_ring() { return &g_ring; }

namespace gpu {

namespace {

class GpuRecorder;
GpuRecorder* g_recorder = nullptr;

class GpuRecorder final : public Backend {
public:
    explicit GpuRecorder(std::unique_ptr<Backend> inner) : inner_(std::move(inner)) {
        for (int i = 0; i < 2; i++) {
            g_store[i].assign(kBufferWords, 0);
            g_ring.buffers[i] = g_store[i].data();
        }
        g_recorder = this;
        reference::SetGpuSink(&Sink);
        // DUAL=1: the CPU renderer draws every frame too (REF=1 shows it), LIMIT=n: first n draws only
        dual_ = std::getenv("DUAL") && std::getenv("DUAL")[0] == '1';
        const char* limit = std::getenv("LIMIT");
        reference::SetGpuCompare(dual_, limit ? uint32_t(std::atoi(limit)) : 0);
        if (dual_ || limit) LOGI("webgpu: compare mode, dual %d, limit %s", int(dual_), limit ? limit : "none");
        const char* r = std::getenv("RENDERER");
        reference_ = r && !std::strcmp(r, "reference");
        LOGI("webgpu: %s", reference_ ? "the reference renderer's path" : "the local build's draw path");
    }

    bool Init(uint32_t w, uint32_t h) override {
        width_ = w, height_ = h;
        LOGI("webgpu: recording for the page, back buffer %ux%u", w, h);
        return inner_->Init(w, h);
    }
    void Clear(const State& s, uint32_t n, const uint8_t* rects, uint32_t flags, uint32_t color, float z,
               uint32_t stencil) override {
        bool has_rt = (flags & 0xF0) && s.rt.header && s.rt.data;
        bool has_ds = (flags & 3) && s.zs.header && s.zs.data;
        uint32_t rw = width_, rh = height_, zw = width_, zh = height_;
        if (has_rt) SurfaceSize(s.rt, rw, rh);
        if (has_ds) SurfaceSize(s.zs, zw, zh);
        uint32_t rt_key = has_rt ? reference::GpuKey(s.rt.data) : 0;
        uint32_t zs_key = has_ds ? reference::GpuKey(s.zs.data) : 0;
        auto emit_clear = [&](uint32_t fl, uint32_t rk, uint32_t zk, uint32_t gw, uint32_t gh) {
            uint32_t count = (n && rects) ? n : 1;
            std::vector<uint32_t> p{fl, color, Word(z), stencil, rk, zk, gw, gh, count};
            if (n && rects) {
                for (uint32_t i = 0; i < n * 4; i++) {
                    uint32_t v;
                    std::memcpy(&v, rects + i * 4, 4);
                    p.push_back(v);
                }
            } else {
                const auto& vp = s.viewport;
                for (int32_t v : {int32_t(vp.x), int32_t(vp.y), int32_t(vp.x + vp.w), int32_t(vp.y + vp.h)})
                    p.push_back(uint32_t(v));
            }
            Put(kOpClear, {{p.data(), uint32_t(p.size())}});
        };
        if (has_rt && has_ds && (rw != zw || rh != zh)) {
            emit_clear(flags & 0xF0, rt_key, 0, rw, rh);
            emit_clear(flags & 3, 0, zs_key, zw, zh);
        } else if (has_rt || has_ds) {
            emit_clear(flags, rt_key, zs_key, has_rt ? rw : zw, has_rt ? rh : zh);
        }
        if (has_rt) {
            targets_.insert(TargetKey(rt_key, false, rw, rh));
            color_targets_.insert(rt_key);
        }
        if (has_ds) {
            targets_.insert(TargetKey(zs_key, true, zw, zh));
        }
        if (dual_) reference::Clear(s, n, rects, flags, color, z, stencil);
    }
    void Draw(const State& s, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices) override {
        if (reference_) reference::Draw(s, prim, start, count, indices);  // its vertex stage, then Sink
        else DrawShared(s, prim, start, count, indices);
    }
    // CopyRects between surfaces: on the GPU (the screen effects copy the back buffer into a texture they then draw
    // with); the destination becomes a GPU target, so later draws sample it from there
    void Copy(const Surface& src, const Surface& dst, uint32_t n, uint32_t rects, uint32_t points) override {
        uint32_t sw = 0, sh = 0, dw = 0, dh = 0;
        uint32_t src_key = reference::GpuKey(src.data), dst_key = reference::GpuKey(dst.data);
        bool gpu_src = reference::SurfaceSize(src, sw, sh) && reference::SurfaceSize(dst, dw, dh) &&
                       (targets_.count(TargetKey(src_key, false, sw, sh)) || color_targets_.count(src_key));
        if (gpu_src) {
            uint32_t count = n && rects ? n : 1;
            std::vector<uint32_t> p{src_key, sw, sh, dst_key, dw, dh, count};
            for (uint32_t i = 0; i < count; i++) {
                int32_t x0 = 0, y0 = 0, x1 = int32_t(sw), y1 = int32_t(sh), dx = 0, dy = 0;
                if (n && rects) {
                    x0 = int32_t(kernel::R32(rects + i * 16)), y0 = int32_t(kernel::R32(rects + i * 16 + 4));
                    x1 = int32_t(kernel::R32(rects + i * 16 + 8)), y1 = int32_t(kernel::R32(rects + i * 16 + 12));
                    dx = x0, dy = y0;
                }
                if (points) dx = int32_t(kernel::R32(points + i * 8)), dy = int32_t(kernel::R32(points + i * 8 + 4));
                for (int32_t v : {x0, y0, x1, y1, dx, dy}) p.push_back(uint32_t(v));
            }
            Put(kOpCopy, {{p.data(), uint32_t(p.size())}});
            reference::MarkGpuTarget(dst.data, dw);
            targets_.insert(TargetKey(dst_key, false, dw, dh));
            color_targets_.insert(dst_key);
        }
        if (dual_ || !gpu_src) inner_->Copy(src, dst, n, rects, points);
    }
    void Present(const Surface& front) override {
        uint32_t p[3] = {reference::GpuKey(front.data), width_, height_};
        Put(kOpPresent, {{p, 3}});
        inner_->Present(front);
        uint32_t frame = frame_ + 1;
        g_ring.words[frame & 1] = used_;
        g_ring.published.store(frame, std::memory_order_release);
        while (g_ring.consumed.load(std::memory_order_acquire) + 1 < frame)
            std::this_thread::sleep_for(std::chrono::microseconds(500));
        hashed_.clear();
        last_vc_valid_ = last_dc_valid_ = false;  // each frame's first draw sends both
        if (overflowed_) {
            uploaded_.clear();  // what this frame meant to upload may not have reached the page
            images_.clear();
            shaders_.clear();
            g_ring.overflow++;
        }
        frame_ = frame;
        used_ = 0;
        overflowed_ = false;
    }
    void Sync(uint32_t va, uint32_t size) override { inner_->Sync(va, size); }
    void SyncLate(uint32_t va) override { inner_->SyncLate(va); }
    void Flush() override { inner_->Flush(); }
    void SetGamma(const uint8_t* ramp) override { inner_->SetGamma(ramp); }

private:
    struct Span {
        const void* data;
        uint32_t words;
    };

    // append one command made of spans to the buffer of the frame being recorded (frame_ + 1)
    bool Put(uint32_t op, std::initializer_list<Span> spans) {
        uint32_t words = 0;
        for (const Span& s : spans) words += s.words;
        if (overflowed_ || used_ + 2 + words > kBufferWords) {
            overflowed_ = true;
            return false;
        }
        uint32_t* buf = g_ring.buffers[(frame_ + 1) & 1];
        buf[used_++] = op;
        buf[used_++] = words;
        for (const Span& s : spans) {
            std::memcpy(buf + used_, s.data, size_t(s.words) * 4);
            used_ += s.words;
        }
        return true;
    }

    static void Sink(const reference::GpuDraw& d) { g_recorder->RecordDraw(d); }

    void RecordDraw(const reference::GpuDraw& d) {
        if (!d.rt || !d.index_count) return;
        for (const reference::GpuTexture& t : d.tex) {
            if (!t.key || !t.rgba || t.target) continue;
            auto it = uploaded_.find(t.key);
            if (it != uploaded_.end() && it->second == t.hash) continue;
            uint32_t head[3] = {t.key, t.w, t.h};
            if (Put(kOpTexture, {{head, 3}, {t.rgba, t.w * t.h}})) uploaded_[t.key] = t.hash;
        }
        uint32_t h[kDrawHeader] = {d.rt, d.rt_w, d.rt_h, d.zs, Word(d.zscale), uint32_t(d.left), uint32_t(d.top),
                                   uint32_t(d.right), uint32_t(d.bottom), d.vertex_count, d.index_count, d.blend, d.src,
                                   d.dst, d.op, d.blend_color, d.color_mask, d.ztest, d.zwrite, d.zfunc, d.cull, d.front,
                                   d.atest, d.afunc, d.aref, d.alphakill, d.programmable, d.flat, d.tfactor, d.fog_color};
        std::memcpy(&h[30], d.ff, sizeof d.ff);  // 36 words
        for (int i = 0; i < 4; i++) {
            const reference::GpuTexture& t = d.tex[i];
            uint32_t* o = &h[66 + i * 8];
            o[0] = t.rgba || t.target ? t.key : 0, o[1] = t.w, o[2] = t.h, o[3] = t.kind, o[4] = t.rect, o[5] = t.address_u,
            o[6] = t.address_v, o[7] = t.linear | t.target << 1;  // bit 1: a GPU render target, bound by address
        }
        Put(kOpDraw, {{h, kDrawHeader}, {d.verts, d.vertex_count * reference::kVertexFloats}, {d.indices, d.index_count}});
    }

    // ---- the local build's draw path ----

    static void PutWords(std::vector<uint32_t>& v, const void* data, size_t bytes) {
        size_t at = v.size();
        v.resize(at + 1 + (bytes + 3) / 4, 0);
        v[at] = uint32_t(bytes);
        if (bytes) std::memcpy(&v[at + 1], data, bytes);
    }

    void SendShader(uint64_t id, uint32_t stage, const std::string& text) {
        if (shaders_.count(id)) return;
        std::vector<uint32_t> p{uint32_t(id), uint32_t(id >> 32), stage};
        PutWords(p, text.data(), text.size());
        if (Put(kOpShader, {{p.data(), uint32_t(p.size())}})) shaders_.insert(id);
    }

    // a stage's texture: a render target the page has, or guest memory decoded as gpu/texture.cpp does for D3D12;
    // fills the stage's TexInfo and its header words (id low, id high or 0, 1 for a target), false for none
    bool StageTexture(const State& s, int i, TexInfo& info, uint32_t* h, TextureDesc& d) {
        if (!s.textures[size_t(i)] || !Describe(s.textures[size_t(i)], d)) return false;
        const FormatInfo& fi = Format(d.format);
        info.kind = d.cube ? kTexCube : d.depth > 1 ? kTex3D : kTex2D;
        info.rect = d.linear;
        if (fi.depth) info.shadow = fi.bits == 16 ? 2 : 1;
        uint32_t key = reference::GpuKey(d.data);
        if (targets_.count(TargetKey(key, fi.depth, d.width, d.height)) &&
            CanBeTargetFormat(d.format, fi.compressed) && info.kind == kTex2D) {
            info.flags |= kTexTarget;
            h[0] = key, h[1] = 0, h[2] = 1;
            return true;
        }
        StagePalette pal;
        ReadStagePalette(s, i, d, pal);
        uint64_t id = Hash(&d, sizeof(d), pal.hash ^ pal.va);
        h[0] = uint32_t(id), h[1] = uint32_t(id >> 32), h[2] = 0;
        // cheap change check once per frame: lock epochs plus a sample of the bytes (D3D12Backend::BindGuestTexture)
        if (!hashed_.count(id)) {
            hashed_.insert(id);
            uint64_t sig = MemSig(d.data, TotalSize(d)) ^ DirtyEpoch(d.data) ^ pal.hash;
            auto it = images_.find(id);
            if ((it == images_.end() || it->second != sig) && SendImage(id, d, fi, pal)) images_[id] = sig;
        }
        return true;
    }

    bool SendImage(uint64_t id, const TextureDesc& d, const FormatInfo& fi, const StagePalette& pal) {
        uint32_t faces = d.cube ? 6 : 1;
        Host host = fi.host == Host::R16Unorm ? Host::R32F : fi.host;
        if (fi.compressed && !(g_ring.features & 1)) {
            static bool warned = false;
            if (!warned) LOGW("webgpu: the browser has no BC texture formats; compressed textures are skipped");
            warned = true;
            return false;
        }
        std::vector<uint32_t> p{uint32_t(id), uint32_t(id >> 32), d.width, d.height, d.depth, d.levels,
                                uint32_t(d.cube ? 3 : d.depth > 1 ? 2 : 1), uint32_t(host)};
        std::vector<uint8_t> px;
        const uint8_t* guest = static_cast<const uint8_t*>(kernel::HostPtr(d.data));
        for (uint32_t f = 0; f < faces; f++) {
            for (uint32_t l = 0; l < d.levels; l++) {
                uint32_t pitch = 0;
                DecodeLevel(d, l, f, guest, pal.va ? pal.colours : nullptr, px, pitch);
                if (fi.host == Host::R16Unorm) {  // no 16 bit unorm texture format in WebGPU's core: to float
                    std::vector<uint8_t> wide(px.size() * 2);
                    for (size_t k = 0; k + 1 < px.size(); k += 2) {
                        float v = uint16_t(px[k] | px[k + 1] << 8) / 65535.0f;
                        std::memcpy(&wide[k * 2], &v, 4);
                    }
                    px.swap(wide);
                    pitch *= 2;
                }
                p.push_back(pitch);
                PutWords(p, px.data(), px.size());
            }
        }
        return Put(kOpImage, {{p.data(), uint32_t(p.size())}});
    }

    static bool UnitTexcoords(const Assembled& a) {
        if (!a.vertices.empty() || !a.vertex_count || a.vertex_count > 4096) return false;
        bool any = false;
        for (int r = 0; r < 16; r++) {
            const VertexElement& e = a.elements[size_t(r)];
            if (!((a.regs >> r) & 1) || e.format != DXGI_FORMAT_R32G32_FLOAT) continue;
            const VertexBuffer& vb = a.buffers[e.buffer];
            if (!vb.src || !vb.stride || uint64_t(a.vertex_count) * vb.stride > vb.bytes) return false;
            for (uint32_t v = 0; v < a.vertex_count; v++) {
                float uv[2];
                std::memcpy(uv, vb.src + size_t(v) * vb.stride + e.offset, sizeof(uv));
                if (!(uv[0] >= -1e-4f && uv[0] <= 1.0001f && uv[1] >= -1e-4f && uv[1] <= 1.0001f)) return false;
            }
            any = true;
        }
        return any;
    }

    static bool AnyStageWraps(const State& s) {
        for (const auto& t : s.tss)
            if (t[kTssAddressU] == 1 || t[kTssAddressV] == 1) return true;
        return false;
    }

    // D3DTADDRESS 1 wrap 2 mirror 3 clamp 4 border 5 clamp to edge; D3DTEXF 1 point 2 linear 3 anisotropic
    static uint32_t SamplerWord(const State& s, int i, bool clamp) {
        const auto& t = s.tss[size_t(i)];
        uint32_t au = clamp && t[kTssAddressU] == 1 ? 3 : t[kTssAddressU];
        uint32_t av = clamp && t[kTssAddressV] == 1 ? 3 : t[kTssAddressV];
        uint32_t aniso = (t[kTssMinFilter] == 3 || t[kTssMagFilter] == 3) ? std::max<uint32_t>(t[kTssMaxAnisotropy], 1) : 1;
        return (au & 7) | (av & 7) << 3 | (t[kTssAddressW] & 7) << 6 | (t[kTssMinFilter] & 3) << 9 |
               (t[kTssMagFilter] & 3) << 11 | (t[kTssMipFilter] & 3) << 13 | (std::min<uint32_t>(aniso, 16) & 31) << 15 |
               std::min<uint32_t>(t[kTssMaxMipLevel], 15) << 20 | CmpIndex(s.rs[kRsShadowFunc]) << 24;
    }

    // a depth buffer drawn as colour into another depth buffer's memory: copied as depth (D3D12Backend::DepthCopy)
    bool DepthCopy(const State& s) {
        TextureDesc d;
        if (!s.rt.header || !s.textures[0] || !Describe(s.textures[0], d) || Format(d.format).depth || d.cube ||
            d.depth != 1)
            return false;
        uint32_t gw = 0, gh = 0;
        SurfaceSize(s.rt, gw, gh);
        uint32_t dst_key = reference::GpuKey(s.rt.data), src_key = reference::GpuKey(d.data);
        if (dst_key == src_key || !targets_.count(TargetKey(dst_key, true, gw, gh)) ||
            !targets_.count(TargetKey(src_key, true, d.width, d.height)))
            return false;
        uint32_t p[6] = {src_key, d.width, d.height, dst_key, gw, gh};
        Put(kOpDepthCopy, {{p, 6}});
        return true;
    }

    struct WebKey {  // the fields KeyBlend and KeyRasterizer fill (draw_setup.h)
        uint32_t blend = 0, src = 0, dst = 0, op = 0, wmask = 0, cull = 0, ccw = 0, fill = 0;
    };

    void DrawShared(const State& s, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices) {
        if (SpriteOnStage3(s, prim)) {
            DrawShared(SpriteStageZero(s), prim, start, count, indices);
            return;
        }
        if (DepthCopy(s)) return;
        Assembled& a = asm_;
        if (!Assemble(s, prim, start, count, indices, a)) return;
        bool has_rt = s.rt.header && s.rt.data != 0;
        bool has_ds = s.zs.header && s.zs.data && (s.rs[kRsZEnable] || s.rs[kRsStencilEnable]);
        if (!has_rt && !has_ds) return;
        GuestExtent size{640, 480};
        SurfaceSize(has_rt ? s.rt : s.zs, size.w, size.h);
        uint32_t zw = 0, zh = 0;
        if (has_ds) SurfaceSize(s.zs, zw, zh);
        if (has_rt && has_ds && (size.w != zw || size.h != zh)) {
            has_ds = false;
            zw = zh = 0;
        }

        // shaders: the same choice and generator as D3D12Backend::Draw
        ShaderChoice sh = ClassifyShaders(s, a, count);
        if (sh.prog) EnsureProgramHash(s);
        uint64_t vkey = VertexKey(s, a, sh);
        TexInfo info[4];
        uint32_t texw[4][3] = {};
        TextureDesc descs[4];
        bool bound[4] = {};
        for (int i = 0; i < 4; i++) bound[i] = StageTexture(s, i, info[i], texw[i], descs[i]);
        PixelKey pk = MakePixelKey(s, info);
        pk.quad = sh.quad;
        uint64_t pkey = Hash(&pk, sizeof(pk)) ^ 0x9E3779B97F4A7C15ull;
        if (!shaders_.count(vkey)) {
            std::string vs = sh.prog ? VertexProgramWgsl(s.vs->program, a.regs, a.decode)
                                     : FixedVertexWgsl(a.regs, a.pretransformed, a.decode, sh.fk);
            if (sh.prog && sh.sprite) vs = SpriteSizeWgsl(std::move(vs));
            vs = ReversedDepthWgsl(std::move(vs));
            SendShader(vkey, 0, sh.quad ? QuadWgsl(std::move(vs)) : vs);
        }
        if (!shaders_.count(pkey)) SendShader(pkey, 1, PixelWgsl(pk));

        // pipeline state: KeyBlend, KeyRasterizer and D3D12Backend::KeyDepthStencil's fields
        WebKey k;
        KeyBlend(s, k);
        KeyRasterizer(s, a, k);
        uint32_t zen = has_ds && s.rs[kRsZEnable] != 0, zwrite = 0, zfunc = 0, sten = has_ds && s.rs[kRsStencilEnable] != 0;
        if (zen) zwrite = SimpleState(s, kMZWriteEnable) != 0, zfunc = SimpleState(s, kMZFunc);

        // constants as D3D12Backend::UploadConstants fills them
        DrawConstants c{};
        FillSurfaceConstants(s, size, c);
        FillFogConstants(s, sh.prog, c);
        FillCombinerConstants(s, c);
        FillDepthConstants(s, sh.prog, a.topology, c);
        for (int i = 0; i < 4; i++) {
            if (bound[i]) c.texsize[i][0] = float(descs[i].width), c.texsize[i][1] = float(descs[i].height);
            if (info[i].flags & kTexTarget) c.rtshift[i] = RenderTargetShift(s, sh.prog, 1.0f);
            c.spread[i][3] = 1.0f;
            const auto& t = s.tss[size_t(i)];
            c.bump[i][0] = Bits(t[kTssBumpEnvMat00]), c.bump[i][1] = Bits(t[kTssBumpEnvMat01]);
            c.bump[i][2] = Bits(t[kTssBumpEnvMat10]), c.bump[i][3] = Bits(t[kTssBumpEnvMat11]);
            c.bumpl[i][0] = Bits(t[kTssBumpEnvLScale]), c.bumpl[i][1] = Bits(t[kTssBumpEnvLOffset]);
        }
        if (a.topology == Topology::Points) c.surf[3] = 1.0f;
        if (sh.fixed_transform) {
            FillTransformConstants(s, size, c);
            FillLightConstants(s, sh.fk, c);
        }
        // the console's signed blends: an add of one sign, then a reverse subtract of the other (D3D12Backend::Draw)
        constexpr uint32_t kOpAddSigned = 0xF006, kOpRevSubtractSigned = 0xF005, kOpAdd = 0x8006, kOpRevSubtract = 0x800B;
        uint32_t guest_op = k.op;
        bool signed_op = k.blend && (guest_op == kOpAddSigned || guest_op == kOpRevSubtractSigned);
        if (signed_op) k.op = kOpAdd, c.sblend[0] = guest_op == kOpAddSigned ? 1.0f : 2.0f;

        // viewport and scissor (D3D12Backend::SetViewportScissor at the console's size)
        int32_t x0 = int32_t(s.viewport.x), y0 = int32_t(s.viewport.y);
        int32_t x1 = int32_t(s.viewport.x + s.viewport.w), y1 = int32_t(s.viewport.y + s.viewport.h);
        if (s.scissor_count) {
            x0 = std::max(x0, int32_t(s.scissors[0])), y0 = std::max(y0, int32_t(s.scissors[1]));
            x1 = std::min(x1, int32_t(s.scissors[2])), y1 = std::min(y1, int32_t(s.scissors[3]));
        }
        int32_t W = int32_t(size.w), H = int32_t(size.h);
        x0 = std::clamp(x0, 0, W), x1 = std::clamp(x1, 0, W), y0 = std::clamp(y0, 0, H), y1 = std::clamp(y1, 0, H);

        uint32_t h[kDraw2Header] = {};
        h[0] = uint32_t(vkey), h[1] = uint32_t(vkey >> 32), h[2] = uint32_t(pkey), h[3] = uint32_t(pkey >> 32);
        h[4] = has_rt ? reference::GpuKey(s.rt.data) : 0, h[5] = size.w, h[6] = size.h;
        h[7] = has_ds ? reference::GpuKey(s.zs.data) : 0, h[8] = zw, h[9] = zh;
        h[10] = uint32_t(a.topology) | uint32_t(a.strip) << 8;
        h[11] = k.blend, h[12] = k.src, h[13] = k.dst, h[14] = k.op, h[15] = k.wmask;
        h[16] = zen, h[17] = zwrite, h[18] = zfunc, h[19] = sten;
        if (sten) {
            h[20] = SimpleState(s, kMStencilFunc), h[21] = SimpleState(s, kMStencilMask) & 0xFF;
            h[22] = SimpleState(s, kMStencilWriteMask) & 0xFF, h[23] = s.rs[kRsStencilFail];
            h[24] = SimpleState(s, kMStencilZFail), h[25] = SimpleState(s, kMStencilPass);
        }
        h[26] = k.cull, h[27] = k.ccw, h[28] = k.fill, h[29] = SimpleState(s, kMBlendColor);
        h[30] = SimpleState(s, kMStencilRef) & 0xFF;
        h[31] = uint32_t(x0), h[32] = uint32_t(y0), h[33] = uint32_t(x1), h[34] = uint32_t(y1);
        h[35] = a.regs, h[36] = a.buffer_count, h[37] = a.index_count, h[38] = a.index16, h[39] = a.draw_count;
        h[40] = uint32_t(a.base_vertex);
        for (int r = 0; r < 16; r++)
            if ((a.regs >> r) & 1) {
                const VertexElement& e = a.elements[size_t(r)];
                h[41 + r] = e.format | uint32_t(e.buffer) << 8 | uint32_t(e.offset) << 16;
            }
        for (uint32_t b = 0; b < a.buffer_count && b < 16; b++) h[57 + b] = a.buffers[b].stride;
        bool clamp_uv = AnyStageWraps(s) && UnitTexcoords(a);
        for (int i = 0; i < 4; i++) {
            h[73 + i * 3] = texw[i][0], h[74 + i * 3] = texw[i][1];
            // bit 0 bound, 1 a render target, 4.. TexKind, 8.. shadow
            h[75 + i * 3] = bound[i] ? (1u | texw[i][2] << 1 | uint32_t(info[i].kind) << 4 | uint32_t(info[i].shadow) << 8) : 0;
            h[85 + i] = SamplerWord(s, i, clamp_uv);
        }
        bool vc_same = last_vc_valid_ && !std::memcmp(last_vc_.data(), s.vsc.data(), sizeof(s.vsc));
        bool dc_same = last_dc_valid_ && !std::memcmp(&last_dc_, &c, sizeof(c));
        h[89] = (vc_same ? 0 : 1) | (dc_same ? 0 : 2);
        float zslope = has_ds && OffsetsPolygons(s, a.topology) ? -gpu::Bits(SimpleState(s, kMSlopeScale)) : 0.0f;
        h[90] = Word(zslope);
        h[92] = uint32_t(sizeof(DrawConstants) / 4);

        std::vector<uint32_t> tail;
        // vertex buffers as the input assembler reads them, D3DCOLOR elements turned from B G R A to R G B A bytes
        for (uint32_t b = 0; b < a.buffer_count; b++) {
            const VertexBuffer& vb = a.buffers[b];
            size_t at = tail.size();
            PutWords(tail, vb.src, vb.bytes);
            uint8_t* bytes = reinterpret_cast<uint8_t*>(&tail[at + 1]);
            for (int r = 0; r < 16; r++) {
                const VertexElement& e = a.elements[size_t(r)];
                if (!((a.regs >> r) & 1) || e.buffer != b || e.format != DXGI_FORMAT_B8G8R8A8_UNORM || !vb.stride) continue;
                for (uint32_t o = e.offset; o + 4 <= vb.bytes; o += vb.stride) std::swap(bytes[o], bytes[o + 2]);
            }
        }
        PutWords(tail, a.index_count ? a.index_src : nullptr, a.index_count ? size_t(a.index_count) * (a.index16 ? 2 : 4) : 0);

        std::vector<Span> spans{{h, kDraw2Header}};
        if (!vc_same) spans.push_back({s.vsc.data(), uint32_t(sizeof(s.vsc) / 4)});
        if (!dc_same) spans.push_back({&c, uint32_t(sizeof(c) / 4)});
        spans.push_back({tail.data(), uint32_t(tail.size())});
        if (PutSpans(kOpDraw2, spans)) {
            std::memcpy(last_vc_.data(), s.vsc.data(), sizeof(s.vsc));
            last_dc_ = c;
            last_vc_valid_ = last_dc_valid_ = true;
        }
        if (signed_op) {  // the part that subtracts
            h[14] = kOpRevSubtract;
            c.sblend[0] = guest_op == kOpAddSigned ? 2.0f : 1.0f;
            h[89] = 2;
            std::vector<Span> again{{h, kDraw2Header}, {&c, uint32_t(sizeof(c) / 4)}, {tail.data(), uint32_t(tail.size())}};
            if (PutSpans(kOpDraw2, again)) last_dc_ = c;
        }
        if (has_rt) {
            uint32_t rt_key = reference::GpuKey(s.rt.data);
            targets_.insert(TargetKey(rt_key, false, size.w, size.h));
            color_targets_.insert(rt_key);
            reference::MarkGpuTarget(s.rt.data, size.w);
        }
        if (has_ds) targets_.insert(TargetKey(reference::GpuKey(s.zs.data), true, zw, zh));
    }

    bool PutSpans(uint32_t op, const std::vector<Span>& spans) {
        uint32_t words = 0;
        for (const Span& sp : spans) words += sp.words;
        if (overflowed_ || used_ + 2 + words > kBufferWords) {
            overflowed_ = true;
            return false;
        }
        uint32_t* buf = g_ring.buffers[(frame_ + 1) & 1];
        buf[used_++] = op;
        buf[used_++] = words;
        for (const Span& sp : spans) {
            std::memcpy(buf + used_, sp.data, size_t(sp.words) * 4);
            used_ += sp.words;
        }
        return true;
    }

    Assembled asm_;
    bool reference_ = false;
    std::set<uint64_t> targets_;            // SizedTargetKey of render and depth targets drawn or cleared
    std::set<uint32_t> color_targets_;      // color target keys drawn or cleared
    std::set<uint64_t> shaders_;            // shader ids the page has
    std::map<uint64_t, uint64_t> images_;   // texture id -> hash of the bytes the page has
    std::set<uint64_t> hashed_;             // texture ids checked this frame
    std::array<std::array<float, 4>, 192> last_vc_{};
    DrawConstants last_dc_{};
    bool last_vc_valid_ = false, last_dc_valid_ = false;

    std::unique_ptr<Backend> inner_;
    uint32_t width_ = 640, height_ = 480;
    uint32_t frame_ = 0, used_ = 0;
    bool overflowed_ = false, dual_ = false;
    std::map<uint32_t, uint64_t> uploaded_;  // texture key -> hash of what the page has
};

}  // namespace

std::unique_ptr<Backend> CreateGpuRecorder(std::unique_ptr<Backend> inner) {
    return std::make_unique<GpuRecorder>(std::move(inner));
}

}  // namespace gpu
