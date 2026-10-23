// Include the actual HLE implementation so its private scratch buffer/backend can be supplied without adding
// test hooks to the runtime. Entry-point bodies keep their production ABI readers/return logic; test-local
// visibility lets the compiler discard unrelated window/device entry points and their OS dependencies.
#include "xdk/hle.h"
#undef X86_FUNC
#define X86_FUNC(name) static void name(X86Context& __restrict ctx, uint8_t* base)
#include "../runtime/xdk/d3d8.cpp"
#include <cassert>

namespace fixture {
std::vector<uint8_t> memory(0x1000000);
uint32_t next_system = 0x20000;
constexpr uint32_t stack = 0x10000, scratch = 0x600000;
struct Backend final : gpu::Backend {
    gpu::State last;
    gpu::Assembled assembled;
    std::vector<uint8_t> bytes;
    int draws = 0;
    bool Init(uint32_t, uint32_t) override { return true; }
    void Draw(const gpu::State& state, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices) override {
        last = state; ++draws;
        assert(gpu::Assemble(state, prim, start, count, indices, assembled));
        auto buffer = assembled.buffers[0]; bytes.assign(buffer.src, buffer.src + buffer.bytes);
    }
    void Clear(const gpu::State&, uint32_t, const uint8_t*, uint32_t, uint32_t, float, uint32_t) override {}
    void Copy(const gpu::Surface&, const gpu::Surface&, uint32_t, uint32_t, uint32_t) override {}
    void Present(const gpu::Surface&) override {}
    void Sync(uint32_t, uint32_t) override {}
    void SyncLate(uint32_t) override {}
    void Flush() override {}
    void SetGamma(const uint8_t*) override {}
};
void Call(X86Func* fn, std::initializer_list<uint32_t> args) {
    X86Context ctx{}; ctx.esp = stack;
    uint32_t at = stack + 4;
    for (auto arg : args) { kernel::W32(at, arg); at += 4; }
    fn(ctx, memory.data()); assert(ctx.esp == at);
}
uint32_t Bits(float f) { uint32_t b; std::memcpy(&b, &f, 4); return b; }
void Commands(const std::vector<uint32_t>& words) {
    d3d::g_scratch_push = scratch; d3d::g_scratch_size = 0x10000; d3d::g_walk = scratch;
    std::memcpy(memory.data() + scratch, words.data(), words.size() * 4);
    d3d::SetDev(d3d::kDevPush, scratch + uint32_t(words.size()) * 4);
}
}
namespace kernel { uint8_t* g_base = fixture::memory.data(); }
namespace logx { void Write(Level, const char*, ...) {} }
namespace mem {
uint32_t AllocSystem(uint32_t n, uint32_t align) {
    auto& at = fixture::next_system; at = (at + align - 1) & ~(align - 1); auto result = at; at += n; return result;
}
uint32_t AllocContiguous(uint32_t n, uint32_t, uint32_t, uint32_t) { assert(n <= 0x400000); return 0x800000; }
uint32_t PhysicalFromVirtual(uint32_t va) { return va; }
uint32_t VirtualFromPhysical(uint32_t pa) { return pa; }
}

int main() {
    using namespace fixture;
    auto backend = std::make_unique<Backend>(); auto* observer = backend.get(); d3d::g_backend = std::move(backend);
    auto original = std::make_shared<gpu::VertexShader>();
    original->handle = 0x20001; original->program_type = 0x2078;
    original->program = {10, 11, 12, 0, 20, 21, 22, 1};
    original->inputs[0] = {0, 0, 0x42}; original->stream_stride[0] = 16;
    auto declaration = std::make_shared<gpu::VertexShader>();
    declaration->handle = 0x20011; declaration->inputs = original->inputs; declaration->stream_stride[0] = 16;
    d3d::g_vs[original->handle] = original; d3d::g_vs[declaration->handle] = declaration;
    Call(sub_004151C0, {original->handle, 8});
    Call(sub_00415220, {declaration->handle, 8});
    auto selected = d3d::State().vs;
    assert(selected->program == original->program && selected->inputs[0].type == 0x42);
    uint32_t changed[] = {30, 31, 32, 1};
    d3d::WriteSlots(9, changed, 1); d3d::SyncProgram();
    assert(d3d::State().vs != selected && d3d::State().vs->program[4] == 30);
    assert(selected->program[4] == 20); // an earlier queued draw's program remains immutable
    Call(sub_00415220, {0, 9});
    assert(d3d::State().vs->program.size() == 4 && d3d::State().vs->inputs[0].type == 0x42);
    Call(sub_00415530, {original->handle});
    assert(d3d::State().vs == original);

    // Older inline writes must execute before a later HLE constant setter.
    Commands({0x00041EA4, 105, 0x00100B80, Bits(1), Bits(1), Bits(1), Bits(1)});
    for (int i = 0; i < 4; ++i) kernel::W32(0x30000 + i * 4, Bits(4));
    X86Context ctx{}; ctx.esp = stack; ctx.ecx = 105; ctx.edx = 0x30000;
    sub_00414F10(ctx, memory.data());
    for (auto value : d3d::State().vsc[105]) assert(value == 4);
    assert(d3d::g_walk == d3d::Dev(d3d::kDevPush));

    // Immediate screen-space positions retain their RHW, and a constant color outside BEGIN is visible at END.
    Call(sub_00415530, {4}); // D3DFVF_XYZRHW
    Commands({0x0004194C, 0xFF0000FF, 0x000417FC, 1, 0x00101A00, Bits(100), Bits(200), Bits(.5f), Bits(1), 0x000417FC, 0});
    d3d::Walk(d3d::Dev(d3d::kDevPush));
    assert(observer->draws == 1 && observer->assembled.pretransformed);
    assert(observer->last.vattr[3][0] == 1 && observer->last.vattr[3][1] == 0 && observer->last.vattr[3][3] == 1);
    float position[4]; std::memcpy(position, observer->bytes.data(), 16);
    assert(position[0] == 100 && position[1] == 200 && position[3] == 1);
    // A per-vertex color missing from the bound FVF is added to the packed declaration in BGRA order.
    Commands({0x000417FC, 1, 0x0004194C, 0xFFFF0000, 0x00101A00, Bits(10), Bits(20), Bits(.25f), Bits(1), 0x000417FC, 0});
    d3d::Walk(d3d::Dev(d3d::kDevPush));
    assert(observer->draws == 2 && observer->assembled.pretransformed && (observer->last.vs_handle & 0x40));
    assert(observer->bytes.size() == 20 && observer->bytes[16] == 255 && observer->bytes[18] == 0 && observer->bytes[19] == 255);
}
