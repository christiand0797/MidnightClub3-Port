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
#include "kernel/memory.h"
#include "world_tuning.h"
#include "tuning_profiles.h"

using kernel::G;
using kernel::R32;
using kernel::W32;
using kernel::W16;

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

// The Internet Play entry opens the online lobbies (online.cpp), so its label says so. The lookup returns a string
// object whose +8 is the text; the copy keeps the object's other fields and points at a guest copy of the new label,
// in the same character width as the original (UTF-16 when its second byte is 0).
namespace {
uint32_t g_online_label = 0;
void RenameInternetPlay(X86Context& ctx) {
    uint32_t obj = ctx.eax;
    if (!obj || !R32(obj + 8)) return;
    if (!g_online_label) {
        uint32_t buf = mem::AllocSystem(0x100);
        if (!buf) return;
        for (uint32_t i = 0; i < 0x40; i += 4) W32(buf + i, R32(obj + i));
        uint32_t text = buf + 0x40, old = R32(obj + 8);
        const char* label = "Online Lobbies";
        bool wide = G<uint8_t>(old + 1) == 0;
        for (uint32_t i = 0; label[i] || !i; i++) {
            if (wide) W16(text + i * 2, uint16_t(uint8_t(label[i])));
            else G<uint8_t>(text + i) = uint8_t(label[i]);
            if (!label[i + 1]) {
                if (wide) W16(text + (i + 1) * 2, 0);
                else G<uint8_t>(text + i + 1) = 0;
                break;
            }
        }
        W32(buf + 8, text);
        g_online_label = buf;
        LOGR("online: Internet Play is labelled Online Lobbies (%s text)", wide ? "UTF-16" : "8-bit");
    }
    ctx.eax = g_online_label;
}
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
        if (k == "Internet_Play" && app::Get().online.enabled) {
            __imp__sub_000EA750(ctx, base);
            RenameInternetPlay(ctx);
            return;
        }
    }
    __imp__sub_000EA750(ctx, base);
}

// ── Career race results & events: safe listener broadcast (sub_002793C0 / sub_00279430) ──
// mcPgCareer::ReportRaceResult() (0x277EE0) broadcasts through a list of 12-byte listener entries
// {u16 id, u32 key, object*} at [this+0x78 .. this+0x7C], calling the entry at index [this+0x94] first.
// Listener callbacks can mutate or free entries in the list during iteration. Snapshot the list before
// iterating, validate object/vtable/code addresses before each call, and pick up any newly registered
// listeners at the end (needed for race startup callbacks).
namespace {
constexpr uint32_t kListenerMax = 512;

bool ListenerRam(uint32_t va) {
    return !(va & 3u) && va >= 0x10000u && va < 0x84000000u && mem::IsCommitted(va);
}

bool ListenerValid(uint32_t obj, uint32_t slot) {
    if (!ListenerRam(obj)) return false;
    uint32_t vt = R32(obj);
    if ((vt & 3u) || vt < 0x004476E0u || vt >= 0x004A8140u || !mem::IsCommitted(vt)) return false;
    uint32_t fn = R32(vt + slot);
    return fn >= 0x00012000u && fn < 0x004476C8u;
}

bool ListenerLive(uint32_t self, uint32_t obj, uint16_t id, uint32_t key) {
    uint32_t begin = R32(self + 0x78), end = R32(self + 0x7C);
    if (!ListenerRam(begin) || end < begin || end - begin > 12u * 4096u) return false;
    for (uint32_t e = begin; e + 12u <= end; e += 12u)
        if (R32(e + 8) == obj && kernel::R16(e) == id && R32(e + 4) == key) return true;
    return false;
}

void ListenerPrune(uint32_t self, uint32_t slot) {
    if (!ListenerRam(self)) return;
    uint32_t begin = R32(self + 0x78), end = R32(self + 0x7C), idx = R32(self + 0x94);
    if (!ListenerRam(begin) || end < begin || end - begin > 12u * 4096u || (end - begin) % 12u != 0) return;
    uint32_t wr = begin, new_idx = 0xFFFFFFFFu, i = 0;
    for (uint32_t rd = begin; rd < end; rd += 12u, ++i) {
        uint32_t obj = R32(rd + 8);
        if (!ListenerValid(obj, slot)) continue;
        if (i == idx) new_idx = (wr - begin) / 12u;
        if (wr != rd) {
            W32(wr, R32(rd));
            W32(wr + 4, R32(rd + 4));
            W32(wr + 8, R32(rd + 8));
        }
        wr += 12u;
    }
    if (wr != end) {
        W32(self + 0x7C, wr);
        if (idx != 0xFFFFFFFFu) W32(self + 0x94, new_idx);
    }
}

void ListenerCall(X86Context& ctx, uint8_t* base, uint32_t obj, uint32_t slot, uint32_t arg) {
    uint32_t fn_va = R32(R32(obj) + slot);
    uint32_t esp0 = ctx.esp, bx = ctx.ebx, si = ctx.esi, di = ctx.edi, bp = ctx.ebp;
    X86_PUSH32(arg);
    X86_PUSH32(0x2793F5u);
    ctx.ecx = obj;
    x86_icall(ctx, base, fn_va);
    ctx.esp = esp0;
    ctx.ebx = bx;
    ctx.esi = si;
    ctx.edi = di;
    ctx.ebp = bp;
}

void ListenerBroadcast(X86Context& ctx, uint8_t* base, uint32_t slot) {
    uint32_t self = ctx.ecx;
    uint32_t arg = R32(ctx.esp + 4);
    uint32_t saved_ebx = ctx.ebx, saved_esi = ctx.esi, saved_edi = ctx.edi, saved_ebp = ctx.ebp;
    struct Entry { uint16_t id; uint32_t key, obj; } snap[kListenerMax];

    ListenerPrune(self, slot);
    uint32_t begin = R32(self + 0x78), end = R32(self + 0x7C), idx = R32(self + 0x94);
    uint32_t n = (ListenerRam(begin) && end >= begin) ? (end - begin) / 12u : 0;
    if (n > kListenerMax) n = kListenerMax;
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t e = begin + 12u * i;
        snap[i] = {kernel::R16(e), R32(e + 4), R32(e + 8)};
    }
    uint16_t cur_id = 0;
    uint32_t cur_key = 0;
    if (idx != 0xFFFFFFFFu && idx < n) {
        cur_id = snap[idx].id;
        cur_key = snap[idx].key;
        if (ListenerValid(snap[idx].obj, slot))
            ListenerCall(ctx, base, snap[idx].obj, slot, arg);
    }
    for (uint32_t i = 0; i < n; ++i) {
        if (snap[i].id == cur_id && snap[i].key == cur_key) continue;
        if (!ListenerLive(self, snap[i].obj, snap[i].id, snap[i].key)) continue;
        if (ListenerValid(snap[i].obj, slot))
            ListenerCall(ctx, base, snap[i].obj, slot, arg);
    }
    for (int round = 0; round < 8 && n < kListenerMax; ++round) {
        uint32_t b = R32(self + 0x78), en = R32(self + 0x7C), added = 0;
        if (!ListenerRam(b) || en < b || en - b > 12u * 4096u) break;
        for (uint32_t e = b; e + 12u <= en && n < kListenerMax; e += 12u) {
            uint16_t id = kernel::R16(e);
            uint32_t key = R32(e + 4), obj = R32(e + 8);
            bool seen = (id == cur_id && key == cur_key);
            for (uint32_t k = 0; k < n && !seen; ++k)
                seen = (snap[k].obj == obj && snap[k].id == id && snap[k].key == key);
            if (seen) continue;
            snap[n++] = {id, key, obj};
            ++added;
            if (ListenerValid(obj, slot))
                ListenerCall(ctx, base, obj, slot, arg);
        }
        if (!added) break;
    }
    ctx.ebx = saved_ebx;
    ctx.esi = saved_esi;
    ctx.edi = saved_edi;
    ctx.ebp = saved_ebp;
    ctx.esp += 8;  // ret 0x04
}
}  // namespace

X86_EXTERN_FUNC(__imp__sub_002793C0);
X86_FUNC(sub_002793C0) { ListenerBroadcast(ctx, base, 0x28); }

X86_EXTERN_FUNC(__imp__sub_00279430);
X86_FUNC(sub_00279430) { ListenerBroadcast(ctx, base, 0x2C); }

// ── Watchdog heartbeat thread (sub_001B15D0): sleep 8 ms instead of spinning on Sleep(0) ──
// sub_001B15D0 calls sub_0001735C(0) from return address 0x001B1654 in a tight loop checking a 1-second timer.
X86_EXTERN_FUNC(__imp__sub_0001735C);
X86_FUNC(sub_0001735C) {
    if (R32(ctx.esp) == 0x001B1654u && R32(ctx.esp + 4) == 0u) {
        W32(ctx.esp + 4, 8u);
    }
    __imp__sub_0001735C(ctx, base);
}

// ── Steering wheel capability check (sub_00084BD0): cache per open handle ──
X86_EXTERN_FUNC(__imp__sub_00084BD0);
X86_FUNC(sub_00084BD0) {
    static uint32_t handle_of[8] = {}, answer_of[8] = {};
    static unsigned next = 0;
    uint32_t pad = ctx.ecx;
    if (pad && mem::IsCommitted(pad) && R32(pad + 0x1C) == 4u) {
        uint32_t handle = R32(pad + 0x34);
        if (handle) {
            for (unsigned i = 0; i < 8; ++i) {
                if (handle_of[i] == handle) {
                    ctx.eax = (ctx.eax & 0xFFFFFF00u) | (answer_of[i] & 0xFFu);
                    ctx.esp += 4;
                    return;
                }
            }
            __imp__sub_00084BD0(ctx, base);
            handle_of[next % 8] = handle;
            answer_of[next % 8] = ctx.eax & 0xFFu;
            ++next;
            return;
        }
    }
    __imp__sub_00084BD0(ctx, base);
}
