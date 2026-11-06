// Browser port, step 4: the game's draws on the player's GPU through WebGPU (docs/notes/browser_port_plan.md).
//
// The game's threads cannot own WebGPU: getting an adapter and device are promises, and those threads block. So, as
// nfsu2-web does (runtime/web/gpu_proto.h), the backend records commands into shared memory and the page's main thread
// runs them with WebGPU (tools/web/gpu.js) once per animation frame:
//
//   game threads --(gpu::Backend calls)--> GpuRecorder --(frame command buffer, shared memory)--> gpu.js --> WebGPU
//
// Draws run the reference renderer's vertex stage on the CPU (harness/reference_renderer.cpp: vertex programs, fixed
// function transform and lighting, fog) and hand the transformed triangles to the GPU, which rasterizes, samples,
// combines the texture stages and blends. Textures go up decoded to RGBA8 when their contents change.
//
// Two command buffers alternate by frame. Present publishes the frame's buffer and waits while the page is still
// running the one before. Commands are u32 words: op, payload word count, payload:
//   1 clear:   flags, color (ARGB), z (float bits), stencil, rt, zs, rt width, rt height, rect count, rects (4 x i32)
//   2 present: front buffer (guest address), width, height
//   3 texture: key, width, height, then width * height RGBA8 texels
//   4 draw:    kDrawHeader words (see Draw), vertices (reference::kVertexFloats floats each), u32 indices
//   5 copy:    src, src width, src height, dst, dst width, dst height, rect count, rects (x0 y0 x1 y1 dx dy, i32)
// The recorder wraps the CPU stand in (harness/null_backend.cpp) for copies and the console-memory effects.
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <thread>
#include <vector>

#include "core/log.h"
#include "gpu/backend.h"
#include "kernel/kernel.h"
#include "reference_renderer.h"

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#else
#define EMSCRIPTEN_KEEPALIVE
#endif

namespace {

enum Op : uint32_t { kOpClear = 1, kOpPresent = 2, kOpTexture = 3, kOpDraw = 4, kOpCopy = 5 };
constexpr uint32_t kDrawHeader = 98;
constexpr uint32_t kBufferWords = 8u << 20;  // 32 MiB per frame buffer

// Shared with gpu.js; the layout is part of the protocol (web_gpu_ring returns its address)
struct Ring {
    std::atomic<uint32_t> published{0};  // last frame whose buffer is complete
    std::atomic<uint32_t> consumed{0};   // last frame the page has run
    uint32_t words[2] = {0, 0};          // length of each buffer, in words
    uint32_t overflow = 0;               // frames cut short for exceeding a buffer
    uint32_t reserved[3] = {};
    uint32_t* buffers[2] = {nullptr, nullptr};
};
Ring g_ring;
std::vector<uint32_t> g_store[2];

uint32_t Bits(float f) {
    uint32_t u;
    std::memcpy(&u, &f, 4);
    return u;
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
    }

    bool Init(uint32_t w, uint32_t h) override {
        width_ = w, height_ = h;
        LOGI("webgpu: recording for the page, back buffer %ux%u", w, h);
        return inner_->Init(w, h);
    }
    void Clear(const State& s, uint32_t n, const uint8_t* rects, uint32_t flags, uint32_t color, float z,
               uint32_t stencil) override {
        std::vector<uint32_t> p{flags, color, Bits(z), stencil, reference::GpuKey(s.rt.data), reference::GpuKey(s.zs.data),
                                width_, height_, rects ? n : 0};
        if (rects)
            for (uint32_t i = 0; i < n * 4; i++) {
                uint32_t v;
                std::memcpy(&v, rects + i * 4, 4);
                p.push_back(v);
            }
        Put(kOpClear, {{p.data(), uint32_t(p.size())}});
        if (dual_) reference::Clear(s, n, rects, flags, color, z, stencil);
    }
    void Draw(const State& s, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices) override {
        reference::Draw(s, prim, start, count, indices);  // its vertex stage, then Sink
    }
    // CopyRects between surfaces: on the GPU (the screen effects copy the back buffer into a texture they then draw
    // with); the destination becomes a GPU target, so later draws sample it from there
    void Copy(const Surface& src, const Surface& dst, uint32_t n, uint32_t rects, uint32_t points) override {
        uint32_t sw = 0, sh = 0, dw = 0, dh = 0;
        if (reference::SurfaceSize(src, sw, sh) && reference::SurfaceSize(dst, dw, dh)) {
            uint32_t count = n && rects ? n : 1;
            std::vector<uint32_t> p{reference::GpuKey(src.data), sw, sh, reference::GpuKey(dst.data), dw, dh, count};
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
        }
        if (dual_) inner_->Copy(src, dst, n, rects, points);
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
        if (overflowed_) {
            uploaded_.clear();  // what this frame meant to upload may not have reached the page
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
        uint32_t h[kDrawHeader] = {d.rt, d.rt_w, d.rt_h, d.zs, Bits(d.zscale), uint32_t(d.left), uint32_t(d.top),
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
