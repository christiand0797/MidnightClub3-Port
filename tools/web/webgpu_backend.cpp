// Browser port, step 4: the game's draws on the player's GPU through WebGPU (docs/notes/browser_port_plan.md).
//
// The game's threads cannot own WebGPU: getting an adapter and device are promises, and those threads block. So, as
// nfsu2-web does (runtime/web/gpu_proto.h), the backend records commands into shared memory and the page's main thread
// runs them with WebGPU (tools/web/gpu.js) once per animation frame:
//
//   game threads --(gpu::Backend calls)--> GpuRecorder --(frame command buffer, shared memory)--> gpu.js --> WebGPU
//
// Two command buffers alternate by frame. Present publishes the frame's buffer and waits while the page is still
// running the one before (at most one frame in flight). Commands are little-endian u32 words: op, word count of the
// payload, payload. The recorder wraps the CPU stand in (harness/null_backend.cpp), which keeps the console-memory
// effects (copies into guest memory, the optional CPU frames) until the WebGPU side does them.
//
// This first slice records clears and presents only; draws, textures and shaders follow.
#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

#include "core/log.h"
#include "gpu/backend.h"

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#else
#define EMSCRIPTEN_KEEPALIVE
#endif

namespace {

enum Op : uint32_t {
    kOpClear = 1,    // flags (D3DCLEAR), color (D3DCOLOR ARGB), z (float bits), stencil, rect count, rects (4 x i32 each)
    kOpPresent = 2,  // width, height of the console back buffer
};

constexpr uint32_t kBufferWords = 1u << 20;  // 4 MiB per frame buffer

// Shared with gpu.js; the layout is part of the protocol (web_gpu_ring returns its address)
struct Ring {
    std::atomic<uint32_t> published{0};  // last frame whose buffer is complete
    std::atomic<uint32_t> consumed{0};   // last frame the page has run
    uint32_t words[2] = {0, 0};          // length of each buffer, in words
    uint32_t overflow = 0;               // frames dropped for exceeding a buffer
    uint32_t reserved[3] = {};
    uint32_t* buffers[2] = {nullptr, nullptr};
};
Ring g_ring;
std::vector<uint32_t> g_store[2];

}  // namespace

extern "C" EMSCRIPTEN_KEEPALIVE Ring* web_gpu_ring() { return &g_ring; }

namespace gpu {

namespace {

class GpuRecorder final : public Backend {
public:
    explicit GpuRecorder(std::unique_ptr<Backend> inner) : inner_(std::move(inner)) {
        for (int i = 0; i < 2; i++) {
            g_store[i].assign(kBufferWords, 0);
            g_ring.buffers[i] = g_store[i].data();
        }
    }

    bool Init(uint32_t w, uint32_t h) override {
        width_ = w, height_ = h;
        LOGI("webgpu: recording for the page, back buffer %ux%u", w, h);
        return inner_->Init(w, h);
    }
    void Clear(const State& s, uint32_t n, const uint8_t* rects, uint32_t flags, uint32_t color, float z,
               uint32_t stencil) override {
        uint32_t zbits;
        std::memcpy(&zbits, &z, 4);
        std::vector<uint32_t> payload{flags, color, zbits, stencil, rects ? n : 0};
        if (rects)
            for (uint32_t i = 0; i < n * 4; i++) {
                uint32_t v;
                std::memcpy(&v, rects + i * 4, 4);
                payload.push_back(v);
            }
        Put(kOpClear, payload.data(), uint32_t(payload.size()));
        inner_->Clear(s, n, rects, flags, color, z, stencil);
    }
    void Draw(const State& s, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices) override {
        inner_->Draw(s, prim, start, count, indices);
    }
    void Copy(const Surface& src, const Surface& dst, uint32_t n, uint32_t rects, uint32_t points) override {
        inner_->Copy(src, dst, n, rects, points);
    }
    void Present(const Surface& front) override {
        uint32_t size[2] = {width_, height_};
        Put(kOpPresent, size, 2);
        inner_->Present(front);
        // publish this frame, then wait while the page still runs the previous one
        uint32_t frame = frame_ + 1;
        g_ring.words[frame & 1] = used_;
        g_ring.published.store(frame, std::memory_order_release);
        while (g_ring.consumed.load(std::memory_order_acquire) + 1 < frame)
            std::this_thread::sleep_for(std::chrono::microseconds(500));
        frame_ = frame;
        used_ = 0;
        overflowed_ = false;
    }
    void Sync(uint32_t va, uint32_t size) override { inner_->Sync(va, size); }
    void SyncLate(uint32_t va) override { inner_->SyncLate(va); }
    void Flush() override { inner_->Flush(); }
    void SetGamma(const uint8_t* ramp) override { inner_->SetGamma(ramp); }

private:
    // append one command to the buffer of the frame being recorded (frame_ + 1)
    void Put(uint32_t op, const uint32_t* payload, uint32_t words) {
        uint32_t* buf = g_ring.buffers[(frame_ + 1) & 1];
        if (overflowed_ || used_ + 2 + words > kBufferWords) {
            if (!overflowed_) g_ring.overflow++;
            overflowed_ = true;
            return;
        }
        buf[used_++] = op;
        buf[used_++] = words;
        std::memcpy(buf + used_, payload, words * 4);
        used_ += words;
    }

    std::unique_ptr<Backend> inner_;
    uint32_t width_ = 640, height_ = 480;
    uint32_t frame_ = 0, used_ = 0;
    bool overflowed_ = false;
};

}  // namespace

std::unique_ptr<Backend> CreateGpuRecorder(std::unique_ptr<Backend> inner) {
    return std::make_unique<GpuRecorder>(std::move(inner));
}

}  // namespace gpu
