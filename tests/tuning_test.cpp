// Exercise the actual game hooks with a small guest address space and parser bodies supplied by this test.
#include "app/app.h"
#include "core/log.h"
#include "xdk/hle.h"

#include <array>
#include <cassert>
#include <cmath>
#include <cstring>
#include <functional>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

namespace {
std::vector<uint8_t> guest(0x600000);
std::array<bool, 0x600> committed;
std::function<void(uint32_t)> parse;
std::function<void(uint32_t)> render_apply;
std::function<void(uint32_t)> rebuild;
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
X86_FUNC(__imp__sub_000FFD90) { ctx.esp += 4; }
X86_FUNC(__imp__sub_000F32A0) { if (render_apply) render_apply(ctx.ecx); ctx.esp += 16; }
X86_FUNC(__imp__sub_00103C50) { if (rebuild) rebuild(kernel::R32(ctx.esp + 4)); ctx.esp += 12; }
#ifndef MC3_TEST_REAL_WHEEL_SOLVER
X86_FUNC(__imp__sub_000A7250) { ctx.esp += 16; }
X86_FUNC(__imp__sub_000A65D0) { x87_push(ctx, 0); ctx.esp += 4; }
X86_FUNC(__imp__sub_001284F0) { ctx.esp += 24; }
#endif

X86_EXTERN_FUNC(sub_000A3CA0);
X86_EXTERN_FUNC(sub_000FB760);
X86_EXTERN_FUNC(sub_0032F490);
X86_EXTERN_FUNC(sub_0032F840);
X86_EXTERN_FUNC(sub_000A7250);
X86_EXTERN_FUNC(sub_000A65D0);
X86_EXTERN_FUNC(sub_001284F0);
X86_EXTERN_FUNC(sub_000FFD90);
X86_EXTERN_FUNC(sub_000F32A0);
X86_EXTERN_FUNC(sub_00103C50);
void ApplyVehicleTuning();
void TuningSetPlayerSim(uint32_t sim, bool player);
void TuningForgetSim(uint32_t sim);
void TuningCarCfgApplied(uint32_t sim);
void TuningBindCar(uint32_t car, uint32_t sim, uint32_t cfg, bool player);

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

int main(int argc, char** argv) {
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
        kernel::W32(kStack, caller); PutFloat(kStack + 4 + (caller == 0xF7B7D ? 0x18 : 0x58), arch_limit);
        sub_000A65D0(ctx, guest.data());
        assert(ctx.esp == kStack + 4);
        return float(ctx.st[ctx.top]);
    };
    assert(Near(displacement(0xF7525), 0.04999f));
    assert(Near(displacement(0xF7B7D), 0.04999f)); // the opposite side has its own ordinary wheel branch
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

    // Real per-axle controls, live refresh and render values. The same hook runs for any model; distinct values
    // at front/rear detect axle swaps, and two independent cars detect stale addresses and shared-data edits.
    settings = app::Settings::Tuning{};
    constexpr uint32_t car = 0x58000, sim = 0x50000, front = 0x52000, rear = 0x54000, engine = 0x56000;
    constexpr uint32_t cfg = 0x5A000, render = 0x90000, model = 0xA0000;
    kernel::W32(car + 0x50, sim); kernel::W32(car + 0x64, cfg); kernel::W32(car + 0x68, render);
    kernel::W32(sim + 0x54, front); kernel::W32(sim + 0x58, rear); kernel::W32(sim + 0x5C, engine);
    kernel::W32(render + 0x1800, model);
    PutFloat(cfg + 0x0C, -2.5f); PutFloat(cfg + 0x10, -1.0f);
    PutFloat(render + 0x12D8, -.04f); PutFloat(render + 0x12DC, -.03f);
    auto original_parameters = [=] {
        for (const auto [w, base] : {std::pair{front, 1.0f}, std::pair{rear, 2.0f}}) {
            PutFloat(w + 0x40, .5f); PutFloat(w + 0x44, .18f);
            PutFloat(w + 0x4C, .24f); PutFloat(w + 0x50, .23f);
            PutFloat(w + 0x58, base); PutFloat(w + 0x5C, base * .1f);
            PutFloat(w + 0x34, base * .3f); PutFloat(w + 0x68, base * 2.0f); PutFloat(w, base * 1.5f);
        }
        PutFloat(engine + 4, 600); PutFloat(engine + 8, 1000); PutFloat(engine + 0x0C, 6000);
    };
    original_parameters();
    PutFloat(front + 0x48, -.35f); PutFloat(rear + 0x48, -.20f);
    TuningSetPlayerSim(sim, true); TuningBindCar(car, sim, cfg, true); TuningCarCfgApplied(sim);
    settings.camber_front = -12; settings.camber_rear = -8;
    settings.offset_front = .04f; settings.offset_rear = -.02f;
    settings.spring_front = 1.5f; settings.spring_rear = .75f;
    settings.damper_front = 1.25f; settings.damper_rear = .5f;
    settings.brake_front = 2; settings.brake_rear = .5f;
    settings.grip_front = 1.25f; settings.grip_rear = .75f; settings.engine_power = 2;
    ApplyVehicleTuning(); ApplyVehicleTuning();
    assert(Near(Float(front + 0x58), 1.5f) && Near(Float(rear + 0x58), 1.5f));
    assert(Near(Float(front + 0x5C), .125f) && Near(Float(rear + 0x5C), .1f));
    assert(Near(Float(front + 0x34), .6f) && Near(Float(rear + 0x34), .3f));
    assert(Near(Float(front + 0x68), 2.5f) && Near(Float(rear + 0x68), 3));
    assert(Near(Float(front), 1.875f) && Near(Float(rear), 2.25f) && Near(Float(engine + 4), 1200));
    assert(Near(Float(render + 0x12E8), -14.5f) && Near(Float(render + 0x12EC), -9));
    assert(Near(Float(render + 0x12D8), -.08f) && Near(Float(render + 0x12DC), -.01f));
    assert(Near(Float(cfg + 0x0C), -2.5f)); // original purchased save data is not rewritten
    kernel::W32(cfg + 8, 0); Call(sub_000FFD90, car);
    assert((kernel::R32(cfg + 8) & 0x81) == 0x81); // first live refresh
    kernel::W32(cfg + 8, 0); Call(sub_000FFD90, car); assert(kernel::R32(cfg + 8) == 0);
    settings.offset_rear = -.01f; Call(sub_000FFD90, car); assert((kernel::R32(cfg + 8) & 0x81) == 0x81);

    // A paint-only render refresh must not accidentally adopt the already offset wheel as its new baseline.
    kernel::W32(kStack + 8, 0);
    kernel::W32(cfg + 8, 0x100); Call(sub_000F32A0, render, cfg); ApplyVehicleTuning();
    assert(Near(Float(render + 0x12D8), -.08f) && Near(Float(render + 0x12DC), -.02f));
    // A real wheel rebuild supplies new unmodified hub offsets. Take that baseline exactly once.
    kernel::W32(cfg + 8, 1);
    render_apply = [](uint32_t r) { PutFloat(r + 0x12D8, -.06f); PutFloat(r + 0x12DC, -.05f); };
    Call(sub_000F32A0, render, cfg); render_apply = {}; ApplyVehicleTuning();
    assert(Near(Float(render + 0x12D8), -.10f) && Near(Float(render + 0x12DC), -.04f));
    rebuild = [&](uint32_t rebuilt_car) { assert(rebuilt_car == car); original_parameters(); };
    Call(sub_00103C50, 0, car); rebuild = {}; ApplyVehicleTuning();
    assert(Near(Float(engine + 4), 1200) && Near(Float(front + 0x58), 1.5f)); // no multiplier accumulation

#ifdef MC3_TEST_REAL_WHEEL_SOLVER
    // Execute the actual retail airbag mover through the production wrapper. All four wheel channels use their
    // own front/rear baseline, converge without overshoot, and retain original rates and the neutral position.
    constexpr uint32_t bag = 0x60000, bag_tune = 0x62000;
    kernel::W32(bag + 8, car); kernel::W32(bag + 4, bag_tune);
    PutFloat(bag_tune + 0x10, .5f); PutFloat(bag_tune + 0x14, .8f);
    PutFloat(0x4B2D48, 1.0f / 60);
    auto move_bag = [&](uint32_t wheel_index, int state, float baseline) {
        X86Context ctx{}; ctx.ecx = bag; ctx.esp = kStack;
        kernel::W32(kStack + 4, bag + 0x20 + wheel_index * 4); kernel::W32(kStack + 8, uint32_t(state));
        PutFloat(kStack + 12, .1f); PutFloat(kStack + 16, -.1f); PutFloat(kStack + 20, baseline);
        sub_001284F0(ctx, guest.data()); assert(ctx.esp == kStack + 24);
        return Float(bag + 0x20 + wheel_index * 4);
    };
    for (unsigned w = 0; w != 4; ++w) {
        const float baseline = w < 2 ? -.35f : -.20f;
        PutFloat(bag + 0x20 + w * 4, 0);
        for (int n = 0; n < 180; ++n) assert(move_bag(w, +1, baseline) >= baseline - .00001f);
        assert(Near(Float(bag + 0x20 + w * 4), baseline));
        for (int n = 0; n < 180; ++n) assert(move_bag(w, -1, baseline) <= -baseline + .00001f);
        assert(Near(Float(bag + 0x20 + w * 4), -baseline));
        for (int n = 0; n < 180; ++n) move_bag(w, 0, baseline);
        assert(Near(Float(bag + 0x20 + w * 4), baseline));
    }
    settings.airbag_match_ride = false; PutFloat(bag + 0x20, 0);
    for (int n = 0; n < 120; ++n) move_bag(0, +1, -.35f);
    assert(Near(Float(bag + 0x20), -.1f)); // disabled: unmodified original endpoint
#endif
    TuningSetPlayerSim(sim, false); ApplyVehicleTuning();
    assert(Near(Float(front + 0x58), 1) && Near(Float(rear + 0x58), 2));
    assert(Near(Float(engine + 4), 600));
    assert(Near(Float(render + 0x12D8), -.06f) && Near(Float(render + 0x12DC), -.05f));
    assert(Near(Float(render + 0x12E8), -2.5f) && Near(Float(render + 0x12EC), -1));
    TuningForgetSim(sim);

#ifdef MC3_TEST_REAL_WHEEL_SOLVER
    // Optional catalogue generated from the user's archives by tools/garage/audit_vehicle_tuning.py. No retail
    // assets are checked into the test suite. Execute original solver/query bodies for every shipped front/rear
    // tune, rather than assuming the first test car's travel and arch dimensions represent the whole catalogue.
    if (argc > 1) {
        std::ifstream catalogue(argv[1]); assert(catalogue);
        std::string line; unsigned models = 0, queries = 0;
        constexpr uint32_t check_sim = 0x100000, check_front = 0x102000, check_rear = 0x104000, wheel = 0x106000;
        while (std::getline(catalogue, line)) {
            std::istringstream row(line); std::string name;
            std::array<float, 2> travel{}, extent{}, stop{}, suspension_extent{}, arch{};
            row >> name;
            for (unsigned a = 0; a < 2; ++a) row >> travel[a] >> extent[a] >> stop[a] >> suspension_extent[a] >> arch[a];
            assert(row && !name.empty());
            settings = app::Settings::Tuning{};
            kernel::W32(check_sim + 0x54, check_front); kernel::W32(check_sim + 0x58, check_rear);
            for (unsigned a = 0; a < 2; ++a) {
                const uint32_t tune = a ? check_rear : check_front;
                PutFloat(tune + 0x40, travel[a]); PutFloat(tune + 0x44, extent[a]);
                PutFloat(tune + 0x4C, stop[a]); PutFloat(tune + 0x50, suspension_extent[a]);
            }
            TuningSetPlayerSim(check_sim, true);
            for (float lowering : {-.175f, -.35f, -.85f}) {
                PutFloat(check_front + 0x48, lowering); PutFloat(check_rear + 0x48, lowering);
                TuningCarCfgApplied(check_sim); ApplyVehicleTuning();
                const float extra = -lowering - .15f;
                for (unsigned a = 0; a < 2; ++a) {
                    const uint32_t tune = a ? check_rear : check_front;
                    assert(Near(Float(tune + 0x4C), stop[a] + extra));
                    assert(Near(Float(tune + 0x44), extent[a] + extra));
                    kernel::W32(wheel + 4, tune); PutFloat(wheel + 0x108, 1);
                    PutFloat(wheel + 0x19C, 1); PutFloat(wheel + 0x10C, lowering);
                    for (float request : {0.0f, (stop[a] + extra) * .9f, -(extent[a] + extra) * .9f}) {
                        X86Context ctx{}; ctx.ecx = wheel; ctx.esp = kStack;
                        PutFloat(kStack + 4, request); kernel::W32(kStack + 8, 1);
                        PutFloat(kStack + 12, 1.0f / 60); sub_000A7250(ctx, guest.data());
                        if (!Near(Float(wheel + 0x144), request)) {
                            std::cerr << name << " axle " << a << " contact request " << request << " failed\n";
                            assert(false);
                        }
                    }
                    PutFloat(wheel + 0x144, .001f);
                    for (float radius : {.18f, .35f, .60f}) {
                        PutFloat(wheel + 0x100, radius);
                        for (uint32_t caller : {0xF7525u, 0xF7B7Du}) {
                            X86Context ctx{}; ctx.ecx = wheel; ctx.esp = kStack;
                            kernel::W32(kStack, caller);
                            PutFloat(kStack + 4 + (caller == 0xF7B7D ? 0x18 : 0x58), arch[a]);
                            sub_000A65D0(ctx, guest.data());
                            const float correction = std::min(0.0f, arch[a] - float(ctx.st[ctx.top]) - radius);
                            assert(correction >= -.00001f); ++queries;
                        }
                    }
                }
            }
            TuningForgetSim(check_sim); ++models;
        }
        assert(models > 0);
        std::cout << "Verified " << models << " original vehicle tunes, both axles, " << queries
                  << " opposite-side render queries; chassis-ground collision remains a live check.\n";
    }
#endif
}
