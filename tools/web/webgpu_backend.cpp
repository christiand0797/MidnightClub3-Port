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
//   2 present: front buffer (guest address), width, height, post/grade/FXAA/CAS words (see Present)
//   3 texture: key, width, height, then width * height RGBA8 texels
//   4 draw:    kDrawHeader words (see Draw), vertices (reference::kVertexFloats floats each), u32 indices
//   5 copy:    src, src width, src height, dst, dst width, dst height, rect count, rects (x0 y0 x1 y1 dx dy, i32)
//   6 shader:  id (2 words), stage (0 vertex, 1 pixel), byte length, WGSL text (padded to words)
//   7 image:   id (2 words), width, height, depth, levels, kind (1 2D, 2 3D, 3 cube), host format (gpu::Host), then per
//              face and level: row pitch, byte length, bytes (padded to words)
//   8 draw2:   kDraw2Header words (see DrawShared), vertex constants (if flagged), DrawConstants (if flagged), per
//              vertex buffer: byte length, bytes; then the indices (byte length, bytes)
//   9 depth:   src_key, src_w, src_h, dst_key, dst_w, dst_h
//  10 gamma:   256 RGBA8 words (1024 bytes) for the console's 256x1 gamma LUT
//  11 evict:   id (2 words) of an idle guest texture to destroy on the page
// The recorder wraps the CPU stand in (harness/null_backend.cpp) for copies and the console-memory effects.
#include <atomic>
#include <chrono>
#include <cmath>
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

#include "app/app.h"
#include "app/resolution.h"
#include "core/log.h"
#include "gpu/backend.h"
#include "gpu/car_paint.h"
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

enum Op : uint32_t {
    kOpClear = 1,
    kOpPresent = 2,
    kOpTexture = 3,
    kOpDraw = 4,
    kOpCopy = 5,
    kOpShader = 6,
    kOpImage = 7,
    kOpDraw2 = 8,
    kOpDepthCopy = 9,
    kOpGamma = 10,
    kOpEvictImage = 11,
    kOpReplacement = 12,  // texture pack replacement: id (2 words), then RGBA8 texels like OP_IMAGE
    kOpPbrMaps = 13,      // PBR companion maps: id (2 words), nrm_w, nrm_h, nrm_bytes, orm_w, orm_h, orm_bytes, then nrm pixels, then orm pixels
};
constexpr uint32_t kDraw2Header = 96;
constexpr uint32_t kDrawHeader = 98;
constexpr uint32_t kBufferWords = 8u << 20;  // 32 MiB per frame buffer

// Shared with gpu.js; the layout is part of the protocol (web_gpu_ring returns its address)
struct Ring {
    std::atomic<uint32_t> published{0};  // last frame whose buffer is complete
    std::atomic<uint32_t> consumed{0};   // last frame the page has run
    uint32_t words[2] = {0, 0};          // length of each buffer, in words
    uint32_t overflow = 0;               // frames cut short for exceeding a buffer
    uint32_t features = 0;               // the page's: bit 0 texture-compression-bc
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

uint64_t TargetKey(uint32_t key, bool depth, uint32_t gw, uint32_t gh) {
    return uint64_t(gw & 0x7FFF) << 48 | uint64_t(gh & 0x7FFF) << 33 | uint64_t(depth ? 1 : 0) << 32 | key;
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

// textures recognised by their content hash (d3d12.cpp:544)
enum TexRole : uint8_t { kRoleNone, kRoleNeon };
constexpr uint64_t kNeonTextures[] = {0x2EF7B12D7E0B7B93ull, 0x45CEC7A39FE1B185ull, 0x275CDB0152EEA3A0ull};
constexpr uint32_t kRoleMaxSize = 64;

uint64_t ContentHash(const gpu::TextureDesc& d, const uint8_t* guest, uint32_t bytes, uint64_t palette) {
    uint32_t shape[6] = {d.format, d.width, d.height, d.depth, d.levels, d.cube ? 1u : 0u};
    uint64_t h = gpu::Hash(shape, sizeof(shape));
    h = gpu::Hash(guest, bytes, h);
    return palette ? gpu::Hash(&palette, sizeof(palette), h) : h;
}

uint8_t TextureRole(uint64_t name) {
    for (uint64_t n : kNeonTextures)
        if (n == name) return kRoleNeon;
    return kRoleNone;
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

constexpr uint64_t kCarPaintSalt = 0xCA9A1A7ull << 32;
constexpr uint64_t kPixelKeySalt = 0x9E3779B97F4A7C15ull;
constexpr float kUpscaledMin = 1.01f;
constexpr float kGlowBoxTexels = 3.0f;
constexpr uint32_t kBlendOne = 1, kBlendSrcAlpha = 0x302, kBlendInvSrcAlpha = 0x303;
constexpr uint32_t kFullScreenWidth = 640;

bool CarLightingProgram(const State& s, const Assembled& a) {
    if (s.vs->car_lighting < 0)
        s.vs->car_lighting = car_paint::LightingProgram(VertexProgramWgsl(s.vs->program, a.regs, a.decode)) ? 1 : 0;
    return s.vs->car_lighting > 0;
}

uint32_t CarPaintTargetMask(const PixelKey& pk) {
    uint32_t m = 0;
    for (int st = 0; st < 3; st++)
        if (pk.tex[st].kind == kTex2D && (pk.tex[st].flags & kTexTarget)) m |= 1u << st;
    return m;
}

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
        ApplySettings();
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
        TargetInfo* rt_t = has_rt ? &EnsureTarget(rt_key, false, rw, rh) : nullptr;
        TargetInfo* zs_t = has_ds ? &EnsureTarget(zs_key, true, zw, zh) : nullptr;
        auto emit_clear = [&](uint32_t fl, uint32_t rk, uint32_t zk, const TargetInfo& ti) {
            uint32_t count = (n && rects) ? n : 1;
            std::vector<uint32_t> p{fl, color, Word(z), stencil, rk, zk, ti.w, ti.h, count};
            auto push_rect = [&](int x0, int y0, int x1, int y1) {
                auto mr = app::resolution::MapRect({ti.gw, ti.gh}, {ti.w, ti.h}, {x0, y0, x1, y1});
                for (int32_t v : {mr.left, mr.top, mr.right, mr.bottom}) p.push_back(uint32_t(v));
            };
            if (n && rects) {
                for (uint32_t i = 0; i < n; i++) {
                    int32_t r[4];
                    std::memcpy(r, rects + i * 16, 16);
                    push_rect(r[0], r[1], r[2], r[3]);
                }
            } else {
                const auto& vp = s.viewport;
                push_rect(int(vp.x), int(vp.y), int(vp.x + vp.w), int(vp.y + vp.h));
            }
            Put(kOpClear, {{p.data(), uint32_t(p.size())}});
        };
        if (rt_t && zs_t && (rt_t->w != zs_t->w || rt_t->h != zs_t->h)) {
            emit_clear(flags & 0xF0, rt_key, 0, *rt_t);
            emit_clear(flags & 3, 0, zs_key, *zs_t);
        } else if (rt_t || zs_t) {
            emit_clear(flags, rt_key, zs_key, rt_t ? *rt_t : *zs_t);
        }
        if (rt_t) color_targets_.insert(rt_key);
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
                       (FindTarget(src_key, false, sw, sh) != nullptr || color_targets_.count(src_key));
        if (gpu_src) {
            TargetInfo& st = EnsureTarget(src_key, false, sw, sh);
            TargetInfo& dt = EnsureTarget(dst_key, false, dw, dh);
            float sx = float(st.w) / st.gw, sy = float(st.h) / st.gh;
            float dx_scale = float(dt.w) / dt.gw, dy_scale = float(dt.h) / dt.gh;
            uint32_t count = n && rects ? n : 1;
            std::vector<uint32_t> p{src_key, st.w, st.h, dst_key, dt.w, dt.h, count};
            for (uint32_t i = 0; i < count; i++) {
                int32_t x0 = 0, y0 = 0, x1 = int32_t(st.w), y1 = int32_t(st.h), dx = 0, dy = 0;
                if (n && rects) {
                    int32_t rx0 = int32_t(kernel::R32(rects + i * 16)), ry0 = int32_t(kernel::R32(rects + i * 16 + 4));
                    int32_t rx1 = int32_t(kernel::R32(rects + i * 16 + 8)), ry1 = int32_t(kernel::R32(rects + i * 16 + 12));
                    x0 = int32_t(uint32_t(rx0) * sx);
                    y0 = int32_t(uint32_t(ry0) * sy);
                    x1 = int32_t(std::min<uint32_t>(uint32_t(rx1) * sx, st.w));
                    y1 = int32_t(std::min<uint32_t>(uint32_t(ry1) * sy, st.h));
                    dx = x0, dy = y0;
                }
                if (points) {
                    int32_t px = int32_t(kernel::R32(points + i * 8)), py = int32_t(kernel::R32(points + i * 8 + 4));
                    dx = int32_t(uint32_t(px) * dx_scale);
                    dy = int32_t(uint32_t(py) * dy_scale);
                }
                for (int32_t v : {x0, y0, x1, y1, dx, dy}) p.push_back(uint32_t(v));
            }
            Put(kOpCopy, {{p.data(), uint32_t(p.size())}});
            reference::MarkGpuTarget(dst.data, dw);
            color_targets_.insert(dst_key);
        }
        if (dual_ || !gpu_src) inner_->Copy(src, dst, n, rects, points);
    }

    void Present(const Surface& front) override {
        if (ramp_dirty_) UploadGammaRamp();
        uint32_t front_key = reference::GpuKey(front.data);
        uint32_t gw = width_, gh = height_;
        SurfaceSize(front, gw, gh);
        TargetInfo& ft = EnsureTarget(front_key, false, gw, gh);
        app::ReportRenderResolution({ft.w, ft.h});
        const auto& g = app::Get().graphics;
        const app::Mode out = app::Output();
        uint32_t p[16] = {
            front_key,
            ft.w,
            ft.h,
            (g.post ? 1u : 0u) | (gamma_ ? 2u : 0u) | (app::Widescreen() ? 4u : 0u),
            g.aa,
            Word(g.aa_strength),
            Word(g.sharpen),
            Word(g.brightness),
            Word(g.contrast),
            Word(g.saturation),
            Word(g.vibrance),
            Word(g.gamma),
            out.width ? out.width : ft.w,
            out.height ? out.height : ft.h,
            gw,
            gh,
        };
        Put(kOpPresent, {{p, 16}});
        ReleaseOldTextures();
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
            ramp_dirty_ = gamma_;
            g_ring.overflow++;
        }
        frame_ = frame;
        used_ = 0;
        overflowed_ = false;
        ApplySettings();
    }

    void Sync(uint32_t va, uint32_t size) override { inner_->Sync(va, size); }
    void SyncLate(uint32_t va) override { inner_->SyncLate(va); }
    void Flush() override { inner_->Flush(); }
    void SetGamma(const uint8_t* ramp) override {
        std::memcpy(ramp_, ramp, sizeof(ramp_));
        ramp_dirty_ = true;
        inner_->SetGamma(ramp);
    }

private:
    struct Span {
        const void* data;
        uint32_t words;
    };

    struct TargetInfo {
        uint32_t gw = 0, gh = 0;
        uint32_t w = 0, h = 0;
        bool depth = false;
        bool screen_copy = false;
    };

    struct ImageEntry {
        uint64_t sig = 0;
        uint32_t frame = 0;
        uint8_t role = kRoleNone;
    };

    void ApplySettings() {
        if (reference_) {
            if (scale_ != 1.0f || offscreen_ != 1.0f || scene_.width || scene_.height) {
                targets_.clear();
                scale_ = 1.0f;
                offscreen_ = 1.0f;
                scene_ = {};
            }
            return;
        }
        app::Mode scene = app::RenderResolution();
        float want = app::RenderScale(height_);
        float off = std::clamp(app::Get().graphics.offscreen_scale, 1.0f, 3.0f);
        if (scene != scene_ || std::fabs(want - scale_) > 1e-4f || std::fabs(off - offscreen_) > 1e-4f) {
            targets_.clear();
            scale_ = want;
            offscreen_ = off;
            scene_ = scene;
            app::ReportRenderResolution({});
        }
    }

    TargetInfo* FindTarget(uint32_t key, bool depth, uint32_t gw, uint32_t gh) {
        auto it = targets_.find(TargetKey(key, depth, gw, gh));
        return it != targets_.end() ? &it->second : nullptr;
    }

    TargetInfo& EnsureTarget(uint32_t key, bool depth, uint32_t gw, uint32_t gh) {
        uint64_t tk = TargetKey(key, depth, gw, gh);
        auto it = targets_.find(tk);
        if (it != targets_.end()) return it->second;
        TargetInfo t;
        t.gw = gw;
        t.gh = gh;
        t.depth = depth;
        if (reference_) {
            t.w = gw;
            t.h = gh;
        } else {
            app::Mode size = app::resolution::Target({gw, gh}, {width_, height_}, scene_, scale_ * offscreen_);
            t.w = size.width;
            t.h = size.height;
        }
        return targets_[tk] = t;
    }

    void UploadGammaRamp() {
        ramp_dirty_ = false;
        uint32_t rgba[256];
        bool identity = true;
        for (int i = 0; i < 256; i++) {
            uint8_t r = ramp_[i], g = ramp_[256 + i], b = ramp_[512 + i];
            rgba[i] = uint32_t(r) | uint32_t(g) << 8 | uint32_t(b) << 16 | 0xFF000000u;
            identity &= r == i && g == i && b == i;
        }
        gamma_ = !identity;
        if (!Put(kOpGamma, {{rgba, 256}})) ramp_dirty_ = true;
    }

    void ReleaseOldTextures() {
        constexpr uint32_t kSweepEvery = 60;
        constexpr size_t kTexturesKept = 2048;
        constexpr uint32_t kIdleFrames = 600;
        if ((frame_ + 1) % kSweepEvery != 0 || images_.size() <= kTexturesKept) return;
        for (auto it = images_.begin(); it != images_.end();) {
            if (it->second.frame + kIdleFrames < frame_ + 1) {
                uint32_t p[2] = {uint32_t(it->first), uint32_t(it->first >> 32)};
                if (!Put(kOpEvictImage, {{p, 2}})) break;
                it = images_.erase(it);
            } else {
                ++it;
            }
        }
    }

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
    bool StageTexture(const State& s, int i, const TargetInfo* bound_rt, const TargetInfo* bound_ds, TexInfo& info,
                      uint32_t* h, TextureDesc& d, TargetInfo*& stage_tgt, float& stage_wide, uint8_t& stage_role) {
        stage_tgt = nullptr;
        stage_wide = 0.0f;
        stage_role = kRoleNone;
        if (!s.textures[size_t(i)] || !Describe(s.textures[size_t(i)], d)) return false;
        const FormatInfo& fi = Format(d.format);
        info.kind = d.cube ? kTexCube : d.depth > 1 ? kTex3D : kTex2D;
        info.rect = d.linear;
        if (fi.depth) info.shadow = fi.bits == 16 ? 2 : 1;
        uint32_t key = reference::GpuKey(d.data);
        if (TargetInfo* t = FindTarget(key, fi.depth, d.width, d.height);
            t && CanBeTargetFormat(d.format, fi.compressed) && info.kind == kTex2D) {
            if (bound_ds && t == bound_ds) return false;
            info.flags |= kTexTarget;
            stage_tgt = t;
            float k = std::max(float(t->w) / t->gw, float(t->h) / t->gh);
            if (!t->depth && k > kUpscaledMin && bound_rt && bound_rt != t &&
                (t->gw > bound_rt->gw || t->gh > bound_rt->gh)) {
                stage_wide = k;
                info.flags |= kTexWide;
            }
            h[0] = key, h[1] = 0, h[2] = 1;
            return true;
        }
        StagePalette pal;
        ReadStagePalette(s, i, d, pal);
        uint64_t id = Hash(&d, sizeof(d), pal.hash ^ pal.va);
        h[0] = uint32_t(id), h[1] = uint32_t(id >> 32), h[2] = 0;
        uint32_t total = TotalSize(d);
        // cheap change check once per frame: lock epochs plus a sample of the bytes (D3D12Backend::BindGuestTexture)
        auto it = images_.find(id);
        if (!hashed_.count(id)) {
            hashed_.insert(id);
            uint64_t sig = MemSig(d.data, total) ^ DirtyEpoch(d.data) ^ pal.hash;
            if (it == images_.end() || it->second.sig != sig) {
                if (SendImage(id, d, fi, pal)) {
                    uint8_t role = kRoleNone;
                    if (fi.compressed && d.width <= kRoleMaxSize && d.height <= kRoleMaxSize) {
                        const uint8_t* guest = static_cast<const uint8_t*>(kernel::HostPtr(d.data));
                        role = TextureRole(ContentHash(d, guest, total, pal.hash));
                    }
                    it = images_.insert_or_assign(id, ImageEntry{sig, frame_ + 1, role}).first;
                }
            }
        }
        if (it != images_.end()) {
            it->second.frame = frame_ + 1;
            stage_role = it->second.role;
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
    static uint32_t SamplerWord(const State& s, int i, bool clamp, uint32_t levels, bool shadow) {
        const auto& t = s.tss[size_t(i)];
        uint32_t au = clamp && t[kTssAddressU] == 1 ? 3 : t[kTssAddressU];
        uint32_t av = clamp && t[kTssAddressV] == 1 ? 3 : t[kTssAddressV];
        uint32_t minf = t[kTssMinFilter], magf = t[kTssMagFilter], mipf = t[kTssMipFilter];
        uint32_t aniso_setting = app::Get().aniso, detail = app::Get().graphics.texture_detail;
        if (detail >= 1 && !aniso_setting) aniso_setting = 16;
        uint32_t aniso = (minf == 3 || magf == 3) ? std::max<uint32_t>(t[kTssMaxAnisotropy], 1) : 1;
        if (aniso_setting && minf >= 2 && mipf && levels > 1 && !shadow) aniso = aniso_setting;
        uint32_t max_mip = std::min<uint32_t>(t[kTssMaxMipLevel], 15);
        if (detail >= 1 && !shadow) max_mip = 0;
        return (au & 7) | (av & 7) << 3 | (t[kTssAddressW] & 7) << 6 | (minf & 3) << 9 | (magf & 3) << 11 |
               (mipf & 3) << 13 | (std::min<uint32_t>(aniso, 16) & 31) << 15 | max_mip << 20 |
               CmpIndex(s.rs[kRsShadowFunc]) << 24 | (detail & 3) << 28;
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
        TargetInfo* src_t = FindTarget(src_key, true, d.width, d.height);
        TargetInfo* dst_t = FindTarget(dst_key, true, gw, gh);
        if (dst_key == src_key || !src_t || !dst_t) return false;
        uint32_t p[6] = {src_key, src_t->w, src_t->h, dst_key, dst_t->w, dst_t->h};
        Put(kOpDepthCopy, {{p, 6}});
        return true;
    }

    struct WebKey {  // the fields KeyBlend and KeyRasterizer fill (draw_setup.h)
        uint32_t blend = 0, src = 0, dst = 0, op = 0, wmask = 0, cull = 0, ccw = 0, fill = 0;
    };

    static bool ScaleScreenEffect(const WebKey& k, uint32_t zen, const TargetInfo* rt, const TargetInfo* src,
                                  uint32_t count, DrawConstants& c) {
        if (!rt || rt->gw < kFullScreenWidth || count != 4 || !k.blend || !src || src->depth || src->gw * 2 > rt->gw)
            return false;
        const auto& g = app::Get().graphics;
        if (k.src == kBlendSrcAlpha && k.dst == kBlendInvSrcAlpha && zen) {
            c.sblend[1] = g.motion_blur - 1.0f;
            return true;
        }
        if (k.src == kBlendSrcAlpha && k.dst == kBlendOne) {
            c.sblend[2] = std::sqrt(src->screen_copy ? g.glow : g.headlights) - 1.0f;
            return true;
        }
        return false;
    }

    static bool ScaleTextureEffect(const WebKey& k, const bool bound[4], const uint8_t role[4], DrawConstants& c) {
        bool neon = false;
        for (int i = 0; i < 4; i++) neon |= bound[i] && role[i] == kRoleNeon;
        if (!neon || !k.blend) return false;
        float scale = std::clamp(app::Get().graphics.neon, 0.0f, 4.0f);
        if (scale == 1.0f) return false;
        if (k.src == kBlendSrcAlpha) {
            c.sblend[3] = scale - 1.0f;
        } else if (scale > 0.0f) {
            c.sblend[2] = scale - 1.0f;
            c.sblend[1] = 1.0f / scale - 1.0f;
        } else {
            c.sblend[2] = -1.0f;
        }
        return true;
    }

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
        uint32_t rt_key = has_rt ? reference::GpuKey(s.rt.data) : 0;
        uint32_t zs_key = has_ds ? reference::GpuKey(s.zs.data) : 0;
        TargetInfo* bound_rt = has_rt ? &EnsureTarget(rt_key, false, size.w, size.h) : nullptr;
        TargetInfo* bound_ds = has_ds ? &EnsureTarget(zs_key, true, zw, zh) : nullptr;
        if (bound_rt && bound_ds && (bound_rt->w != bound_ds->w || bound_rt->h != bound_ds->h)) {
            has_ds = false;
            bound_ds = nullptr;
            zw = zh = 0;
        }
        const TargetInfo* active_t = bound_rt ? bound_rt : bound_ds;
        float target_scale = active_t && active_t->gw ? float(active_t->w) / float(active_t->gw) : 1.0f;

        // shaders: the same choice and generator as D3D12Backend::Draw
        ShaderChoice sh = ClassifyShaders(s, a, count);
        if (sh.prog) EnsureProgramHash(s);
        uint64_t vkey = VertexKey(s, a, sh);
        const auto& graphics = app::Get().graphics;
        sh.car_paint = graphics.car_paint && sh.prog && !sh.quad && !a.pretransformed && CarLightingProgram(s, a);
        if (sh.car_paint) vkey ^= kCarPaintSalt;

        TexInfo info[4];
        uint32_t texw[4][3] = {};
        TextureDesc descs[4];
        bool bound[4] = {};
        TargetInfo* stage_tgt[4] = {};
        float stage_wide[4] = {};
        uint8_t stage_role[4] = {};
        for (int i = 0; i < 4; i++)
            bound[i] = StageTexture(s, i, bound_rt, bound_ds, info[i], texw[i], descs[i], stage_tgt[i], stage_wide[i],
                                    stage_role[i]);
        PixelKey pk = MakePixelKey(s, info);
        pk.quad = sh.quad;
        pk.car_paint = sh.car_paint;
        uint64_t pkey = Hash(&pk, sizeof(pk)) ^ kPixelKeySalt;
        if (!shaders_.count(vkey)) {
            std::string vs = sh.prog ? VertexProgramWgsl(s.vs->program, a.regs, a.decode)
                                     : FixedVertexWgsl(a.regs, a.pretransformed, a.decode, sh.fk);
            if (sh.prog && sh.car_paint) vs = car_paint::VertexWgsl(std::move(vs));
            else if (sh.prog && sh.sprite) vs = SpriteSizeWgsl(std::move(vs));
            vs = ReversedDepthWgsl(std::move(vs));
            SendShader(vkey, 0, sh.quad ? QuadWgsl(std::move(vs)) : vs);
        }
        if (!shaders_.count(pkey)) {
            std::string ps = PixelWgsl(pk);
            if (pk.car_paint) ps = car_paint::PixelWgsl(std::move(ps), CarPaintTargetMask(pk));
            SendShader(pkey, 1, ps);
        }

        // pipeline state: KeyBlend, KeyRasterizer and D3D12Backend::KeyDepthStencil's fields
        WebKey k;
        KeyBlend(s, k);
        KeyRasterizer(s, a, k);
        uint32_t zen = has_ds && s.rs[kRsZEnable] != 0, zwrite = 0, zfunc = 0, sten = has_ds && s.rs[kRsStencilEnable] != 0;
        if (zen) zwrite = SimpleState(s, kMZWriteEnable) != 0, zfunc = SimpleState(s, kMZFunc);

        // constants as D3D12Backend::UploadConstants fills them
        DrawConstants c{};
        if (sh.car_paint) {
            c.carpaint[0] = std::clamp(graphics.car_relight, 0.0f, 1.0f);
            c.carpaint[1] = std::clamp(graphics.car_specular, 0.0f, 3.0f);
            c.carpaint[2] = std::clamp(graphics.car_roughness, 0.05f, 0.8f);
            c.carpaint[3] = std::clamp(graphics.car_reflection, 0.0f, 2.0f);
        }
        FillSurfaceConstants(s, size, c);
        FillFogConstants(s, sh.prog, c);
        FillCombinerConstants(s, c);
        FillDepthConstants(s, sh.prog, a.topology, c);
        float shift_ratio = active_t && active_t->gw && active_t->gh && active_t->w
                                ? (float(active_t->h) / active_t->gh) / (float(active_t->w) / active_t->gw)
                                : 1.0f;
        for (int i = 0; i < 4; i++) {
            if (bound[i]) {
                c.texsize[i][0] = float(descs[i].width);
                c.texsize[i][1] = float(descs[i].height);
                if (stage_wide[i] > 0.0f) {
                    float n = std::ceil(kGlowBoxTexels * stage_wide[i] * 0.5f);
                    c.spread[i][0] = kGlowBoxTexels / (n * float(descs[i].width));
                    c.spread[i][1] = kGlowBoxTexels / (n * float(descs[i].height));
                    c.spread[i][2] = n;
                }
            }
            if (info[i].flags & kTexTarget) c.rtshift[i] = RenderTargetShift(s, sh.prog, target_scale);
            c.spread[i][3] = shift_ratio;
            const auto& t = s.tss[size_t(i)];
            c.bump[i][0] = Bits(t[kTssBumpEnvMat00]), c.bump[i][1] = Bits(t[kTssBumpEnvMat01]);
            c.bump[i][2] = Bits(t[kTssBumpEnvMat10]), c.bump[i][3] = Bits(t[kTssBumpEnvMat11]);
            c.bumpl[i][0] = Bits(t[kTssBumpEnvLScale]), c.bumpl[i][1] = Bits(t[kTssBumpEnvLOffset]);
        }
        if (a.topology == Topology::Points) c.surf[3] = target_scale;
        if (sh.fixed_transform) {
            FillTransformConstants(s, size, c);
            FillLightConstants(s, sh.fk, c);
        }
        ScaleScreenEffect(k, zen, bound_rt, stage_tgt[0], count, c);
        ScaleTextureEffect(k, bound, stage_role, c);
        if (bound_rt) bound_rt->screen_copy = count == 4 && stage_tgt[0] && stage_tgt[0]->gw >= bound_rt->gw * 2;

        // the console's signed blends: an add of one sign, then a reverse subtract of the other (D3D12Backend::Draw)
        constexpr uint32_t kOpAddSigned = 0xF006, kOpRevSubtractSigned = 0xF005, kOpAdd = 0x8006, kOpRevSubtract = 0x800B;
        uint32_t guest_op = k.op;
        bool signed_op = k.blend && (guest_op == kOpAddSigned || guest_op == kOpRevSubtractSigned);
        if (signed_op) k.op = kOpAdd, c.sblend[0] = guest_op == kOpAddSigned ? 1.0f : 2.0f;

        // viewport and scissor (D3D12Backend::SetViewportScissor at host target size)
        int32_t W = int32_t(active_t ? active_t->w : size.w), H = int32_t(active_t ? active_t->h : size.h);
        float sx = float(W) / float(size.w), sy = float(H) / float(active_t ? active_t->gh : size.h);
        int32_t x0 = int32_t(s.viewport.x * sx), y0 = int32_t(s.viewport.y * sy);
        int32_t x1 = int32_t((s.viewport.x + s.viewport.w) * sx), y1 = int32_t((s.viewport.y + s.viewport.h) * sy);
        if (s.scissor_count) {
            x0 = std::max(x0, int32_t(s.scissors[0] * sx)), y0 = std::max(y0, int32_t(s.scissors[1] * sy));
            x1 = std::min(x1, int32_t(s.scissors[2] * sx)), y1 = std::min(y1, int32_t(s.scissors[3] * sy));
        }
        x0 = std::clamp(x0, 0, W), x1 = std::clamp(x1, 0, W), y0 = std::clamp(y0, 0, H), y1 = std::clamp(y1, 0, H);

        uint32_t h[kDraw2Header] = {};
        h[0] = uint32_t(vkey), h[1] = uint32_t(vkey >> 32), h[2] = uint32_t(pkey), h[3] = uint32_t(pkey >> 32);
        h[4] = rt_key, h[5] = bound_rt ? bound_rt->w : 0, h[6] = bound_rt ? bound_rt->h : 0;
        h[7] = has_ds ? zs_key : 0, h[8] = bound_ds ? bound_ds->w : 0, h[9] = bound_ds ? bound_ds->h : 0;
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
            uint32_t levels = bound[i] && !texw[i][2] ? descs[i].levels : 1;
            h[85 + i] = SamplerWord(s, i, clamp_uv, levels, info[i].shadow != 0);
        }
        bool vc_same = last_vc_valid_ && !std::memcmp(last_vc_.data(), s.vsc.data(), sizeof(s.vsc));
        bool dc_same = last_dc_valid_ && !std::memcmp(&last_dc_, &c, sizeof(c));
        h[89] = (vc_same ? 0 : 1) | (dc_same ? 0 : 2);
        float zslope_scale = bound_ds && bound_ds->gw ? float(bound_ds->w) / float(bound_ds->gw) : 1.0f;
        float zslope = has_ds && OffsetsPolygons(s, a.topology) ? -gpu::Bits(SimpleState(s, kMSlopeScale)) * zslope_scale : 0.0f;
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
            color_targets_.insert(rt_key);
            reference::MarkGpuTarget(s.rt.data, size.w);
        }
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
    app::Mode scene_{};
    float scale_ = 1.0f;
    float offscreen_ = 1.0f;
    uint8_t ramp_[768] = {};
    bool ramp_dirty_ = false, gamma_ = false;
    std::map<uint64_t, TargetInfo> targets_;  // TargetKey -> host & guest target info
    std::set<uint32_t> color_targets_;        // color target keys drawn or cleared
    std::set<uint64_t> shaders_;              // shader ids the page has
    std::map<uint64_t, ImageEntry> images_;   // texture id -> sig/frame/role of what the page has
    std::set<uint64_t> hashed_;               // texture ids checked this frame
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
