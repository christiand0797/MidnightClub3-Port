// Current-career money, from the original USA Remix XBE (SHA256 0b3d81a0...51a3):
//  0x27816E/0x2781A5: career manager vtable 0x473B98, singleton [0x557F0C].
//  0x2781E0/0x2781EE: its +0x0C points to the 0xCC8-byte career data, vtable 0x474288.
//  0x148A76 and 0x152746 read data+0xACC for the garage wallet; 0x1527DE writes it directly.
//  0x280256..0x2802A4 serializes this exact signed dword as "Money" (0x474130).
//  0x28301C..0x283024 loads the same field. There is no separate setter in these native paths.
//  0x197F0..0x1980D asks to save when [0x557648] OR manager+0x20 is nonzero; ordinary garage
//  changes mark these at 0x1431A9/0x1431F5. Use the same dirty semantics, not host-side save edits.
#include "economy.h"

#include <limits>
#include <mutex>

#include "kernel/kernel.h"
#include "kernel/memory.h"
#include "xdk/hle.h"

namespace mc3::economy {
namespace {
constexpr uint32_t kManager = 0x557F0C, kManagerVtable = 0x473B98, kDataVtable = 0x474288;
constexpr uint32_t kGameState = 0x53F2D4, kOwnedCount = 0x53F80C, kOwnedSelected = 0x53F80D;
constexpr uint32_t kDirty = 0x557648, kCash = 0xACC, kCity = 0x50;

struct Career { uint32_t manager = 0, data = 0, city = 0; };
struct Command { bool pending = false; Career career; uint32_t target = 0; uint64_t epoch = 0; };
std::mutex g_lock;
Snapshot g_snapshot;
Career g_observed;
Command g_command;
uint64_t g_epoch = 0;

bool Committed(uint32_t va, uint32_t bytes) {
    const uint64_t end = uint64_t(va) + bytes;
    if (!kernel::g_base || !bytes || va < 0x10000 || end > 0x84000000ull) return false;
    for (uint64_t page = va & ~(uint64_t(mem::kPage) - 1); page < end; page += mem::kPage)
        if (!mem::IsCommitted(uint32_t(page))) return false;
    return true;
}
bool Object(uint32_t va, uint32_t size, uint32_t vtable) {
    return (va & 3) == 0 && Committed(va, size) && kernel::R32(va) == vtable;
}
bool Same(const Career& a, const Career& b) {
    return a.manager == b.manager && a.data == b.data && a.city == b.city;
}
bool Current(Career& career, uint32_t& money) {
    if (!Committed(kManager, 4) || !Committed(kGameState, 4) ||
        !Committed(kOwnedCount, 2) || !Committed(kDirty, 1)) return false;
    // Original career logic checks state 2 at 0x277EE8; garage movie construction checks state 5
    // at 0x139EED. Do not edit the constructor's unloaded/template career in title/arcade states.
    const auto state = kernel::R32(kGameState);
    if (state != 2 && state != 5) return false;
    const auto count = kernel::G<uint8_t>(kOwnedCount), selected = kernel::G<uint8_t>(kOwnedSelected);
    // Original garage slot accesses use this count/index at 0x1430E7/0x143197; 30 slots at 0x26CCE0.
    if (!count || count > 30 || selected >= count) return false;
    career.manager = kernel::R32(kManager);
    if (!Object(career.manager, 0x64, kManagerVtable)) return false;
    career.data = kernel::R32(career.manager + 0x0C);
    if (!Object(career.data, 0xCC8, kDataVtable)) return false;
    // CurrentCareerCity's constructor sentinel is 13 (0x283513); a loaded city is below it.
    career.city = kernel::R32(career.data + kCity);
    if (career.city >= 13) return false;
    money = kernel::R32(career.data + kCash);
    return money <= uint32_t(std::numeric_limits<int32_t>::max());
}

void Invalidate() {
    std::lock_guard lock(g_lock);
    ++g_epoch;
    g_command = {};
    g_observed = {};
    g_snapshot.ready = false;
    g_snapshot.pending = false;
    g_snapshot.status = Status::CareerChanged;
    g_snapshot.career_token = g_epoch;
}
}  // namespace

Snapshot ReadSnapshot() {
    std::lock_guard lock(g_lock);
    return g_snapshot;
}

bool SetMoney(uint32_t target, uint64_t expected_career_token) {
    std::lock_guard lock(g_lock);
    if (target > kMaxMoney) {
        g_snapshot.status = Status::InvalidAmount;
        return false;
    }
    if (!g_snapshot.ready || g_command.pending) return false;
    if (expected_career_token != g_epoch || expected_career_token != g_snapshot.career_token) {
        g_snapshot.status = Status::CareerChanged;
        return false;
    }
    g_command = {true, g_observed, target, g_epoch};
    g_snapshot.pending = true;
    g_snapshot.status = Status::Queued;
    return true;
}

const char* StatusText(Status status) {
    switch (status) {
        case Status::Ready: return "Current career balance";
        case Status::Queued: return "Applying on the next game update";
        case Status::Applied: return "Balance changed; use the game's normal save flow";
        case Status::CareerChanged: return "Career changed; pending edit cancelled";
        case Status::InvalidAmount: return "Enter an amount from 0 to 999,999,999";
        default: return "Load a career and enter the garage or streets to edit money";
    }
}

void ApplyCommands() {
    // All guest reads/writes occur on the original game-update thread, never the ImGui thread.
    std::lock_guard lock(g_lock);
    Career current;
    uint32_t money = 0;
    const bool ready = Current(current, money);
    Status status = g_snapshot.status;
    // A ready-to-ready switch can happen entirely between two UI frames. Give
    // it a new token before checking pending commands, even if no unloaded frame
    // was visible to the menu. Reset/load hooks cover same-allocation reuse.
    if (ready != g_snapshot.ready || (ready && !Same(g_observed, current))) ++g_epoch;
    if (!ready) {
        if (g_command.pending) status = Status::CareerChanged;
        else if (status != Status::CareerChanged) status = Status::Unavailable;
        g_command = {};
        g_observed = {};
    } else {
        if (g_command.pending) {
            if (g_command.epoch == g_epoch && Same(g_command.career, current)) {
                if (money != g_command.target) {
                    kernel::W32(current.data + kCash, g_command.target);
                    kernel::G<uint8_t>(kDirty) = 1;
                    kernel::G<uint8_t>(current.manager + 0x20) = 1;
                    money = g_command.target;
                    ++g_snapshot.revision;
                }
                status = Status::Applied;
            } else {
                status = Status::CareerChanged;
            }
            g_command = {};
        } else if (!Same(g_observed, current) || !g_snapshot.ready || status == Status::Unavailable) {
            status = Status::Ready;
        }
        g_observed = current;
    }
    g_snapshot.ready = ready;
    g_snapshot.pending = false;
    g_snapshot.money = ready ? money : 0;
    g_snapshot.status = status;
    g_snapshot.career_token = g_epoch;
}

}  // namespace mc3::economy

void ApplyEconomyCommands() { mc3::economy::ApplyCommands(); }

// A profile can be loaded into the same allocation. Pointer equality alone cannot protect a request
// queued for the preceding career. Invalidate before the original reset/load/destruction runs.
X86_EXTERN_FUNC(__imp__sub_002827C0);
X86_FUNC(sub_002827C0) {
    mc3::economy::Invalidate();
    __imp__sub_002827C0(ctx, base);
}
X86_EXTERN_FUNC(__imp__sub_002829E0);
X86_FUNC(sub_002829E0) {
    mc3::economy::Invalidate();
    __imp__sub_002829E0(ctx, base);
}
X86_EXTERN_FUNC(__imp__sub_00276E80);
X86_FUNC(sub_00276E80) {
    mc3::economy::Invalidate();
    __imp__sub_00276E80(ctx, base);
}
