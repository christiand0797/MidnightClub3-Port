// Hooks for default.xbe. A strong definition replaces the recompiled function at that address, for example:
//
// #include "xdk/hle.h"
//
// X86_EXTERN_FUNC(__imp__sub_00012345);
// X86_FUNC(sub_00012345) {
//     __imp__sub_00012345(ctx, base);  // the original
// }
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>

#include "app/app.h"
#include "core/log.h"
#include "xdk/hle.h"
#include "world_tuning.h"
#include "tuning_profiles.h"

using kernel::G;
using kernel::R32;
using kernel::W32;

// The engine's clock (time manager at 0x20B6F0, once a frame) runs in one of two modes:
//  - measured: seconds since the last frame from the time stamp counter (1/733.33 MHz), rounded UP to whole 60 Hz
//    fields when the byte at 0x534776 is set;
//  - fixed step (0x534770 set, by 0x20BB00(rate, substeps) from 0x20880 when play starts): every frame advances
//    1 / (rate * substeps) seconds, kept at 0x4B2D58, the rate at 0x4B2D54. The game asks for 30: the console's races
//    ran at 30 fps, and here at 60 fps that made cars, traffic, animation and the race clock run at twice speed.
// 60 fps mode asks for a 60 Hz step instead: the same simulation in half-size steps, twice as many a second.
namespace {
constexpr uint32_t kPrevTick = 0x534778;    // time stamp low dword at the last update
constexpr uint32_t kFrameLock = 0x534776;   // byte: round measured time up to whole fields
constexpr uint32_t kFixedStep = 0x534770;   // dword: fixed step mode
constexpr uint32_t kStep = 0x4B2D58;        // float seconds a fixed step frame advances
constexpr uint32_t kFrameTime = 0x4B2D70;   // float seconds the update settled on
constexpr double kTicksPerSecond = 733333333.0;
constexpr double kField = 1.0 / 60.0;
constexpr double kMargin = 0.0002;          // read this far short of the field, the original's own read comes later
double g_carry = 0;                         // seconds measured but not yet credited (or credited ahead)
// a summary every ten seconds: what the game counted against what passed
double g_real = 0, g_game = 0;
uint32_t g_frames = 0, g_fixed = 0;
float g_asked_rate = 0;                     // the rate the game last asked the fixed step for
float g_original_step = 0;                  // restored when 60 fps is turned off during a race
std::chrono::steady_clock::time_point g_report = std::chrono::steady_clock::now();

float F32(uint32_t va) {
    float f;
    uint32_t b = R32(va);
    std::memcpy(&f, &b, 4);
    return f;
}
}  // namespace

// 0x20BB00(float rate, int substeps): fixed step mode, a frame advancing 1 / (rate * substeps) seconds
X86_EXTERN_FUNC(__imp__sub_0020BB00);
X86_FUNC(sub_0020BB00) {
    const uint32_t rate_arg = ctx.esp + 4;
    const uint32_t original_bits = R32(rate_arg);
    float rate = F32(rate_arg);
    uint32_t substeps = R32(ctx.esp + 8);
    g_asked_rate = rate;
    g_original_step = std::isfinite(rate) && rate > 0 && substeps ? float(1.0 / (double(rate) * substeps)) : 0;
    g_carry = 0;
    if (app::Get().graphics.sixty && rate > 0 && rate < 59.0f) {
        float sixty = rate * std::round(60.0f / rate);  // 30 becomes 60, 20 becomes 60, 15 becomes 60
        uint32_t bits;
        std::memcpy(&bits, &sixty, 4);
        W32(ctx.esp + 4, bits);
        LOGR("timing: the game asked for a %.0f Hz fixed step, running it at %.0f Hz for 60 fps", rate, sixty);
    }
    __imp__sub_0020BB00(ctx, base);
    // This is cdecl: the caller still owns its arguments after the original returns.
    W32(rate_arg, original_bits);
}

void ApplyVehicleTuning();  // tuning.cpp
void ApplyEconomyCommands();  // economy.cpp: queued, one-shot current-career actions
// replay.cpp installs this: records the fixed step every replay frame used and forces it again on playback
// (a pointer, so the timing test links hooks.cpp alone)
bool (*g_replay_fixed_step)(uint32_t base_bits) = nullptr;
extern float g_lod_scale;  // lod.cpp
extern float g_fov_scale;  // fov.cpp

// measured mode: the frame is read just under the nearest whole number of 60 Hz fields, so the round up gives that
// many fields; what that drops or adds is carried into the next frame, so game time keeps to real time
X86_EXTERN_FUNC(__imp__sub_0020B6F0);
X86_FUNC(sub_0020B6F0) {
    uint32_t now = uint32_t(x86_rdtsc());
    double measured = double(uint32_t(now - R32(kPrevTick))) / kTicksPerSecond;
    if (R32(kFixedStep) && app::Get().graphics.sixty) {
        // fixed step: a frame the PC was late with (two or three 60 Hz refreshes) advances that many fields, the
        // rounding carried, so the race keeps to real time instead of slowing with every missed refresh
        if (measured < 4.5 * kField) {
            double want = measured + g_carry;
            double fields = std::clamp(std::floor(want / kField + 0.5), 1.0, 4.0);
            g_carry = std::clamp(want - fields * kField, -kField, kField);
            float step = float(fields * kField);
            uint32_t bits;
            std::memcpy(&bits, &step, 4);
            W32(kStep, bits);
        } else {
            g_carry = 0;  // a load or a pause: one field, as the console's step would
            float step = float(kField);
            uint32_t bits;
            std::memcpy(&bits, &step, 4);
            W32(kStep, bits);
        }
        // a replay re-simulates the race from its recorded input: it has to step exactly as the recording did
        float one = float(kField);
        uint32_t one_bits;
        std::memcpy(&one_bits, &one, 4);
        if (g_replay_fixed_step && g_replay_fixed_step(one_bits)) g_carry = 0;
    } else if (R32(kFixedStep)) {
        // The frame-rate switch also lowers the host cap to 30. Leaving the last 1/60 step here would halve
        // simulation speed until another level load calls the fixed-step setter again.
        if (g_original_step > 0) {
            uint32_t bits;
            std::memcpy(&bits, &g_original_step, 4);
            W32(kStep, bits);
        }
        uint32_t original_bits;
        std::memcpy(&original_bits, &g_original_step, 4);
        if (g_replay_fixed_step) g_replay_fixed_step(g_original_step > 0 ? original_bits : 0);
        g_carry = 0;
    } else if (G<uint8_t>(kFrameLock) && measured < 4.5 * kField) {
        double want = measured + g_carry;
        double fields = std::max(1.0, std::floor(want / kField + 0.5));
        g_carry = std::clamp(want - fields * kField, -kField, kField);
        W32(kPrevTick, now - uint32_t((fields * kField - kMargin) * kTicksPerSecond));
    } else {
        g_carry = 0;  // fixed step, a pause, a load or an unlocked clock: nothing to keep in step with
    }
    __imp__sub_0020B6F0(ctx, base);
    g_lod_scale = app::Get().graphics.lod;
    g_fov_scale = app::Get().graphics.fov_scale;
    mc3::car_tuning::ObserveFrame();
    ApplyVehicleTuning();
    ApplyEconomyCommands();
    mc3::world::Apply(ctx, base);

    // a frame far past its field: loads and hitches, set against the audio log's starved streams
    if (measured > 0.1) {
        static std::atomic<unsigned> long_frames{0};
        if (long_frames.fetch_add(1) < 300) LOGR("timing: a frame took %.0f ms", measured * 1000);
    }
    g_real += measured;
    g_game += F32(kFrameTime);
    g_frames++;
    if (R32(kFixedStep)) g_fixed++;
    auto t = std::chrono::steady_clock::now();
    if (t >= g_report) {
        if (g_frames > 1)
            LOGR("timing: %u frames (%u fixed step %.4f s, game asked %.0f Hz), %.2f game s over %.2f real s, x%.3f",
                 g_frames, g_fixed, F32(kStep), g_asked_rate, g_game, g_real, g_real > 0 ? g_game / g_real : 0.0);
        g_report = t + std::chrono::seconds(10);
        g_real = g_game = 0;
        g_frames = g_fixed = 0;
    }
}

// Text lookup, 0x0EA750 (thiscall: this = string table, arg = key): the menus fetch every label through it. Each key
// is logged once with its caller, which maps the menu code (where a page is built, which keys it uses).
#include <mutex>
#include <set>
#include <string>
namespace {
std::mutex g_keys_lock;
std::set<std::string> g_keys;
}  // namespace

X86_EXTERN_FUNC(__imp__sub_000EA750);
X86_FUNC(sub_000EA750) {
    uint32_t caller = R32(ctx.esp), key = R32(ctx.esp + 4), table = ctx.ecx;
    if (key) {
        std::string k;
        for (uint32_t i = 0; i < 96; i++) {
            char c = char(G<uint8_t>(key + i));
            if (!c) break;
            k += c;
        }
        std::lock_guard<std::mutex> lock(g_keys_lock);
        if (g_keys.insert(k).second) LOGR("text: \"%s\" from %08X (table %08X)", k.c_str(), caller, table);
    }
    __imp__sub_000EA750(ctx, base);
}
