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
uint32_t original_light_result = 0, original_enable_result = 0;
unsigned original_set_calls = 0, original_enable_calls = 0;
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
extern "C" void __imp__sub_00411FB0(X86Context& ctx, uint8_t* base) {
    assert(base==fixture::memory.data());++fixture::original_set_calls;
    kernel::Return(ctx,fixture::original_light_result,2);
}
extern "C" void __imp__sub_00412270(X86Context& ctx, uint8_t* base) {
    assert(base==fixture::memory.data());++fixture::original_enable_calls;
    // Verify the real wrapper established nesting BEFORE the original could synthesize a default.
    assert(d3d::g_state.pbr_light_proof.Snapshot(d3d::g_state.light_enable).pending_enable_depth==1);
    kernel::Return(ctx,fixture::original_enable_result,2);
}
namespace mem {
bool IsCommitted(uint32_t va) { return va < fixture::memory.size(); }
uint32_t AllocSystem(uint32_t n, uint32_t align) {
    auto& at = fixture::next_system; at = (at + align - 1) & ~(align - 1); auto result = at; at += n; return result;
}
uint32_t AllocContiguous(uint32_t n, uint32_t, uint32_t, uint32_t) { assert(n <= 0x400000); return 0x800000; }
std::vector<uint32_t> freed;
void FreeSystem(uint32_t va) { freed.push_back(va); }
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
    // Real pending push packets take the current scoped metadata at parsing time, not at later restoration.
    Commands({0x000417FC, 1, 0x00101A00, Bits(1), Bits(2), Bits(.5f), Bits(1), 0x000417FC, 0});
    d3d::State().tire_fit.source_ratio=.83f;d3d::State().tire_fit.target_ratio=.975f;
    assert(observer->draws==2);
    d3d::FlushPendingCommands();
    assert(observer->draws==3&&observer->last.tire_fit.target_ratio==.975f);
    d3d::State().tire_fit={};d3d::FlushPendingCommands();
    assert(observer->draws==3&&d3d::g_walk==d3d::Dev(d3d::kDevPush));

    // Pixel shaders are XDK objects (D3DDevice_CreatePixelShader 0x415A90): reference count 1, owned, definition
    // pointer at +8 to its copy at +0xC. SetPixelShader keeps CDevice_PixelShader current, because the XDK's
    // state-block capture copies that field (adding a reference): D3DX records every pass's inline PixelShader that
    // way, and with the field left 0 each replay set no shader (car paint lost its vinyl and reflection).
    constexpr uint32_t def = 0x31000, out = 0x32000;
    for (uint32_t i = 0; i < 0xF0; i += 4) kernel::W32(def + i, 0xC0DE0000 + i);
    Call(sub_00415A90, {def, out});
    const uint32_t shader = kernel::R32(out);
    assert(kernel::R32(shader) == 1 && kernel::R32(shader + 4) == 1 && kernel::R32(shader + 8) == shader + 0xC);
    assert(std::memcmp(memory.data() + shader + 0xC, memory.data() + def, 0xF0) == 0);
    Call(sub_00415B00, {shader});
    assert(d3d::Dev(XDK_CDevice_PixelShader) == shader && d3d::State().ps && d3d::State().ps->handle == shader);
    assert(d3d::State().ps->def[5] == 0xC0DE0014);
    kernel::W32(shader, kernel::R32(shader) + 1);  // a captured state block's reference (0x416A63 inc [eax])
    Call(sub_00415AE0, {shader});                  // the owner's release: the state block still holds it
    assert(kernel::R32(shader) == 1 && d3d::g_ps.count(shader) && mem::freed.empty());
    Call(sub_00415B00, {0});
    assert(d3d::Dev(XDK_CDevice_PixelShader) == 0 && !d3d::State().ps);
    Call(sub_00415B00, {shader});                  // the state block's replay
    assert(d3d::State().ps && d3d::State().ps->handle == shader);
    Call(sub_00415AE0, {shader});                  // its last reference
    assert(!d3d::g_ps.count(shader) && mem::freed == std::vector<uint32_t>{shader});

    // Exercise the actual strong wrappers, original-outcome handoff and ABI, not only the pure tracker.
    // These controlled original-boundary doubles deliberately fail/succeed; the separate source review
    // and table test bind the original XDK record layout. They do not claim native light acceptance.
    d3d::g_state.light_enable=0;d3d::g_state.pbr_light_proof.DeviceReset();
    constexpr uint32_t light=0x710000,table=0x720000;
    std::memset(memory.data()+light,0,26*4);kernel::W32(light,3);kernel::W32(light+4,Bits(1));
    original_light_result=0x8007000e;
    Call(sub_00411FB0,{0,light});assert(original_set_calls==1);
    assert(!d3d::g_state.pbr_light_proof.Snapshot(0,true).light_writes_after_success);
    assert(d3d::g_state.lights[0][1]==Bits(1)); // Legacy pre-original shadow behavior preserved after failure.
    original_light_result=0;Call(sub_00411FB0,{0,light});
    original_enable_result=0;Call(sub_00412270,{0,1});assert(original_enable_calls==1);
    const auto proof=d3d::g_state.pbr_light_proof.Snapshot(1);
    assert(proof.light_writes_after_success&&proof.light_index_coverage&&proof.stateblock_valid);
    kernel::W32(d3d::kDevicePtr,d3d::kDevice);
    kernel::W32(d3d::kDevice+0x7A4,table);kernel::W32(d3d::kDevice+0x7A8,16);
    for(unsigned i=0;i<8;++i)kernel::W32(d3d::kDevice+0x7AC+i*4,i?0:table);
    kernel::W32(d3d::kDevice+0x7CC,1);
    std::memcpy(memory.data()+table,d3d::g_state.lights[0].data(),26*4);
    kernel::W32(table+0x68,1);kernel::W32(table+0x8C,0);
    std::memcpy(memory.data()+d3d::kDevice+0xF00,d3d::g_state.material.data(),17*4);
    for(const auto [original,host]:{std::pair{0u,2u},std::pair{1u,3u},std::pair{6u,256u}})
        std::memcpy(memory.data()+d3d::kDevice+0xC60+original*64,d3d::g_state.transforms[host].data(),64);
    assert(!gpu::pbr::observation::GuestProofRequested());
    gpu::pbr::observation::RequestGuestProof(true);
    ObservePbrActiveLights();assert(d3d::g_state.pbr_active_light_records_verified);
    kernel::W32(table+4,Bits(.5f));ObservePbrActiveLights();assert(!d3d::g_state.pbr_active_light_records_verified);
    kernel::W32(table+4,Bits(1));kernel::W32(d3d::kDevice+0xF00,kernel::R32(d3d::kDevice+0xF00)^1);
    ObservePbrActiveLights();assert(!d3d::g_state.pbr_active_light_records_verified);
    original_enable_result=0x8007000e;Call(sub_00412270,{0,0});
    assert(!d3d::g_state.pbr_light_proof.Snapshot(d3d::g_state.light_enable,true).light_writes_after_success);
    gpu::pbr::observation::RequestGuestProof(false);
}
