// Actual garage hooks, with the original guest bodies and Flash/database APIs replaced by bounded fixtures.
#include "app/app.h"
#include "xdk/hle.h"
#include <cassert>
#include <cmath>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace fixture {
std::vector<uint8_t> memory(0x600000);
uint32_t next_string = 0x80000;
constexpr uint32_t stack = 0x18000, db = 0x20000, garage = 0x100000, flash = 0x120000;
constexpr uint32_t vm = 0x121000, vtable = 0x123000, member = 0x124000;
struct Value { uint32_t payload = 0, type = 1; };
struct Array { uint32_t value, object; std::vector<Value> items; std::set<size_t> members; };
std::map<std::string, Array> arrays;
std::vector<uint32_t> rides, widths;
std::map<uint32_t, bool> players;
unsigned stock_ride_calls = 0, stock_available_calls = 0, cfg_applied_calls = 0;
unsigned member_calls = 0;
constexpr uint32_t globals[] = {0x5014D0, 0x5014D8, 0x5014D4, 0x501410, 0x501420, 0x50142C};

void Pool(unsigned count) {
    constexpr uint32_t nodes = 0x125000;
    kernel::W32(vm + 0x24, count ? nodes : 0);
    for (unsigned i = 0; i < count; ++i)
        kernel::W32(nodes + i * 16, i + 1 < count ? nodes + (i + 1) * 16 : 0);
}

uint32_t String(const std::string& s) {
    uint32_t va = next_string;
    assert(size_t(va) + s.size() + 1 < memory.size());
    std::memcpy(memory.data() + va, s.c_str(), s.size() + 1);
    next_string += uint32_t(s.size() + 1);
    return va;
}
void Inline(uint32_t va, const std::string& s) { std::memcpy(memory.data() + va, s.c_str(), s.size() + 1); }
void Database() {
    for (auto row : {std::pair{0x100u, &rides}, std::pair{0xF8u, &widths}}) {
        uint32_t data = row.first == 0x100 ? 0x25000 : 0x26000;
        kernel::W32(db + row.first, data);
        kernel::W16(db + row.first + 4, uint16_t(row.second->size()));
        for (size_t i = 0; i < row.second->size(); ++i) kernel::W32(data + uint32_t(i) * 4, (*row.second)[i]);
    }
}
void AddArray(const std::string& name, std::vector<Value> values) {
    uint32_t id = uint32_t(arrays.size());
    Array a{0x27000 + id * 16, 0x28000 + id * 16, std::move(values), {}};
    for (size_t i = 0; i < a.items.size(); ++i) a.members.insert(i);
    kernel::W32(a.value, a.object);
    kernel::W32(a.value + 4, 5);
    kernel::W32(a.object, vtable);
    arrays[name] = std::move(a);
}
using Hook = void (*)(X86Context&, uint8_t*);
X86Context Call(Hook hook, uint32_t self = 0, std::initializer_list<uint32_t> args = {}) {
    X86Context ctx{}; ctx.ecx = self; ctx.esp = stack;
    uint32_t at = stack + 4;
    for (uint32_t arg : args) { kernel::W32(at, arg); at += 4; }
    hook(ctx, memory.data());
    return ctx;
}
}
namespace app { Settings& Get() { static Settings settings; return settings; } }
namespace logx { void Write(Level, const char*, ...) {} }
namespace mem { bool IsCommitted(uint32_t va) { return va >= 0x10000 && va < fixture::memory.size(); } }
namespace kernel {
uint8_t* g_base = fixture::memory.data();
std::string ReadCString(uint32_t va, size_t max) {
    std::string s;
    while (s.size() < max && uint64_t(va) + s.size() < fixture::memory.size()) {
        char c = char(fixture::memory[va + s.size()]); if (!c) break; s += c;
    }
    return s;
}
uint32_t CallGuest(X86Context& ctx, uint32_t fn, const uint32_t* a, size_t count) {
    using namespace fixture;
    if (fn == 0x1B3B90) { assert(count == 1 && a[0] == 0x2000); return 0x30000; }
    if (fn == 0x274A80 || fn == 0x2749C0) {
        assert(ctx.ecx == db && count == 1);
        (fn == 0x274A80 ? rides : widths).push_back(a[0]); Database(); return 0;
    }
    if (fn == member) {
        assert(count == 2);
        ++member_calls;
        const uint32_t expected[] = {vm + 0x10, vm + 0x1C, vm + 0x28, vm + 0x34, R32(vm + 0x40), R32(vm + 0x44)};
        for (unsigned i = 0; i < 6; ++i) assert(R32(globals[i]) == expected[i]);
        for (auto& [name, ar] : arrays) if (ar.object == ctx.ecx) {
            const auto key = ReadCString(a[0]); Value value{R32(a[1]), R32(a[1] + 4)};
            if (key == "length") ar.items.resize(value.payload);
            else {
                auto i = std::stoul(key); assert(i < ar.items.size());
                if (ar.members.insert(i).second) {
                    const uint32_t free = R32(vm + 0x24);
                    assert(free); W32(vm + 0x24, R32(free));
                }
                ar.items[i] = value;
            }
            return 0;
        }
        assert(false);
    }
    assert(count >= 1);
    const auto name = ReadCString(a[0]);
    auto it = arrays.find(name);
    if (fn == 0x1395F0) return it == arrays.end() ? uint32_t(-1) : uint32_t(it->second.items.size());
    if (fn == 0x064060) return it == arrays.end() ? 0 : it->second.value;
    assert(it != arrays.end()); auto& ar = it->second;
    if (fn == 0x139650) {
        assert(count == 3); if (a[1] >= ar.items.size()) return 0;
        W32(a[2], ar.items[a[1]].payload); W32(a[2] + 4, ar.items[a[1]].type); return 1;
    }
    if (fn == 0x139540) {
        assert(count == 3 && a[1] < ar.items.size());
        if (ar.members.insert(a[1]).second) {
            const uint32_t free = R32(vm + 0x24); assert(free); W32(vm + 0x24, R32(free));
        }
        ar.items[a[1]] = {a[2], 1}; return 1;
    }
    assert(false); return 0;
}
}
void TuningSetPlayerSim(uint32_t sim, bool player) { fixture::players[sim] = player; }
void TuningForgetSim(uint32_t sim) { fixture::players.erase(sim); }
void TuningBindCar(uint32_t, uint32_t, uint32_t, bool) {}
namespace app { void GarageTuningActive() {} }
void TuningCarCfgApplied(uint32_t) { ++fixture::cfg_applied_calls; }

X86_FUNC(__imp__sub_00270C10) { ctx.esp += 4; }
X86_FUNC(__imp__sub_00270BE0) { ++fixture::stock_ride_calls; x87_push(ctx, 0.025); ctx.esp += 8; }
X86_FUNC(__imp__sub_00272FC0) { x87_push(ctx, 0.195); ctx.esp += 8; }
X86_FUNC(__imp__sub_00270C00) { x87_push(ctx, 0.195); ctx.esp += 4; }
X86_FUNC(__imp__sub_00145DC0) { ++fixture::stock_available_calls; ctx.eax = 0; ctx.esp += 12; }
X86_FUNC(__imp__sub_001465E0) { ctx.esp += 8; }
X86_FUNC(__imp__sub_001519D0) { ctx.esp += 4; }
X86_FUNC(__imp__sub_00121970) { ctx.ecx = 0; ctx.esp += 12; }
X86_FUNC(__imp__sub_002CBDE0) { ctx.ecx = 0; ctx.esp += 4; }
X86_FUNC(__imp__sub_001FAE10) { ctx.ecx = 0; ctx.esp += 4; }
X86_FUNC(__imp__sub_000FFBA0) { ctx.esp += 4; }
#define DECLARE(name) X86_EXTERN_FUNC(sub_##name)
DECLARE(00270C10); DECLARE(00270BE0); DECLARE(00272FC0); DECLARE(00270C00); DECLARE(00145DC0);
DECLARE(001465E0); DECLARE(00121970); DECLARE(002CBDE0); DECLARE(001FAE10); DECLARE(000FFBA0);

int main() {
    using namespace fixture; using kernel::W32;
    W32(0x557F04, db);
    for (int i = 0; i < 15; ++i) {
        auto key = i < 9 ? "rh_" + std::to_string(i * 100) : "rh+" + std::to_string((15 - i) * 100);
        rides.push_back(String(key)); W32(0x471A54 + i * 4, rides.back());
    }
    for (int i = 0; i < 9; ++i) { widths.push_back(String("tw_" + std::to_string(195 + i * 20))); W32(0x471ADC + i * 4, widths.back()); }
    for (int i = 0; i < 17; ++i) W32(0x4719C8 + i * 4, String("rim_" + std::to_string(12 + i)));
    Database();
    Call(sub_00270C10);
    assert(rides.size() == 29 && widths.size() == 38);
    assert(kernel::ReadCString(rides[15]) == "rh_900" && kernel::ReadCString(rides[28]) == "rh+1200");
    Call(sub_00270C10); assert(rides.size() == 29 && widths.size() == 38); // reinitialization never duplicates indices
    assert(kernel::ReadCString(widths[15]) == "tw_155" && kernel::ReadCString(widths[22]) == "tw_555");
    assert(kernel::ReadCString(widths[23]) == "tw_105" && kernel::ReadCString(widths[37]) == "tw_755");
    constexpr uint32_t cfg = 0x150000;
    auto ride = [&](uint8_t i) { fixture::memory[cfg + 0x16] = i; return Call(sub_00270BE0, cfg, {0}); };
    auto ctx = ride(1); assert(stock_ride_calls == 1 && std::abs(ctx.st[ctx.top] - 0.025) < 1e-6);
    ctx = ride(15); assert(ctx.esp == stack + 8 && std::abs(ctx.st[ctx.top] + 0.175) < 1e-6);
    ctx = ride(28); assert(std::abs(ctx.st[ctx.top] - 0.275) < 1e-6);
    app::Get().tuning.garage_extended = false;
    ctx = ride(28); assert(std::abs(ctx.st[ctx.top] - 0.275) < 1e-6); // existing saves keep their interpretation
    fixture::memory[cfg + 0x18] = 15;
    ctx = Call(sub_00272FC0, cfg, {0}); assert(std::abs(ctx.st[ctx.top] - 0.155) < 1e-6);
    fixture::memory[cfg + 0x18] = 37;
    ctx = Call(sub_00272FC0, cfg, {0}); assert(std::abs(ctx.st[ctx.top] - 0.755) < 1e-6);
    ctx = Call(sub_00270C00, 0, {23}); assert(std::abs(ctx.st[ctx.top] - 0.105) < 1e-6);

    W32(garage + 0x18, flash); W32(flash + 8, vm); W32(vtable + 0x24, member);
    W32(vm + 0x40, 0x127000); W32(vm + 0x44, 0x128000);
    for (unsigned i = 0; i < 6; ++i) W32(globals[i], 0x90000 + i * 16);
    Pool(100);
    Inline(garage + 0x3E4, "RimSizeNames"); Inline(garage + 0x416, "RimSizeStatus");
    W32(garage + 0x9CC, 0x18); Inline(0x44C498, "length");
    for (unsigned i = 0; i < 128; ++i) Inline(0x44C4C0 + i * 4, std::to_string(i));
    std::vector<Value> names;
    for (unsigned i = 0; i < 21; ++i) names.push_back({kernel::R32(0x4719C8 + (i % 17) * 4), 1});
    AddArray("RimSizeNames", names); AddArray("RimSizeStatus", {{String("2"), 1}}); AddArray("RimSizePrice", {{100, 3}});
    app::Get().tuning.garage_extended = true;
    Call(sub_001465E0, garage, {0}); // longer list of known names: no read beyond the 17 expected entries
    assert(arrays["RimSizeNames"].items.size() == 17);
    assert(kernel::ReadCString(arrays["RimSizeNames"].items[0].payload) == "rim_28");
    assert(arrays["RimSizeStatus"].items.size() == 17 && arrays["RimSizePrice"].items[16].payload == 100);
    for (unsigned i = 0; i < 6; ++i) assert(kernel::R32(globals[i]) == 0x90000 + i * 16);
    ctx = Call(sub_00145DC0, garage, {0, 0}); assert(ctx.eax == 1 && stock_available_calls == 0);
    app::Get().tuning.garage_extended = false;
    ctx = Call(sub_00145DC0, garage, {0, 0}); assert(ctx.eax == 0 && stock_available_calls == 1);

    // All three Flash arrays must grow together. A full symbol pool leaves their old contents untouched.
    Inline(garage + 0x3E4, "TiresWidthNames"); Inline(garage + 0x416, "TiresWidthStatus");
    W32(garage + 0x9CC, 0x1A);
    AddArray("TiresWidthNames", {{widths[0], 1}});
    AddArray("TiresWidthStatus", {{String("2"), 1}});
    AddArray("TiresWidthPrice", {{75, 3}});
    app::Get().tuning.garage_extended = true;
    Pool(126); // 3*37 new symbols + a 16-symbol reserve = 127 required.
    const unsigned before = member_calls;
    Call(sub_001465E0, garage, {0});
    assert(arrays["TiresWidthNames"].items.size() == 1 && member_calls == before);
    Pool(160);
    Call(sub_001465E0, garage, {0});
    assert(arrays["TiresWidthNames"].items.size() == 38 && arrays["TiresWidthStatus"].items.size() == 38);
    assert(kernel::ReadCString(arrays["TiresWidthNames"].items.front().payload) == "tw_755");
    assert(kernel::ReadCString(arrays["TiresWidthNames"].items.back().payload) == "tw_105");
    assert(arrays["TiresWidthPrice"].items.back().payload == 75);
    for (unsigned i = 0; i < 6; ++i) assert(kernel::R32(globals[i]) == 0x90000 + i * 16);

    // Known AI ownership takes precedence when a preview and a human's car share a CarCfg.
    constexpr uint32_t human = 0x140000, ai = 0x141000, human_sim = 0x160000, ai_sim = 0x161000, preview = 0x162000;
    for (auto c : {std::pair{human, human_sim}, std::pair{ai, ai_sim}}) { W32(c.first + 0x50, c.second); W32(c.first + 0x64, cfg); }
    W32(0x170044, human); W32(0x171044, ai);
    Call(sub_002CBDE0, 0x170000); Call(sub_001FAE10, 0x171000);
    assert(players[human_sim] && !players[ai_sim]);
    Call(sub_00121970, ai_sim, {cfg, 0}); assert(!players[ai_sim]);
    Call(sub_00121970, preview, {cfg, 0}); assert(players[preview]);
    Call(sub_000FFBA0, human); assert(!players.contains(human_sim) && !players[preview]);
    Call(sub_00121970, preview, {cfg, 0}); assert(!players[preview]);
    Call(sub_000FFBA0, ai); assert(!players.contains(ai_sim));
    assert(cfg_applied_calls == 3);

    // Entering the garage directly creates a preview without invoking the human player factory.
    constexpr uint32_t garage_car = 0x142000, garage_sim = 0x163000, garage_cfg = 0x151000;
    W32(garage_car + 0x50, garage_sim); W32(garage_car + 0x64, garage_cfg);
    W32(garage + 0x714, garage_car); W32(garage + 0x72C, 0);
    Call(sub_001465E0, garage, {0});
    assert(players[garage_sim] && cfg_applied_calls == 4);
    Call(sub_001465E0, garage, {0}); // repeated refresh cannot rebase the same preview's F1 offset again
    assert(cfg_applied_calls == 4);
    Call(sub_000FFBA0, garage_car); assert(!players.contains(garage_sim));

    // The garage pointer does not authorize promoting a concrete computer owner.
    Call(sub_001FAE10, 0x171000); // ai factory from the preceding scenario
    W32(garage + 0x714, ai);
    Call(sub_001465E0, garage, {0});
    assert(!players[ai_sim] && cfg_applied_calls == 4);
    Call(sub_000FFBA0, ai);

    // A destroyed simulation's address may be reused by another car while its old shared cfg still lives.
    // Retiring the final old cfg owner must not accidentally retire the new car at that recycled address.
    constexpr uint32_t car_a = 0x143000, car_b = 0x144000, car_c = 0x145000;
    constexpr uint32_t sim_a = 0x164000, sim_b = 0x165000, cfg_x = 0x152000, cfg_y = 0x153000;
    for (auto row : {std::pair{car_a, sim_a}, std::pair{car_b, sim_b}}) {
        W32(row.first + 0x50, row.second); W32(row.first + 0x64, cfg_x);
    }
    W32(0x172044, car_a); W32(0x173044, car_b);
    Call(sub_002CBDE0, 0x172000); Call(sub_002CBDE0, 0x173000);
    Call(sub_00121970, sim_a, {cfg_x, 0}); Call(sub_00121970, sim_b, {cfg_x, 0});
    Call(sub_000FFBA0, car_a); assert(!players.contains(sim_a));
    W32(car_c + 0x50, sim_a); W32(car_c + 0x64, cfg_y); W32(0x174044, car_c);
    Call(sub_002CBDE0, 0x174000); Call(sub_00121970, sim_a, {cfg_y, 0});
    assert(players[sim_a]);
    Call(sub_000FFBA0, car_b); assert(players[sim_a]);
    Call(sub_000FFBA0, car_c); assert(!players.contains(sim_a));
}
