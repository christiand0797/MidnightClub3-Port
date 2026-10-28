#include "economy.h"
#include "kernel/kernel.h"
#include "xdk/hle.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <functional>
#include <iostream>
#include <set>
#include <stdexcept>
#include <thread>
#include <vector>

namespace fixture {
std::vector<uint8_t> memory(0x600000);
std::set<uint32_t> missing;
constexpr uint32_t manager = 0x20000, data = 0x30C00, other_data = 0x40C00;
unsigned reset_calls = 0, load_calls = 0, shutdown_calls = 0;
constexpr uint32_t garage = 0x100000, movie0 = 0x120000, movie1 = 0x121000;
constexpr uint32_t vm0 = 0x130000, vm1 = 0x131000, scratch_top = 0x90000, stack_bytes = 0x10000;
bool scratch_live = false, fail_stack = false, bad_stack_page = false;
unsigned stack_allocations = 0, stack_frees = 0;
unsigned completed_logs = 0, deferred_logs = 0;
struct Call { uint32_t address, self; std::vector<uint32_t> arguments; };
std::vector<Call> calls;
std::function<void(uint32_t)> during_call;
}
namespace kernel { uint8_t* g_base = fixture::memory.data(); }
namespace mem { bool IsCommitted(uint32_t page) {
    if (fixture::bad_stack_page && page == fixture::scratch_top - fixture::stack_bytes) return false;
    return page < fixture::memory.size() && !fixture::missing.contains(page & ~(mem::kPage - 1));
}
uint32_t AllocStack(uint32_t bytes) {
    assert(bytes == fixture::stack_bytes && !fixture::scratch_live);
    if (fixture::fail_stack) return 0;
    fixture::scratch_live = true; ++fixture::stack_allocations;
    std::memset(kernel::g_base + fixture::scratch_top - bytes, 0xAB, bytes);
    return fixture::scratch_top;
}
void FreeStack(uint32_t top) {
    assert(fixture::scratch_live && top == fixture::scratch_top);
    fixture::scratch_live = false; ++fixture::stack_frees;
    std::memset(kernel::g_base + top - fixture::stack_bytes, 0xDD, fixture::stack_bytes);
}
}
namespace logx { void Write(Level, const char* format, ...) {
    if (std::strstr(format, "wallet Flash setters completed")) ++fixture::completed_logs;
    if (std::strstr(format, "wallet refresh deferred")) ++fixture::deferred_logs;
} }

#ifdef MC3_TEST_ORIGINAL_WALLET_HUD
void EconomyOriginalHudCall(X86Context& ctx, uint8_t* base, uint32_t address);
#endif

namespace kernel {
uint32_t CallGuest(X86Context& ctx, uint32_t address, const uint32_t* arguments, size_t count) {
    using namespace fixture;
    assert(scratch_live && ctx.esp == scratch_top);
    assert(address == 0x20CF50 || address == 0x1395E0 || address == 0x139520);
    assert(count == (address == 0x20CF50 ? 4 : 2));
    calls.push_back({address, ctx.ecx, {arguments, arguments + count}});
    const uint32_t esp = ctx.esp;
    for (size_t i = count; i-- > 0;) { ctx.esp -= 4; W32(ctx.esp, arguments[i]); }
    ctx.esp -= 4; W32(ctx.esp, 0xFFFFFFF0);
#ifdef MC3_TEST_ORIGINAL_WALLET_HUD
    EconomyOriginalHudCall(ctx, g_base, address);
#else
    assert(false && "original wallet fixture is required for native-call checks");
#endif
    const uint32_t result = ctx.eax;
    if (during_call) during_call(address);
    // More clobbering than the ordinary bridge deliberately exercises full context isolation.
    ctx.eax = ctx.ecx = ctx.edx = ctx.ebx = ctx.ebp = ctx.esi = ctx.edi = 0xDEADBEEF;
    ctx.cf = ctx.zf = ctx.sf = ctx.of = ctx.pf = ctx.df = 1;
    ctx.top = 7; ctx.st[0] = 1234; ctx.mm[0] = 0xFFFFFFFFFFFFFFFFull; ctx.xmm[0].q[0] = 0xFEDCBA9876543210ull;
    ctx.esp = esp;
    return result;
}
}

X86_FUNC(sub_000640D0) {
    using namespace fixture;
    assert(scratch_live && (ctx.ecx == vm0 || ctx.ecx == vm1));
    const auto name = kernel::R32(ctx.esp + 4);
    assert(name == 0x45A1C0 || name == 0x45A1B4);
    ctx.eax = ctx.ecx + (name == 0x45A1C0 ? 0x50 : 0x60);
    ctx.esp += 8;
}
X86_FUNC(sub_002DCFC5) {
    using namespace fixture;
    assert(scratch_live);
    const auto buffer = kernel::R32(ctx.esp + 4), count = kernel::R32(ctx.esp + 8);
    const auto format = kernel::R32(ctx.esp + 12), args = kernel::R32(ctx.esp + 16);
    assert(buffer == garage + 0x116B && count == 0x81 && format == 0x45987C);
    assert(args >= scratch_top - stack_bytes && args + 4 <= scratch_top);
    // Observe the original formatter's explicit final-NUL reservation before its CRT call.
    assert(base[buffer + count] == 0);
    ctx.eax = uint32_t(std::snprintf(reinterpret_cast<char*>(base + buffer), count,
                                   reinterpret_cast<char*>(base + format), int32_t(kernel::R32(args))));
    ctx.esp += 4; // cdecl: the original formatter removes the four arguments.
}

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
    assert(!fixture::scratch_live);
    std::fill(fixture::memory.begin(), fixture::memory.end(), 0);
    fixture::missing.clear();
    fixture::fail_stack = fixture::bad_stack_page = false;
    fixture::completed_logs = fixture::deferred_logs = 0;
    fixture::calls.clear(); fixture::during_call = {};
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

void WalletObjects() {
    using namespace fixture;
    kernel::W32(0x50F12C, garage);
    kernel::W32(garage, 0x45B860);
    kernel::W32(garage + 0x14, movie0); kernel::W32(garage + 0x18, movie1);
    kernel::W32(movie0, 0x4579F0); kernel::W32(movie1, 0x4579F0);
    kernel::W32(movie0 + 8, vm0); kernel::W32(movie1 + 8, vm1);
    std::memcpy(kernel::g_base + 0x45987C, "$%d.00", 7);
    std::memcpy(kernel::g_base + 0x45A1C0, "Fortune", 8);
    std::memcpy(kernel::g_base + 0x45A1B4, "updatemoney", 12);
}

void WalletTests() {
#ifdef MC3_TEST_ORIGINAL_WALLET_HUD
    using namespace fixture;
    using namespace mc3::economy;
    Setup(90); WalletObjects(); Clean();
    X86Context ctx; std::memset(&ctx, 0x35, sizeof(ctx)); ctx.esp = 0x18000; ctx.ecx = garage;
    const auto original = ctx;
    const auto initial_allocs = stack_allocations, initial_frees = stack_frees;
    assert(!RefreshGarageWallet(ctx, kernel::g_base, garage)); // No edit, no work/allocation.
    assert(stack_allocations == initial_allocs);
    assert(QueueMoney(1000090)); ApplyEconomyCommands();
    const auto before = memory;
    assert(RefreshGarageWallet(ctx, kernel::g_base, garage));
    assert(std::memcmp(&ctx, &original, sizeof(ctx)) == 0);
    assert(std::equal(before.begin() + 0x10000, before.begin() + 0x20000, memory.begin() + 0x10000));
    assert(Cash() == 1000090 && calls.size() == 5 && !scratch_live);
    assert(completed_logs == 1 && deferred_logs == 0);
    assert(stack_allocations == initial_allocs + 1 && stack_frees == initial_frees + 1);
    assert(calls[0].address == 0x20CF50 && calls[0].arguments == std::vector<uint32_t>({garage + 0x116B, 0x82, 0x45987C, 1000090}));
    for (unsigned i = 0; i < 2; ++i) {
        const auto movie = i ? movie1 : movie0, vm = i ? vm1 : vm0;
        assert(calls[1 + i * 2].address == 0x1395E0 && calls[1 + i * 2].self == movie);
        assert(calls[1 + i * 2].arguments == std::vector<uint32_t>({0x45A1C0, garage + 0x116B}));
        assert(calls[2 + i * 2].address == 0x139520 && calls[2 + i * 2].self == movie);
        assert(calls[2 + i * 2].arguments == std::vector<uint32_t>({0x45A1B4, 1}));
        assert(kernel::R32(vm + 0x50) == garage + 0x116B && kernel::R32(vm + 0x54) == 1);
        assert(kernel::R32(vm + 0x60) == 1 && kernel::R32(vm + 0x64) == 3);
    }
    assert(std::strcmp(reinterpret_cast<char*>(kernel::g_base + garage + 0x116B), "$1000090.00") == 0);
    assert(!RefreshGarageWallet(ctx, kernel::g_base, garage) && calls.size() == 5); // One shot.
    assert(completed_logs == 1 && deferred_logs == 0);
    // No balance/dirty/save/spending changes occur during presentation refresh.
    for (size_t at = 0; at < memory.size(); ++at) if (memory[at] != before[at]) {
        const bool buffer = at >= garage + 0x116B && at < garage + 0x116B + 0x82;
        const bool scratch = at >= scratch_top - stack_bytes && at < scratch_top;
        const bool values = (at >= vm0 + 0x50 && at < vm0 + 0x68) || (at >= vm1 + 0x50 && at < vm1 + 0x68);
        assert(buffer || scratch || values);
    }
    auto pending = [&] { Setup(90); WalletObjects(); assert(QueueMoney(1000090)); ApplyEconomyCommands(); };
    auto refused = [&] { const auto all = memory; assert(!RefreshGarageWallet(ctx, kernel::g_base, garage)); assert(calls.empty() && memory == all && !scratch_live); };
    pending(); kernel::W32(garage, 0x123456); refused();
    pending(); kernel::W32(0x50F12C, garage + 4); refused();
    const auto reported = deferred_logs;
    refused(); assert(deferred_logs == reported); // Same rejected revision/reason never logs every frame.
    kernel::W32(0x50F12C, garage); assert(RefreshGarageWallet(ctx, kernel::g_base, garage));
    assert(completed_logs == 1);
    pending(); missing.insert(0x50F000); refused();
    pending(); kernel::W32(movie0, 0x123456); refused();
    pending(); kernel::W32(movie1 + 8, 0); refused();
    pending(); kernel::W32(garage + 0x18, movie0); refused();
    pending(); missing.insert((garage + 0x116B) & ~0xFFFu); refused();
    pending(); missing.insert(vm0 & ~0xFFFu); refused();
    pending(); kernel::W32(0x53F2D4, 2); refused();
    pending(); kernel::W32(manager + 0xC, other_data); refused();
    pending(); kernel::W32(data + 0x50, 1); refused();
    pending(); kernel::g_base[0x45A1B4] = '?'; refused();
    pending(); fail_stack = true; refused(); fail_stack = false;
    assert(RefreshGarageWallet(ctx, kernel::g_base, garage)); // Allocation failure did not consume the edit.
    pending(); bad_stack_page = true;
    assert(!RefreshGarageWallet(ctx, kernel::g_base, garage) && calls.empty() && !scratch_live); bad_stack_page = false;
    assert(RefreshGarageWallet(ctx, kernel::g_base, garage));
    pending(); assert(!RefreshGarageWallet(ctx, kernel::g_base + 1, garage) && calls.empty());
    assert(RefreshGarageWallet(ctx, kernel::g_base, garage));
    // A legitimate purchase before display uses the current wallet, never the old requested balance.
    pending(); kernel::W32(data + 0xACC, 1234);
    assert(RefreshGarageWallet(ctx, kernel::g_base, garage) && Cash() == 1234);
    assert(std::strcmp(reinterpret_cast<char*>(kernel::g_base + garage + 0x116B), "$1234.00") == 0);
    // Same-allocation load invalidates the pending display as well as the pending edit.
    pending(); X86Context load{}; load.esp = 0x10000; sub_002829E0(load, kernel::g_base);
    ApplyEconomyCommands(); assert(!RefreshGarageWallet(ctx, kernel::g_base, garage) && calls.empty());
    // Simulated lifecycle/VM replacement during formatting cannot replay stale setters.
    pending(); during_call = [&](uint32_t fn) { if (fn == 0x20CF50) kernel::W32(movie1 + 8, vm1 + 0x1000); };
    assert(!RefreshGarageWallet(ctx, kernel::g_base, garage) && calls.size() == 1 && !scratch_live);
    assert(kernel::R32(vm0 + 0x50) == 0 && kernel::R32(vm1 + 0x50) == 0);
    assert(std::memcmp(&ctx, &original, sizeof(ctx)) == 0);
    pending(); during_call = [&](uint32_t fn) { if (fn == 0x20CF50) kernel::W32(manager + 0xC, other_data); };
    assert(!RefreshGarageWallet(ctx, kernel::g_base, garage) && calls.size() == 1 && Cash(other_data) == 90);
    during_call = {};
    pending(); during_call = [](uint32_t) { throw std::runtime_error("fixture original-call failure"); };
    bool caught = false;
    try { RefreshGarageWallet(ctx, kernel::g_base, garage); }
    catch (const std::runtime_error&) { caught = true; }
    assert(caught && !scratch_live && std::memcmp(&ctx, &original, sizeof(ctx)) == 0);
    during_call = {};
    assert(RefreshGarageWallet(ctx, kernel::g_base, garage));
#endif
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
    WalletTests();
    std::cout << "economy: queued edits, native save-needed query, lifecycle and invalid-memory guards passed\n";
}
