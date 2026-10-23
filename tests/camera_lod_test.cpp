// Exercise the actual camera/LOD hooks without running the game or its renderer.
#include "xdk/hle.h"
#include <array>
#include <cassert>
#include <cmath>
#include <limits>
#include <vector>

namespace fixture {
std::vector<uint8_t> memory(0x600000);
std::array<bool, 0x600> committed;
constexpr uint32_t stack = 0x10000, prop = 0x20000, manager = 0x30000, output = 0x40000;
float fov_seen = 0, far_seen = 0, prefetch_seen = 0, range_seen = 0;
bool change_range = false;
void Float(uint32_t va, float f) { std::memcpy(memory.data() + va, &f, 4); }
float Float(uint32_t va) { float f; std::memcpy(&f, memory.data() + va, 4); return f; }
uint32_t Bits(float f) { uint32_t b; std::memcpy(&b, &f, 4); return b; }
bool Near(float a, float b) { return std::abs(a - b) < 1e-4f; }
X86Context Call(X86Func* hook, uint32_t self, uint32_t ret, std::initializer_list<uint32_t> args) {
    X86Context ctx{}; ctx.ecx = self; ctx.esp = stack; kernel::W32(stack, ret);
    uint32_t at = stack + 4; for (auto a : args) { kernel::W32(at, a); at += 4; }
    hook(ctx, memory.data()); assert(ctx.esp == at); return ctx;
}
}
namespace kernel { uint8_t* g_base = fixture::memory.data(); }
namespace mem { bool IsCommitted(uint32_t va) { return va < fixture::memory.size() && fixture::committed[va / kPage]; } }
namespace logx { void Write(Level, const char*, ...) {} }
extern float g_lod_scale, g_fov_scale;

X86_FUNC(__imp__sub_002D45C0) {
    if (auto out = kernel::R32(ctx.esp + 0x14)) fixture::Float(out, 10);
    ctx.eax = 1; ctx.esp += 24;
}
X86_FUNC(__imp__sub_000E9260) { fixture::prefetch_seen = fixture::Float(ctx.esp + 8); ctx.esp += 12; }
X86_FUNC(__imp__sub_0019D980) { ctx.ecx = 0; ctx.esp += 12; }
X86_FUNC(__imp__sub_0019BF50) {
    fixture::range_seen = fixture::Float(ctx.ecx + 0x303C);
    if (fixture::change_range) fixture::Float(ctx.ecx + 0x303C, 600);
    ctx.esp += 4;
}
X86_FUNC(__imp__sub_0019A630) { ctx.esp += 8; }
X86_FUNC(__imp__sub_002C3F70) { fixture::far_seen = fixture::Float(ctx.esp + 4); ctx.esp += 8; }
X86_FUNC(__imp__sub_002D4AE0) { fixture::fov_seen = fixture::Float(ctx.esp + 4); ctx.esp += 20; }
#define DECLARE(name) X86_EXTERN_FUNC(sub_##name)
DECLARE(002D45C0); DECLARE(000E9260); DECLARE(0019D980); DECLARE(0019BF50);
DECLARE(0019A630); DECLARE(002C3F70); DECLARE(002D4AE0);

int main() {
    using namespace fixture;
    committed.fill(true);
    auto sphere = [&](uint32_t ret) { Call(sub_002D45C0, 0, ret, {Bits(0), Bits(0), Bits(10), Bits(2), output}); return Float(output); };
    g_lod_scale = 2;
    assert(Near(sphere(0x0E6342), 6)); // radius + (view depth - radius) / multiplier
    assert(Near(sphere(0x123456), 10)); // unrelated visibility tests retain original depth
    g_lod_scale = 0.5f; assert(Near(sphere(0x0E6342), 18));
    g_lod_scale = std::numeric_limits<float>::quiet_NaN(); assert(Near(sphere(0x0E6342), 10));
    g_lod_scale = 2;
    Call(sub_000E9260, 0, 0, {0, Bits(100)}); assert(Near(prefetch_seen, 50));
    Call(sub_002C3F70, 0, 0, {Bits(2000)}); assert(Near(far_seen, 4000));
    g_lod_scale = 4; Call(sub_002C3F70, 0, 0, {Bits(2000)}); assert(Near(far_seen, 4000)); // far-clip cap

    Float(prop + 0x9C, 100); Float(prop + 0xA0, 2); kernel::W32(prop + 0x94, 7);
    Call(sub_0019D980, prop, 0, {0, 0}); Float(manager + 0x303C, 200);
    g_lod_scale = 2; Call(sub_0019BF50, manager, 0, {});
    assert(Near(Float(prop + 0x9C), 200) && Near(range_seen, 400) && Near(Float(manager + 0x303C), 200));
    g_lod_scale = .5f; Call(sub_0019BF50, manager, 0, {});
    assert(Near(Float(prop + 0x9C), 50) && Near(range_seen, 100)); // no compounding across frames
    change_range = true; g_lod_scale = 2; Call(sub_0019BF50, manager, 0, {});
    assert(Near(Float(manager + 0x303C), 600)); change_range = false; // preserve a new game-written range
    committed[prop / mem::kPage] = false; Call(sub_0019BF50, manager, 0, {});
    committed[prop / mem::kPage] = true; g_lod_scale = 3; Call(sub_0019BF50, manager, 0, {});
    assert(Near(Float(prop + 0x9C), 200)); // unavailable entries do not revive when an address is reused

    auto camera = [&](uint32_t caller, float fov) { Call(sub_002D4AE0, 0, caller, {Bits(fov), Bits(1.333f), Bits(.25f), Bits(2000)}); return fov_seen; };
    g_fov_scale = 1.2f; assert(Near(camera(0x13853A, 60), 72));
    assert(Near(camera(0x0DAF50, 60), 60)); // mirror/HUD and reapplication callers are not rescaled
    g_fov_scale = 100; assert(Near(camera(0x13853A, 100), 150));
    g_fov_scale = std::numeric_limits<float>::quiet_NaN(); assert(Near(camera(0x13853A, 60), 60));
    g_fov_scale = 1; assert(Near(camera(0x13853A, 60), 60));
    Call(sub_0019A630, manager, 0, {0});
}
