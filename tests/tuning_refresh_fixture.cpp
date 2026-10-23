// Run the original update/cache instructions with the production PC tuning hooks. Unused generated functions
// have local linkage and disappear under optimization; unexpected game systems fail instead of being simulated.
#include "app/app.h"
#include "core/log.h"
#include "kernel/memory.h"
#include "cpu/x86_ops.h"
#include <array>
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <vector>

#undef X86_FUNC
#undef X86_WEAK_FUNC
#define X86_FUNC(name) static void name(X86Context& __restrict ctx, uint8_t* base)
#define X86_WEAK_FUNC(name, impl)
#define __imp__sub_000FFD90 OriginalCarUpdate
#define __imp__sub_000F32A0 OriginalRenderApply
#define __imp__sub_000A7250 UnusedOriginalWheelSolver
#define __imp__sub_000A3CA0 UnusedOriginalSimParser
#define __imp__sub_000A65D0 UnusedOriginalWheelDisplacement
#define __imp__sub_000FB760 UnusedOriginalModelParser
#define __imp__sub_00103C50 UnusedOriginalUpgradeInterpolation
#include "x86_recomp.17.cpp"
#include "x86_recomp.18.cpp"
#include "x86_recomp.26.cpp"
#include "x86_recomp.27.cpp"
#include "x86_recomp.28.cpp"
#include "x86_recomp.32.cpp"
#include "x86_recomp.72.cpp"
#define __imp__sub_001284F0 OriginalAirbagMove
#include "x86_recomp.33.cpp"
#undef __imp__sub_001284F0
#undef __imp__sub_000FFD90
#undef __imp__sub_000F32A0
#undef __imp__sub_000A7250
#undef __imp__sub_000A3CA0
#undef __imp__sub_000A65D0
#undef __imp__sub_000FB760
#undef __imp__sub_00103C50

namespace {
std::vector<uint8_t> guest(0x600000);
constexpr uint32_t kCar = 0x10000, kSim = 0x12000, kCfg = 0x14000, kRender = 0x16000;
constexpr uint32_t kFront = 0x20000, kRear = 0x22000, kEngine = 0x24000;
constexpr uint32_t kBody = 0x26000, kFactory = 0x28000, kResources = 0x30000, kModel = 0x36000;
constexpr uint32_t kManager = 0x38000, kInfoF = 0x3A000, kInfoR = 0x3B000, kGeometry = 0x3C000;
constexpr uint32_t kAxleF = 0x40000, kAxleR = 0x41000, kLinks = 0x42000;
constexpr std::array<uint32_t, 4> kWheels = {0x44000, 0x45000, 0x46000, 0x47000};
constexpr uint32_t kVtable = 0x49000, kGlobals = 0x4A000, kStack = 0x70000;
unsigned cache_updates = 0, cfg_updates = 0, axle_updates = 0, upgrade_updates = 0, body_updates = 0;
float body_clearance = 0;

void F(uint32_t p, float v) { std::memcpy(guest.data() + p, &v, 4); }
float F(uint32_t p) { float v; std::memcpy(&v, guest.data() + p, 4); return v; }
bool Near(float a, float b) { return std::fabs(a - b) < 0.00001f; }
void W(uint32_t p, uint32_t v) { std::memcpy(guest.data() + p, &v, 4); }
void Unexpected(const char* name) { std::cerr << "unexpected generated dependency " << name << '\n'; std::abort(); }

void Invoke(X86Func* fn, uint32_t self, std::initializer_list<uint32_t> args = {}) {
    X86Context ctx{};
    ctx.ecx = self;
    ctx.esp = kStack;
    unsigned i = 1;
    for (uint32_t v : args) W(kStack + 4 * i++, v);
    fn(ctx, guest.data());
    assert(ctx.esp == kStack + 4 * i);
    assert(ctx.top == 0); // Every native float getter must balance its x87 result.
}
}

namespace kernel { uint8_t* g_base = guest.data(); }
namespace mem { bool IsCommitted(uint32_t va) { return va < guest.size(); } }
namespace logx { void Write(Level, const char*, ...) {} }
namespace app {
Settings& Get() { static Settings settings; return settings; }
std::recursive_mutex& UiLock() { static std::recursive_mutex lock; return lock; }
}
void ApplyVehicleTuning();
void TuningBindCar(uint32_t, uint32_t, uint32_t, bool);
void TuningSetPlayerSim(uint32_t, bool);
void TuningForgetSim(uint32_t);
void TuningCarCfgApplied(uint32_t);
// garage_test covers recovery from the concrete human controller. This fixture explicitly binds that car so
// it can compare the native cache behavior before and after ownership without simulating another factory.
void GarageRefreshPlayerCar(uint32_t) {}

extern "C" void __imp__sub_000FFD90(X86Context& c, uint8_t* b) { OriginalCarUpdate(c, b); }
extern "C" void __imp__sub_000F32A0(X86Context& c, uint8_t* b) { OriginalRenderApply(c, b); }
extern "C" void sub_000FF220(X86Context& c, uint8_t* b) { ++axle_updates; __imp__sub_000FF220(c, b); }
extern "C" void sub_00105450(X86Context& c, uint8_t* b) { ++cache_updates; __imp__sub_00105450(c, b); }
extern "C" void sub_00121970(X86Context& c, uint8_t* b) {
    const uint32_t sim = c.ecx;
    ++cfg_updates;
    __imp__sub_00121970(c, b);
    TuningCarCfgApplied(sim); // The garage.cpp post-hook, at the actual native overwrite boundary.
}
#define REAL(name) extern "C" void sub_##name(X86Context& c, uint8_t* b) { __imp__sub_##name(c, b); }
REAL(000A6560)
REAL(000AE890)
REAL(000AEEA0)
REAL(000FF200)
REAL(000F11E0)
REAL(00270BE0)
REAL(00272FC0)
REAL(00272FE0)
REAL(00270B40)
REAL(00270BD0)
REAL(00270C00)
#undef REAL

// The geometry provider and performance interpolation are fixture boundaries, not replacements for refresh.
extern "C" void sub_000FB4E0(X86Context& c, uint8_t*) {
    const float width = F(c.esp + 4);
    x87_push(c, width < 0.25f ? 0.8 : 0.9);
    c.esp += 16;
}
extern "C" void sub_00103A50(X86Context& c, uint8_t*) { c.esp += 12; }
extern "C" void __imp__sub_00103C50(X86Context& c, uint8_t*) {
    ++upgrade_updates;
    F(kFront + 0x58, 100); F(kRear + 0x58, 110);
    c.esp += 12;
}
extern "C" void sub_00120A80(X86Context& c, uint8_t*) { c.eax = kSim; c.esp += 4; }
extern "C" void sub_00121440(X86Context& c, uint8_t*) { body_clearance = F(c.esp + 4); c.esp += 8; }

void x86_icall(X86Context& c, uint8_t*, uint32_t address) {
    assert(c.ecx == kBody);
    if (address == 0x100) c.eax = 1; // Body virtual +28: active body.
    else if (address == 0x104) c.eax = 0; // Body virtual +2C: no reset transition this frame.
    else if (address == 0x108) ++body_updates; // Body virtual +5C: apply revised bounds.
    else Unexpected("virtual body call");
    c.esp += 4;
}

#define UNUSED(name) extern "C" void sub_##name(X86Context&, uint8_t*) { Unexpected(#name); }
UNUSED(00033280) UNUSED(000A2460) UNUSED(000A5890) UNUSED(000A94F0) UNUSED(000FF3D0)
UNUSED(00100001) UNUSED(00101300) UNUSED(001013A0) UNUSED(00111010) UNUSED(00120AA0)
UNUSED(00120DF0) UNUSED(00120EE0) UNUSED(00122A40) UNUSED(00124210) UNUSED(001272E0)
UNUSED(001274B0) UNUSED(00127760) UNUSED(001277C0) UNUSED(00128030) UNUSED(00128390)
UNUSED(001284C0) UNUSED(00128620) UNUSED(00128BE0) UNUSED(00154640) UNUSED(001E2D40)
UNUSED(002694A0) UNUSED(0026D7E0) UNUSED(00273B60) UNUSED(002BD910) UNUSED(002BF270)
UNUSED(002BF840) UNUSED(002C6200) UNUSED(002CAF30) UNUSED(003422F0) UNUSED(0035DEC0)
UNUSED(000B64E0) UNUSED(000F24A0) UNUSED(0013D300) UNUSED(001452B0) UNUSED(00270AF0)
UNUSED(00273020) UNUSED(002BF570)
#undef UNUSED
#define UNUSED_IMPORT(name) extern "C" void __imp__sub_##name(X86Context&, uint8_t*) { Unexpected(#name); }
UNUSED_IMPORT(000A3CA0) UNUSED_IMPORT(000FB760) UNUSED_IMPORT(0032F490) UNUSED_IMPORT(0032F840)
UNUSED_IMPORT(000A7250) UNUSED_IMPORT(000A65D0)
#undef UNUSED_IMPORT
extern "C" void __imp__sub_001284F0(X86Context& c, uint8_t* b) { OriginalAirbagMove(c, b); }

int main() {
    W(kCar + 8, kBody); W(kCar + 0xC, kManager); W(kCar + 0x4C, kFactory);
    W(kCar + 0x50, kSim); W(kCar + 0x64, kCfg); W(kCar + 0x68, kRender);
    guest[kCar + 0x5E] = 1; // Non-profile preview: skip unrelated profile/ability systems.
    W(kFactory + 8, kResources); W(kManager + 0x2F0, kRender);
    W(kSim + 0x54, kFront); W(kSim + 0x58, kRear); W(kSim + 0x5C, kEngine);
    F(kSim + 0x4C, 1); F(kSim + 0x1C, 0.5f);
    W(kBody, kVtable); W(kVtable + 0x28, 0x100); W(kVtable + 0x2C, 0x104); W(kVtable + 0x5C, 0x108);
    W(0x557EFC, kGlobals);
    F(0x447C18, 0.5f); F(0x447C1C, 1); F(0x4499F0, 0.01f); F(0x447E68, 0.5f);
    F(0x454868, 0.3f); F(0x449004, 2);
    F(0x471A90, -0.1f); F(0x471A94, -0.2f); // Original saved ride-height table entries.
    F(0x4B3918, 0.2f); F(0x4B391C, 0.3f); // Width table.
    F(0x4B3910, 0.6f); F(0x4B3914, 0.7f); // Diameter table.
    F(0x471958, 1); // Profile divisor (profile zero).
    guest[kCfg + 0x17] = 1; guest[kCfg + 0x19] = 1; guest[kCfg + 0x1B] = 1;
    guest[kCfg + 0xE1] = 0xFF; // Exercise the purchased-performance post-hook too.
    F(kCfg + 0xC, -0.03f); F(kCfg + 0x10, -0.04f);
    W(kRender + 0x1800, kModel); W(kModel + 0x18, kResources);
    W(kResources + 0x3818, kInfoF); W(kResources + 0x381C, kInfoR);
    W(kResources + 0xDC, kGeometry); W(kGeometry + 8, kGeometry + 0x100);
    F(kInfoF + 0x14, 0.15f); F(kInfoR + 0x14, 0.15f);
    F(kRender + 0x12D0, 0.2f); F(kRender + 0x12C8, 0.3f);
    F(kRender + 0x12D4, 0.6f); F(kRender + 0x12CC, 0.7f);
    F(kRender + 0x12D8, 0.8f); F(kRender + 0x12DC, 0.9f);
    W(kBody + 0x28, 2); W(kBody + 0x2C, kLinks); W(kLinks, kAxleF); W(kLinks + 4, kAxleR);
    F(kAxleF + 0x50, -2); F(kAxleR + 0x50, 3);
    const std::array<uint32_t, 4> slots = {0xD8, 0xDC, 0xE0, 0xE4};
    for (unsigned i = 0; i < kWheels.size(); ++i) {
        W(kBody + slots[i], kWheels[i]);
        F(kWheels[i] + 0x10C, i % 2 ? -0.2f : -0.1f);
        F(kWheels[i] + 0x144, i % 2 ? -0.18f : -0.08f);
    }
    for (uint32_t tune : {kFront, kRear}) {
        F(tune + 0x40, 0.4f); F(tune + 0x44, 0.4f); F(tune + 0x4C, 0.4f); F(tune + 0x50, 0.4f);
        F(tune + 0x58, tune == kFront ? 100 : 110);
        F(tune + 0x5C, 10); F(tune + 0x34, 20); F(tune + 0x68, 1); F(tune + 0x6C, 1);
    }
    F(kEngine + 4, 200);
    auto& s = app::Get().tuning;
    s.ride_front = 0.12f; s.ride_rear = -0.08f;
    s.offset_front = 0.05f; s.offset_rear = 0.07f;
    s.camber_front = -0.1f; s.camber_rear = -0.2f; s.spring_front = 1.5f;

    // Unregistered previews reproduce the ownership symptom: changing F1 does not request a cache refresh.
    Invoke(sub_000FFD90, kCar);
    assert(cache_updates == 0 && Near(F(kWheels[0] + 0x10C), -0.1f));
    assert(Near(F(kFront + 0x48), -0.1f));

    TuningSetPlayerSim(kSim, true);
    TuningBindCar(kCar, kSim, kCfg, true);
    TuningCarCfgApplied(kSim);
    ApplyVehicleTuning();
    const auto saved_cfg = std::vector<uint8_t>(guest.begin() + kCfg + 0xC, guest.begin() + kCfg + 0x24);
    Invoke(sub_000FFD90, kCar);
    assert(cache_updates == 1 && upgrade_updates == 1 && body_updates == 1);
    assert(Near(F(kFront + 0x48), 0.02f) && Near(F(kRear + 0x48), -0.28f));
    assert(Near(F(kWheels[0] + 0x10C), 0.02f) && Near(F(kWheels[1] + 0x10C), -0.28f));
    assert(Near(F(kWheels[0] + 0x144), 0.04f) && Near(F(kWheels[1] + 0x144), -0.26f));
    assert(Near(F(kWheels[2] + 0x10C), 0.02f) && Near(F(kWheels[3] + 0x10C), -0.28f));
    assert(Near(F(kWheels[0] + 0x100), 0.3f) && Near(F(kWheels[1] + 0x104), 0.3f));
    assert(Near(F(kFront + 0x58), 150));

    // The generated renderer rebuild overwrites camber/hubs. Its production post-hook reapplies the PC values.
    W(kCfg + 8, 1);
    Invoke(sub_000F32A0, kRender, {kCfg, 0, 0});
    assert(Near(F(kRender + 0x12E8), -0.13f) && Near(F(kRender + 0x12EC), -0.24f));
    assert(Near(F(kRender + 0x12D8), 0.75f) && Near(F(kRender + 0x12DC), 0.83f));

    // Changing only ride height reaches native wheel caches on the next update, without another garage selection.
    W(kCfg + 8, 0); s.ride_front = -0.25f;
    ApplyVehicleTuning();
    Invoke(sub_000FFD90, kCar);
    assert(cache_updates == 2 && Near(F(kWheels[0] + 0x10C), -0.35f));
    assert(Near(F(kWheels[0] + 0x144), -0.33f));
    assert(Near(F(kAxleF + 0x4C), 2 * (0.75f - 0.11f) + 1));
    assert(Near(F(kAxleR + 0x4C), 3 * (0.83f - 0.16f) + 1));
    const float compression = F(kWheels[0] + 0x144), clearance = body_clearance;
    for (unsigned i = 0; i < 10; ++i) { ApplyVehicleTuning(); Invoke(sub_000FFD90, kCar); }
    assert(cache_updates == 2 && upgrade_updates == 2 && body_updates == 2);
    assert(Near(F(kWheels[0] + 0x144), compression) && Near(body_clearance, clearance));
    assert(Near(F(kFront + 0x58), 150)); // No cumulative multiplier or full body reset.
    assert(std::memcmp(guest.data() + kCfg + 0xC, saved_cfg.data(), saved_cfg.size()) == 0);

    // Leaving the garage and reconstructing a sim at reused addresses must discard the old tuning baselines.
    TuningForgetSim(kSim);
    F(kFront + 0x48, -0.1f); F(kRear + 0x48, -0.2f);
    TuningSetPlayerSim(kSim, true); TuningBindCar(kCar, kSim, kCfg, true);
    TuningCarCfgApplied(kSim); W(kCfg + 8, 0);
    Invoke(sub_000FFD90, kCar);
    assert(cache_updates == 3 && Near(F(kWheels[0] + 0x10C), -0.35f));

    // The original airbag caller supplies distinct axle baselines to the production matching hook. An already
    // deflated rear bag must follow a changed rear baseline on this update, even when all four targets matched.
    constexpr uint32_t airbag = 0x4C000, airbag_cfg = 0x4D000;
    W(airbag + 8, kCar); W(airbag + 4, airbag_cfg);
    F(kFront + 0x48, 0.270f); F(kRear + 0x48, 0.280f);
    F(airbag_cfg + 0x10, 1); F(airbag_cfg + 0x14, 1); F(0x4B2D48, 1.0f / 60);
    s.airbag_match_ride = true;
    for (unsigned i = 0; i < 4; ++i) {
        F(airbag + 0x20 + i * 4, 0.270f);
        W(airbag + 0x10 + i * 4, 1);
    }
    Invoke(__imp__sub_001288C0, airbag);
    Invoke(__imp__sub_00128620, airbag);
    assert(Near(F(airbag + 0x20), 0.270f) && Near(F(airbag + 0x24), 0.270f));
    assert(Near(F(airbag + 0x28), 0.280f) && Near(F(airbag + 0x2C), 0.280f));
    assert(Near(F(kWheels[0] + 0x10C), 0.270f) && Near(F(kWheels[1] + 0x10C), 0.280f));
    assert(Near(F(kWheels[2] + 0x10C), 0.270f) && Near(F(kWheels[3] + 0x10C), 0.280f));
    std::cout << "actual FFD90 / 121970 / F32A0 / FF220 / 105450 / A6560 refresh: PASS\n";
}
