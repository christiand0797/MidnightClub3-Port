// Exercise the actual title hooks with only the guest clock entry points and backing memory replaced.
#include "app/app.h"
#include "kernel/kernel.h"

#include <cassert>
#include <cmath>
#include <vector>

namespace kernel { uint8_t* g_base = nullptr; }
namespace app { Settings& Get() { static Settings s; return s; } }
namespace logx { void Write(Level, const char*, ...) {} }
void ApplyVehicleTuning() {}
float g_lod_scale = 1, g_fov_scale = 1;
// Keep frame times deterministic while running the production hook.
uint64_t g_test_ticks = 73333333300ull;
uint64_t x86_rdtsc() { return g_test_ticks; }

namespace {
constexpr uint32_t kRate = 0x4B2D54, kStep = 0x4B2D58, kSubsteps = 0x4B2D64;
constexpr uint32_t kFrame = 0x4B2D70, kFixed = 0x534770, kPrevTick = 0x534778;
void WriteFloat(uint32_t va, float v) { std::memcpy(kernel::g_base + va, &v, 4); }
float ReadFloat(uint32_t va) { float v; std::memcpy(&v, kernel::g_base + va, 4); return v; }
void Near(float a, float b) { assert(std::abs(a - b) < 1e-6f); }
}

X86_FUNC(__imp__sub_0020BB00) {
    const float rate = ReadFloat(ctx.esp + 4);
    const uint32_t substeps = kernel::R32(ctx.esp + 8);
    WriteFloat(kRate, rate);
    kernel::W32(kSubsteps, substeps);
    kernel::W32(kFixed, 1);
    WriteFloat(kStep, float(1.0 / (double(rate) * substeps)));
    ctx.esp += 4;
}
X86_FUNC(__imp__sub_0020B6F0) {
    WriteFloat(kFrame, ReadFloat(kStep));
    kernel::W32(kPrevTick, uint32_t(x86_rdtsc()));
    ctx.esp += 4;
}
X86_FUNC(__imp__sub_000EA750) { ctx.esp += 8; }
X86_EXTERN_FUNC(sub_0020BB00);
X86_EXTERN_FUNC(sub_0020B6F0);

int main() {
    std::vector<uint8_t> memory(0x540000);
    kernel::g_base = memory.data();
    X86Context ctx{};
    auto request = [&](float rate, uint32_t substeps) {
        ctx.esp = 0x10000;
        WriteFloat(ctx.esp + 4, rate);
        kernel::W32(ctx.esp + 8, substeps);
        sub_0020BB00(ctx, memory.data());
        assert(ctx.esp == 0x10004);
        Near(ReadFloat(0x10004), rate); // preserve the caller's original cdecl argument
    };
    auto frame = [&](double elapsed) {
        ctx.esp = 0x11000;
        kernel::W32(kPrevTick, uint32_t(x86_rdtsc()) - uint32_t(elapsed * 733333333.0));
        sub_0020B6F0(ctx, memory.data());
        assert(ctx.esp == 0x11004);
        return ReadFloat(kFrame);
    };

    app::Get().graphics.sixty = true;
    app::Get().graphics.lod = 2;
    app::Get().graphics.fov_scale = 1.2f;
    request(30, 1);
    Near(ReadFloat(kRate), 60);
    Near(frame(1.0 / 60), 1.0f / 60);
    Near(g_lod_scale, 2);
    Near(g_fov_scale, 1.2f);
    Near(frame(2.0 / 60), 2.0f / 60); // a missed refresh advances the race by two fields
    Near(frame(5), 1.0f / 60);        // loading/pause time does not jump the simulation
    app::Get().graphics.sixty = false;
    Near(frame(1.0 / 30), 1.0f / 30); // changing modes during the same race restores console speed
    for (int i = 0; i < 300; ++i) Near(frame(1.0 / 30), 1.0f / 30);
    app::Get().graphics.sixty = true;
    Near(frame(1.0 / 60), 1.0f / 60);
    app::Get().graphics.sixty = false;
    request(20, 2);
    Near(frame(1.0 / 40), 1.0f / 40); // preserve the game's requested substep count
    request(30, 1);
    Near(frame(1.0 / 30), 1.0f / 30); // a later request replaces the original step
}
