#include "economy.h"
#include "kernel/kernel.h"
#include "xdk/hle.h"

#include <cassert>
#include <iostream>
#include <set>
#include <thread>
#include <vector>

namespace fixture {
std::vector<uint8_t> memory(0x600000);
std::set<uint32_t> missing;
constexpr uint32_t manager = 0x20000, data = 0x30C00, other_data = 0x40C00;
unsigned reset_calls = 0, load_calls = 0, shutdown_calls = 0;
}
namespace kernel { uint8_t* g_base = fixture::memory.data(); }
namespace mem { bool IsCommitted(uint32_t page) {
    return page < fixture::memory.size() && !fixture::missing.contains(page & ~(mem::kPage - 1));
} }
namespace logx { void Write(Level, const char*, ...) {} }

X86_EXTERN_FUNC(sub_002827C0);
X86_EXTERN_FUNC(sub_002829E0);
X86_EXTERN_FUNC(sub_00276E80);
X86_FUNC(__imp__sub_002827C0) {
    assert(!mc3::economy::ReadSnapshot().ready && !mc3::economy::ReadSnapshot().pending);
    ++fixture::reset_calls; ctx.esp += 4;
}
X86_FUNC(__imp__sub_002829E0) {
    assert(!mc3::economy::ReadSnapshot().ready && !mc3::economy::ReadSnapshot().pending);
    ++fixture::load_calls;
    // A different profile can replace the cash in the SAME native allocation.
    kernel::W32(fixture::data + 0xACC, 45678);
    ctx.esp += 12;
}
X86_FUNC(__imp__sub_00276E80) {
    assert(!mc3::economy::ReadSnapshot().ready && !mc3::economy::ReadSnapshot().pending);
    ++fixture::shutdown_calls;
    kernel::W32(0x557F0C, 0); ctx.esp += 4;
}

#ifdef MC3_TEST_ORIGINAL_SAVE_NEEDED
bool EconomyOriginalSaveNeeded(uint8_t* memory);
#endif

namespace {
bool QueueMoney(uint32_t target) { return mc3::economy::SetMoney(target, mc3::economy::ReadSnapshot().career_token); }
void Setup(uint32_t money = 30000) {
    std::fill(fixture::memory.begin(), fixture::memory.end(), 0);
    fixture::missing.clear();
    kernel::W32(0x557F0C, fixture::manager);
    kernel::W32(fixture::manager, 0x473B98);
    kernel::W32(fixture::manager + 0xC, fixture::data);
    for (uint32_t data : {fixture::data, fixture::other_data}) {
        kernel::W32(data, 0x474288);
        kernel::W32(data + 0x50, 0);
        kernel::W32(data + 0xACC, money);
    }
    kernel::W32(0x53F2D4, 5);
    kernel::G<uint8_t>(0x53F80C) = 1;
    ApplyEconomyCommands();
}
uint32_t Cash(uint32_t data = fixture::data) { return kernel::R32(data + 0xACC); }
void Clean() {
    kernel::G<uint8_t>(0x557648) = 0;
    kernel::G<uint8_t>(fixture::manager + 0x20) = 0;
}
void Unready() {
    ApplyEconomyCommands();
    assert(!mc3::economy::ReadSnapshot().ready);
    assert(!QueueMoney(100000));
}
}

int main() {
    using namespace mc3::economy;
    assert(!ReadSnapshot().ready && !QueueMoney(100000));
    ApplyEconomyCommands(); // Uninitialized committed bytes are not a career.
    assert(!ReadSnapshot().ready);
    Setup();
    assert(ReadSnapshot().ready && ReadSnapshot().money == 30000);
#ifdef MC3_TEST_ORIGINAL_SAVE_NEEDED
    assert(!EconomyOriginalSaveNeeded(fixture::memory.data()));
#endif
    // An ordinary UI thread only queues the explicit edit. No cash/dirty writes until game update.
    std::thread ui([] { assert(QueueMoney(123456)); }); ui.join();
    assert(Cash() == 30000 && !kernel::G<uint8_t>(0x557648));
    assert(ReadSnapshot().pending && ReadSnapshot().status == Status::Queued);
    assert(!QueueMoney(999)); // No silent replacement of an already queued action.
    const auto before_edit = fixture::memory;
    ApplyEconomyCommands();
    assert(Cash() == 123456 && ReadSnapshot().money == 123456 && !ReadSnapshot().pending);
    assert(ReadSnapshot().status == Status::Applied);
    assert(kernel::G<uint8_t>(0x557648) == 1 && kernel::G<uint8_t>(fixture::manager + 0x20) == 1);
    for (size_t i = 0; i < before_edit.size(); ++i)
        if (before_edit[i] != fixture::memory[i])
            assert((i >= fixture::data + 0xACC && i < fixture::data + 0xAD0) ||
                   i == 0x557648 || i == fixture::manager + 0x20);
#ifdef MC3_TEST_ORIGINAL_SAVE_NEEDED
    assert(EconomyOriginalSaveNeeded(fixture::memory.data()));
    kernel::G<uint8_t>(0x557648) = 0;
    assert(EconomyOriginalSaveNeeded(fixture::memory.data())); // Native career flag alone survives.
    kernel::G<uint8_t>(fixture::manager + 0x20) = 0;
    assert(!EconomyOriginalSaveNeeded(fixture::memory.data()));
#endif
    const auto revision = ReadSnapshot().revision;
    const auto stable_career_token = ReadSnapshot().career_token;
    Clean();
    assert(QueueMoney(123456)); ApplyEconomyCommands();
    assert(ReadSnapshot().revision == revision && !kernel::G<uint8_t>(0x557648));
    // No permanent override: a normal purchase/reward changes the actual balance on later frames.
    kernel::W32(fixture::data + 0xACC, 120000); ApplyEconomyCommands();
    assert(Cash() == 120000 && ReadSnapshot().money == 120000);
    assert(QueueMoney(0)); ApplyEconomyCommands(); assert(Cash() == 0);
    assert(QueueMoney(kMaxMoney)); ApplyEconomyCommands(); assert(Cash() == kMaxMoney);
    assert(ReadSnapshot().career_token == stable_career_token);
    assert(!QueueMoney(kMaxMoney + 1) && !QueueMoney(0xFFFFFFFF));
    assert(Cash() == kMaxMoney);
    assert(ReadSnapshot().career_token == stable_career_token);

    // Rebinding to another data allocation or city cannot deliver a previous career's request.
    Setup(); assert(QueueMoney(900000));
    kernel::W32(fixture::manager + 0xC, fixture::other_data); ApplyEconomyCommands();
    assert(Cash() == 30000 && Cash(fixture::other_data) == 30000);
    assert(ReadSnapshot().status == Status::CareerChanged && !ReadSnapshot().pending);
    Setup(); assert(QueueMoney(900000)); kernel::W32(fixture::data + 0x50, 1); ApplyEconomyCommands();
    assert(Cash() == 30000 && ReadSnapshot().status == Status::CareerChanged);

    // Original reset/load/shutdown are retained and invalidate BEFORE their body changes memory.
    Setup(); assert(QueueMoney(900000));
    X86Context ctx{};ctx.esp = 0x10000; sub_002829E0(ctx, fixture::memory.data());
    assert(ctx.esp == 0x1000C && fixture::load_calls == 1);
    ApplyEconomyCommands(); assert(Cash() == 45678 && !ReadSnapshot().pending);
    assert(QueueMoney(800000));ctx.esp=0x10000;sub_002827C0(ctx,fixture::memory.data());
    assert(ctx.esp == 0x10004 && fixture::reset_calls == 1);
    ApplyEconomyCommands();assert(Cash() == 45678);
    assert(QueueMoney(700000));ctx.esp=0x10000;sub_00276E80(ctx,fixture::memory.data());
    assert(ctx.esp == 0x10004 && fixture::shutdown_calls == 1);Unready();

    // The UI can miss the unloaded interval entirely. Its old displayed token
    // must never bind an action to a newly ready career, even with reused data.
    Setup(); const auto prior_token=ReadSnapshot().career_token;
    kernel::W32(fixture::manager+0xC,fixture::other_data);ApplyEconomyCommands();
    assert(ReadSnapshot().ready&&ReadSnapshot().career_token!=prior_token);
    assert(!SetMoney(999999,prior_token));ApplyEconomyCommands();
    assert(Cash()==30000&&Cash(fixture::other_data)==30000);
    Setup();const auto prior_load_token=ReadSnapshot().career_token;
    ctx.esp=0x10000;sub_002829E0(ctx,fixture::memory.data());ApplyEconomyCommands();
    assert(ReadSnapshot().ready&&ReadSnapshot().career_token!=prior_load_token);
    assert(!SetMoney(999999,prior_load_token));ApplyEconomyCommands();assert(Cash()==45678);

    // Every accessed page/type/slot/context is checked; no arbitrary field in an unknown object.
    Setup();kernel::W32(fixture::manager, 0x123456);Unready();
    Setup();kernel::W32(fixture::data, 0x123456);Unready();
    Setup();fixture::missing.insert((fixture::data + 0xCC7) & ~0xFFFu);Unready();
    Setup();fixture::missing.insert(0x557000);Unready();
    Setup();kernel::W32(fixture::manager + 0xC, 0xFFFFFFFC);Unready();
    Setup();kernel::W32(fixture::manager + 0xC, fixture::data + 1);Unready();
    Setup();kernel::W32(fixture::data + 0xACC, 0xFFFFFFFF);Unready();
    Setup();kernel::W32(fixture::data + 0x50, 13);Unready();
    Setup();kernel::G<uint8_t>(0x53F80C) = 0;Unready();
    Setup();kernel::G<uint8_t>(0x53F80C) = 31;Unready();
    Setup();kernel::G<uint8_t>(0x53F80D) = 1;Unready();
    Setup();kernel::W32(0x53F2D4, 0);Unready();
    Setup();kernel::W32(0x53F2D4, 2);ApplyEconomyCommands();assert(ReadSnapshot().ready);
    assert(QueueMoney(900000));kernel::W32(0x53F2D4,0);Unready();
    kernel::W32(0x53F2D4,2);ApplyEconomyCommands();assert(Cash()==30000);
    std::cout << "economy: queued edits, native save-needed query, lifecycle and invalid-memory guards passed\n";
}
