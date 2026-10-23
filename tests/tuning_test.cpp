// Exercise the actual game hooks with a small guest address space and parser bodies supplied by this test.
#include "app/app.h"
#include "core/log.h"
#include "xdk/hle.h"

#include <array>
#include <cassert>
#include <cmath>
#include <cstring>
#include <functional>
#include <limits>
#include <vector>

namespace {
std::vector<uint8_t> guest(0x600000);
std::array<bool, 0x600> committed;
std::function<void(uint32_t)> parse;
constexpr uint32_t kStack = 0x70000;
constexpr uint32_t kSim = 0x10000, kFront = 0x12000, kRear = 0x14000, kModel = 0x16000;
constexpr uint32_t kSimParser = 0x18000, kModelParser = 0x1A000;

void PutFloat(uint32_t va, float f) { std::memcpy(guest.data() + va, &f, 4); }
float Float(uint32_t va) {
    float f;
    std::memcpy(&f, guest.data() + va, 4);
    return f;
}
bool Near(float a, float b) { return std::fabs(a - b) < 0.00001f; }

void GuestReturn(X86Context& ctx) {
    ctx.ecx = 0;  // the wrapper must keep the incoming this pointer, rather than read the return register
    ctx.esp += 8;
}
}  // namespace

namespace app {
Settings& Get() {
    static Settings settings;
    return settings;
}
}  // namespace app

namespace kernel {
uint8_t* g_base = guest.data();
}  // namespace kernel
namespace logx { void Write(Level, const char*, ...) {} }

namespace mem {
bool IsCommitted(uint32_t va) { return va < guest.size() && committed[va / kPage]; }
}  // namespace mem

X86_FUNC(__imp__sub_000A3CA0) { GuestReturn(ctx); }
X86_FUNC(__imp__sub_000FB760) { GuestReturn(ctx); }
X86_FUNC(__imp__sub_0032F490) {
    if (parse) parse(ctx.ecx);
    GuestReturn(ctx);
}
X86_FUNC(__imp__sub_0032F840) { ctx.esp += 4; }
#ifndef MC3_TEST_REAL_WHEEL_SOLVER
X86_FUNC(__imp__sub_000A7250) { ctx.esp += 16; }
X86_FUNC(__imp__sub_000A65D0) { x87_push(ctx, 0); ctx.esp += 4; }
#endif

X86_EXTERN_FUNC(sub_000A3CA0);
X86_EXTERN_FUNC(sub_000FB760);
X86_EXTERN_FUNC(sub_0032F490);
X86_EXTERN_FUNC(sub_0032F840);
X86_EXTERN_FUNC(sub_000A7250);
X86_EXTERN_FUNC(sub_000A65D0);
void ApplyVehicleTuning();
void TuningSetPlayerSim(uint32_t sim, bool player);
void TuningForgetSim(uint32_t sim);
void TuningCarCfgApplied(uint32_t sim);

namespace {
using Hook = void (*)(X86Context&, uint8_t*);
void Call(Hook hook, uint32_t self, uint32_t arg = 0) {
    X86Context ctx{};
    ctx.ecx = self;
    ctx.esp = kStack;
    kernel::W32(kStack + 4, arg);
    hook(ctx, guest.data());
}

void RegisterModel(uint32_t model, uint32_t parser) { Call(sub_000FB760, model, parser); }
void ModelValues(uint32_t model, float front, float rear) {
    PutFloat(model + 0x0C, 0.8f);
    PutFloat(model + 0x14, front);
    PutFloat(model + 0x18, rear);
    PutFloat(model + 0x1C, 0.2f);
}
}  // namespace

int main() {
    committed.fill(true);
    auto& settings = app::Get().tuning;
    settings.ride_front = 0.1f;
    settings.ride_rear = -0.05f;
    settings.wheel_limit = 2;

    kernel::W32(kSim + 0x54, kFront);
    kernel::W32(kSim + 0x58, kRear);
    PutFloat(kFront + 0x40, 0.3f);
    PutFloat(kFront + 0x50, 0.4f);
    PutFloat(kRear + 0x40, 0.3f);
    PutFloat(kRear + 0x50, 0.4f);
    ModelValues(kModel, 0.5f, 0.6f);  // constructor defaults, not the values loaded from the tune file
    Call(sub_000A3CA0, kSim, kSimParser);
    RegisterModel(kModel, kModelParser);
    ApplyVehicleTuning();
    assert(Float(kFront + 0x48) == 0 && Float(kRear + 0x48) == 0);
    assert(Float(kModel + 0x14) == 0.5f);  // registration must not activate tuning before the parse

    // An unrelated parser returning cannot publish pending fields from another parser.
    parse = {};
    Call(sub_0032F490, 0x1C000);
    ApplyVehicleTuning();
    assert(Float(kModel + 0x14) == 0.5f);

    parse = [](uint32_t parser) {
        if (parser == kSimParser) {
            PutFloat(kFront + 0x48, 0.02f);
            PutFloat(kRear + 0x48, 0.03f);
        } else if (parser == kModelParser) {
            ModelValues(kModel, 0.7f, 0.8f);
        }
    };
    Call(sub_0032F490, kSimParser);
    Call(sub_0032F490, kModelParser);
    ApplyVehicleTuning();
    assert(Near(Float(kFront + 0x48), 0.02f) && Near(Float(kRear + 0x48), 0.03f)); // traffic stays stock
    TuningSetPlayerSim(kSim, true);
    ApplyVehicleTuning();
    assert(Near(Float(kFront + 0x48), 0.12f) && Near(Float(kRear + 0x48), -0.02f));
    assert(Near(Float(kModel + 0x14), 1.4f) && Near(Float(kModel + 0x18), 1.6f));

    // Repeated frames and a visitor/read of unchanged values must preserve the untuned original.
    parse = {};
    RegisterModel(kModel, kModelParser);
    Call(sub_0032F490, kModelParser);
    settings.wheel_limit = 1.5f;
    ApplyVehicleTuning();
    ApplyVehicleTuning();
    assert(Near(Float(kModel + 0x14), 1.05f));

    // Reloading the file establishes new loaded originals instead of retaining the first car's limits.
    RegisterModel(kModel, kModelParser);
    parse = [](uint32_t) { ModelValues(kModel, 0.9f, 1.0f); };
    Call(sub_0032F490, kModelParser);
    ApplyVehicleTuning();
    assert(Near(Float(kModel + 0x14), 1.35f) && Near(Float(kModel + 0x18), 1.5f));

    // A decommitted target is retired. Recommitting the same page must not revive an old tracked address.
    committed[kModel / mem::kPage] = false;
    settings.wheel_limit = 3;
    ApplyVehicleTuning();
    committed[kModel / mem::kPage] = true;
    ApplyVehicleTuning();
    assert(Near(Float(kModel + 0x14), 1.35f));

    // Changed neighbouring fields signal reuse; stop writing this wheel block.
    PutFloat(kRear + 0x40, 0.7f);
    settings.ride_rear = 0.3f;
    ApplyVehicleTuning();
    assert(Near(Float(kRear + 0x48), -0.02f));

    // Invalid settings and invalid parsed floats never enter the game's physics data.
    settings.ride_front = std::numeric_limits<float>::infinity();
    ApplyVehicleTuning();
    assert(Near(Float(kFront + 0x48), 0.12f));
    constexpr uint32_t invalidModel = 0x22000, invalidParser = 0x24000;
    ModelValues(invalidModel, 1, 1);
    RegisterModel(invalidModel, invalidParser);
    parse = [](uint32_t) {
        PutFloat(invalidModel + 0x14, std::numeric_limits<float>::quiet_NaN());
        PutFloat(invalidModel + 0x18, std::numeric_limits<float>::infinity());
    };
    Call(sub_0032F490, invalidParser);
    ApplyVehicleTuning();
    assert(std::isnan(Float(invalidModel + 0x14)) && std::isinf(Float(invalidModel + 0x18)));

    // A parser used only for serialization may die without reading. Its pending pointers must not survive reuse.
    constexpr uint32_t savedModel = 0x26000, savedParser = 0x28000;
    ModelValues(savedModel, 1, 1);
    RegisterModel(savedModel, savedParser);
    Call(sub_0032F840, savedParser);
    parse = {};
    Call(sub_0032F490, savedParser);
    ApplyVehicleTuning();
    assert(Float(savedModel + 0x14) == 1 && Float(savedModel + 0x18) == 1);

    // The original visitor may return an object on an unavailable page. The wrapper must not inspect its members.
    committed[0x30000 / mem::kPage] = false;
    Call(sub_000A3CA0, 0x30000, 0x32000);
    RegisterModel(0x30000, 0x32000);
    Call(sub_0032F490, 0x32000);
    ApplyVehicleTuning();

    // A garage reset supplies a fresh baseline, rather than accumulating the previous player offset.
    settings.ride_front = 0.1f;
    settings.ride_rear = -0.05f;
    PutFloat(kFront + 0x48, -0.15f);
    PutFloat(kRear + 0x48, 0.025f);
    TuningCarCfgApplied(kSim);
    assert(Near(Float(kFront + 0x48), -0.05f) && Near(Float(kRear + 0x48), -0.025f));
    PutFloat(kFront + 0x48, -0.15f); // the real ApplyCarCfg writes the garage values before every callback
    PutFloat(kRear + 0x48, 0.025f);
    TuningCarCfgApplied(kSim);
    assert(Near(Float(kFront + 0x48), -0.05f));
    PutFloat(kFront + 0x48, -0.05f); // new step equals the previous tuned result, but still needs the F1 offset
    PutFloat(kRear + 0x48, 0.025f);
    TuningCarCfgApplied(kSim);
    assert(Near(Float(kFront + 0x48), 0.05f));
    TuningSetPlayerSim(kSim, false);
    ApplyVehicleTuning();
    assert(Near(Float(kFront + 0x48), -0.05f) && Near(Float(kRear + 0x48), 0.025f));

    // Deep lowering must allow enough wheel compression/extension to preserve contact. Returning to stock,
    // raising an axle or losing player ownership restores the file's original limits without accumulation.
    constexpr uint32_t lowSim = 0x40000, lowFront = 0x42000, lowRear = 0x44000;
    kernel::W32(lowSim + 0x54, lowFront); kernel::W32(lowSim + 0x58, lowRear);
    for (uint32_t w : {lowFront, lowRear}) {
        PutFloat(w + 0x40, 0.5f); PutFloat(w + 0x44, 0.18f);
        PutFloat(w + 0x4C, 0.24f); PutFloat(w + 0x50, 0.23f);
    }
    settings.ride_front = 0; settings.ride_rear = -0.1f;
    PutFloat(lowFront + 0x48, -0.35f); PutFloat(lowRear + 0x48, -0.1f);
    TuningSetPlayerSim(lowSim, true);
    TuningCarCfgApplied(lowSim);
    assert(Near(Float(lowFront + 0x4C), 0.44f) && Near(Float(lowFront + 0x44), 0.38f));
    assert(Near(Float(lowRear + 0x4C), 0.29f) && Near(Float(lowRear + 0x44), 0.23f));
#ifdef MC3_TEST_REAL_WHEEL_SOLVER
    // The title's actual contact solver clamps ground compression to the tuning bounds. Its original clamp
    // rejects these contact distances with stock travel, while extended travel retains them. This checks the
    // real guest instruction path; it does not establish a match between the chassis collision mesh and visuals.
    constexpr uint32_t runtimeWheel = 0x46000;
    kernel::W32(runtimeWheel + 4, lowFront);
    PutFloat(0x447C1C, 1.0f); PutFloat(0x4B2D50, 60.0f); PutFloat(0x4B2D48, 1.0f / 60.0f);
    PutFloat(runtimeWheel + 0x108, 1.0f);
    PutFloat(runtimeWheel + 0x19C, 1.0f); // a positive support force over this fixture's tested range
    auto solve = [&](float requested, bool contact) {
        X86Context ctx{}; ctx.ecx = runtimeWheel; ctx.esp = kStack;
        PutFloat(kStack + 4, requested); kernel::W32(kStack + 8, contact);
        PutFloat(kStack + 12, 1.0f / 60.0f);
        sub_000A7250(ctx, guest.data());
        assert(ctx.esp == kStack + 16);
        return Float(runtimeWheel + 0x144);
    };
    assert(Near(solve(0.35f, true), 0.35f));
    assert(Near(solve(-0.30f, true), -0.30f));
    assert(Near(solve(0.35f, false), -0.38f)); // airborne wheels retain the title's extension behavior
    PutFloat(runtimeWheel + 0x10C, -0.35f); // original cached suspension offset
    PutFloat(runtimeWheel + 0x100, 0.35f);  // original runtime tire radius
    PutFloat(runtimeWheel + 0x144, 0.001f);
    auto displacement = [&](uint32_t caller, float arch_limit = 0.4f) {
        X86Context ctx{}; ctx.ecx = runtimeWheel; ctx.esp = kStack;
        kernel::W32(kStack, caller); PutFloat(kStack + 4 + 0x58, arch_limit);
        sub_000A65D0(ctx, guest.data());
        assert(ctx.esp == kStack + 4);
        return float(ctx.st[ctx.top]);
    };
    assert(Near(displacement(0xF7525), 0.04999f));
    assert(Near(displacement(0xF7525, -0.1f), -0.45001f)); // signed low-arch envelope is valid
    assert(Near(displacement(0xF74D5), 0.0f));
    assert(Near(displacement(0xAEAE1), 0.351f)); // physics/other callers get the real original displacement
    assert(Near(Float(runtimeWheel + 0x144), 0.001f)); // cosmetic correction never changes physical compression
#endif
    ApplyVehicleTuning(); ApplyVehicleTuning();
    assert(Near(Float(lowFront + 0x4C), 0.44f) && Near(Float(lowFront + 0x44), 0.38f));
    settings.ride_rear = 0.2f; ApplyVehicleTuning();
    assert(Near(Float(lowRear + 0x48), 0.1f));
    assert(Near(Float(lowRear + 0x4C), 0.24f) && Near(Float(lowRear + 0x44), 0.18f));
    TuningSetPlayerSim(lowSim, false); ApplyVehicleTuning();
    assert(Near(Float(lowFront + 0x48), -0.35f));
    assert(Near(Float(lowFront + 0x4C), 0.24f) && Near(Float(lowFront + 0x44), 0.18f));
#ifdef MC3_TEST_REAL_WHEEL_SOLVER
    assert(Near(solve(0.35f, true), 0.24f));
    assert(Near(solve(-0.30f, true), -0.18f));
    PutFloat(runtimeWheel + 0x144, 0.001f);
    assert(Near(displacement(0xF7525), 0.351f)); // a computer car retains the original visual fit query too
#endif
    TuningForgetSim(lowSim);

    // Destruction retires ownership and pending fields before a recycled address belongs to a different car.
    Call(sub_000A3CA0, kSim, kSimParser);
    TuningForgetSim(kSim);
    PutFloat(kFront + 0x48, 0.2f);
    parse = {};
    Call(sub_0032F490, kSimParser);
    TuningSetPlayerSim(kSim, true);
    ApplyVehicleTuning();
    assert(Near(Float(kFront + 0x48), 0.2f));
}
