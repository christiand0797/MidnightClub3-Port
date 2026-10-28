// Actual garage hooks, with the original guest bodies and Flash/database APIs replaced by bounded fixtures.
#include "app/app.h"
#include "xdk/hle.h"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <map>
#include <set>
#include <string>
#include <vector>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace fixture {
std::vector<uint8_t> memory(0x600000);
uint32_t next_string = 0x80000;
constexpr uint32_t stack = 0x18000, db = 0x20000, garage = 0x100000, flash = 0x120000;
constexpr uint32_t vm = 0x121000, vtable = 0x123000, member = 0x124000;
struct Value { uint32_t payload = 0, type = 1; };
struct Array { uint32_t value, object; std::vector<Value> items; std::set<size_t> members; };
std::map<std::string, Array> arrays;
std::vector<uint32_t> rides, widths, profiles;
std::map<uint32_t, bool> players;
struct Binding { uint32_t sim, cfg; bool player; };
std::map<uint32_t, Binding> bindings;
unsigned bind_calls = 0;
#ifdef _WIN32
bool sparse_guest = false;
#endif
unsigned stock_ride_calls = 0, stock_available_calls = 0, cfg_applied_calls = 0;
unsigned original_garage_updates = 0, wallet_observations = 0;
unsigned member_calls = 0;
constexpr uint32_t globals[] = {0x5014D0, 0x5014D8, 0x5014D4, 0x501410, 0x501420, 0x50142C};
constexpr uint32_t string_table = 0x507E08;
std::map<uint32_t, uint32_t> allocations;
std::map<std::string, uint32_t> text_entries;
uint32_t next_allocation = 0x300000;
unsigned frees = 0;
bool fail_label_allocation = false;
bool fail_profile_allocation = false;
void (*profile_resource_load)() = nullptr;
void (*profile_resource_unload)() = nullptr;

uint32_t Allocate(uint32_t bytes, uint32_t reuse = 0) {
    const uint32_t va = reuse ? reuse : next_allocation;
    assert(bytes && uint64_t(va) + bytes < memory.size() && !allocations.contains(va));
    if (!reuse) next_allocation += (bytes + 15) & ~15u;
    allocations[va] = bytes;
    std::memset(memory.data() + va, 0, bytes);
    return va;
}
void Free(uint32_t va) {
    if (!va) return;
    // The native allocator crashed interpreting UTF-16 bytes before an interior pointer as heap metadata.
    // Rejecting every non-base pointer makes that same ownership error deterministic in this fixture.
    assert(allocations.contains(va) && "string-table teardown freed an interior or already freed pointer");
    std::memset(memory.data() + va, 0xDD, allocations.at(va));
    allocations.erase(va);
    ++frees;
}
uint32_t OwnedWide(const std::string& text) {
    const uint32_t va = Allocate(uint32_t(text.size() + 1) * 2);
    for (size_t i = 0; i < text.size(); ++i) kernel::W16(va + uint32_t(i) * 2, uint8_t(text[i]));
    return va;
}
std::string Wide(uint32_t va) {
    std::string text;
    for (uint32_t i = 0; i < 160 && kernel::R16(va + 2 * i); ++i) text += char(kernel::R16(va + 2 * i));
    return text;
}
uint32_t TextTable(uint32_t reuse = 0) {
    const uint32_t table = Allocate(8, reuse), buckets = Allocate(4);
    kernel::W32(table, 1); kernel::W32(table + 4, buckets);
    kernel::W32(string_table, table); kernel::W32(string_table + 0x28, 0);
    text_entries.clear();
    return table;
}
uint32_t TextEntry(const std::string& key) {
    if (text_entries.contains(key)) return text_entries.at(key);
    const uint32_t entry = Allocate(0x18), key_text = Allocate(uint32_t(key.size() + 1));
    std::memcpy(memory.data() + key_text, key.c_str(), key.size() + 1);
    kernel::W32(entry + 4, key_text | 1); // the real destructor frees owned keys whose low bit is set
    kernel::W32(entry + 8, OwnedWide(key == "tw_375" ? "Localized width" : "!!" + key + "!!"));
    const uint32_t buckets = kernel::R32(kernel::R32(string_table) + 4), node = Allocate(12);
    kernel::W32(node + 4, entry); kernel::W32(node + 8, kernel::R32(buckets)); kernel::W32(buckets, node);
    text_entries[key] = entry;
    return entry;
}

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
    for (auto row : {std::pair{0x100u, &rides}, std::pair{0xF8u, &widths}, std::pair{0xF0u, &profiles}}) {
        uint32_t data = row.first == 0x100 ? 0x25000 : row.first == 0xF8 ? 0x26000 : 0x26400;
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
namespace mc3::economy { bool RefreshGarageWallet(X86Context& context, uint8_t* base, uint32_t garage) {
    assert(base == fixture::memory.data() && garage == fixture::garage);
    assert(fixture::original_garage_updates == 1 && context.ecx == 0xABCD && context.eax == 123);
    assert(context.esp == fixture::stack + 4);
    ++fixture::wallet_observations;
    return false;
} }
namespace logx { void Write(Level, const char*, ...) {} }
namespace mem { bool IsCommitted(uint32_t va) {
#ifdef _WIN32
    if (fixture::sparse_guest) {
        MEMORY_BASIC_INFORMATION region{};
        return VirtualQuery(kernel::g_base + va, &region, sizeof(region)) && region.State == MEM_COMMIT;
    }
#endif
    return va >= 0x10000 && va < fixture::memory.size();
} }
namespace mem {
uint32_t AllocSystem(uint32_t size, uint32_t) {
    return fixture::fail_profile_allocation ? 0 : fixture::Allocate(size);
}
void FreeSystem(uint32_t va) { fixture::Free(va); }
}
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
    if (fn == 0x1B3B90) {
        assert(count == 1);
        if (a[0] == 0x8000 && !allocations.contains(0x30000)) return Allocate(a[0], 0x30000);
        if (fail_label_allocation) { fail_label_allocation = false; return 0; }
        return Allocate(a[0]);
    }
    if (fn == 0x1B3C00) { assert(count == 1); Free(a[0]); return 0; }
    if (fn == 0x0EA750) { assert(ctx.ecx == string_table && count == 1); return TextEntry(ReadCString(a[0])); }
    if (fn == 0x274A80 || fn == 0x2749C0 || fn == 0x274980) {
        assert(ctx.ecx == db && count == 1);
        (fn == 0x274A80 ? rides : fn == 0x2749C0 ? widths : profiles).push_back(a[0]); Database(); return 0;
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
void TuningForgetSim(uint32_t sim) {
    fixture::players.erase(sim);
    std::erase_if(fixture::bindings, [sim](const auto& item) { return item.second.sim == sim; });
}
void TuningBindCar(uint32_t car, uint32_t sim, uint32_t cfg, bool player) {
    ++fixture::bind_calls;
    if (player) fixture::bindings[car] = {sim, cfg, player};
    else fixture::bindings.erase(car);
}
namespace app { void GarageTuningActive() {} void EndGarageNavigation() {} }
void TuningCarCfgApplied(uint32_t) { ++fixture::cfg_applied_calls; }

X86_FUNC(__imp__sub_00270C10) { ctx.esp += 4; }
X86_FUNC(__imp__sub_000F5E70) {
    if (fixture::profile_resource_load) fixture::profile_resource_load();
    ctx.esp += 4;
}
X86_FUNC(__imp__sub_000F5270) {
    if (fixture::profile_resource_unload) fixture::profile_resource_unload();
    ctx.esp += 4;
}
#ifndef MC3_TEST_PROFILE_MESH_FIXTURE
X86_FUNC(__imp__sub_000F11C0) { x87_push(ctx, 0); ctx.esp += 8; }
#endif
X86_FUNC(__imp__sub_00270BE0) { ++fixture::stock_ride_calls; x87_push(ctx, 0.025); ctx.esp += 8; }
X86_FUNC(__imp__sub_00272FC0) { x87_push(ctx, 0.195); ctx.esp += 8; }
X86_FUNC(__imp__sub_00270C00) { x87_push(ctx, 0.195); ctx.esp += 4; }
#ifndef MC3_TEST_REAL_PROFILE_READERS
X86_FUNC(__imp__sub_00270B40) { x87_push(ctx, 0); ctx.esp += 4; }
X86_FUNC(__imp__sub_00270B60) { ctx.eax = 0; ctx.esp += 4; }
X86_FUNC(__imp__sub_00270BC0) { x87_push(ctx, 0); ctx.esp += 4; }
X86_FUNC(__imp__sub_00272FE0) { x87_push(ctx, 0); ctx.esp += 12; }
#else
X86_EXTERN_FUNC(FixtureGarageSetProfile);
#endif
X86_FUNC(__imp__sub_00145DC0) { ++fixture::stock_available_calls; ctx.eax = 0; ctx.esp += 12; }
X86_FUNC(__imp__sub_001465E0) { ctx.esp += 8; }
X86_FUNC(__imp__sub_001519D0) {
    ++fixture::original_garage_updates; ctx.ecx = 0xABCD; ctx.eax = 123; ctx.esp += 4;
}
X86_FUNC(__imp__sub_00121970) { ctx.ecx = 0; ctx.esp += 12; }
X86_FUNC(__imp__sub_002CBDE0) { ctx.ecx = 0; ctx.esp += 4; }
X86_FUNC(__imp__sub_001FAE10) { ctx.ecx = 0; ctx.esp += 4; }
X86_FUNC(__imp__sub_000FFBA0) { ctx.esp += 4; }
#ifndef MC3_TEST_REAL_STRING_DESTRUCTOR
X86_FUNC(__imp__sub_000EA950) { ctx.esp += 4; }
#else
X86_FUNC(sub_001B3C00) { fixture::Free(kernel::R32(ctx.esp + 4)); ctx.esp += 4; }
X86_FUNC(sub_000EC160) { assert(false && "unexpected pooled key in string-table fixture"); }
X86_FUNC(sub_000EC600) { fixture::Free(kernel::R32(ctx.ecx + 4)); ctx.esp += 4; }
X86_EXTERN_FUNC(FixtureGarageLeaveForSave);
#endif
#define DECLARE(name) X86_EXTERN_FUNC(sub_##name)
DECLARE(00270C10); DECLARE(00270BE0); DECLARE(00272FC0); DECLARE(00270C00); DECLARE(00145DC0);
DECLARE(001465E0); DECLARE(00121970); DECLARE(002CBDE0); DECLARE(001FAE10); DECLARE(000FFBA0);
DECLARE(00270B40); DECLARE(00270B60); DECLARE(00270BC0); DECLARE(00272FE0);
DECLARE(001519D0);
void GarageRefreshPlayerCar(uint32_t car);

int main() {
    using namespace fixture; using kernel::W32;
    const auto updated = Call(sub_001519D0, garage);
    assert(original_garage_updates == 1 && wallet_observations == 1);
    assert(updated.ecx == 0xABCD && updated.eax == 123 && updated.esp == stack + 4);
    W32(0x557F04, db);
    for (int i = 0; i < 15; ++i) {
        auto key = i < 9 ? "rh_" + std::to_string(i * 100) : "rh+" + std::to_string((15 - i) * 100);
        rides.push_back(String(key)); W32(0x471A54 + i * 4, rides.back());
    }
    for (int i = 0; i < 9; ++i) { widths.push_back(String("tw_" + std::to_string(195 + i * 20))); W32(0x471ADC + i * 4, widths.back()); }
    for (int i = 0; i < 17; ++i) W32(0x4719C8 + i * 4, String("rim_" + std::to_string(12 + i)));
    constexpr float stock_ratios[] = {.415532f, .47585f, .538852f, .6f, .66f, .725f, .782f, .83f};
    for (unsigned i = 0; i < 8; ++i) {
        profiles.push_back(String("Profile_" + std::to_string(i)));
        W32(0x471938 + i * 4, profiles.back());
        kernel::G<float>(0x471958 + i * 4) = stock_ratios[i];
    }
    constexpr float stock_rim_values[] = {.381f, .4064f, .4318f, .4572f, .4826f, .508f, .5334f, .5588f,
        .5842f, .6096f, .635f, .6604f, .6858f, .7112f, .7366f, .762f, .7874f}; // exact original calibrated table
    for (unsigned rim = 12; rim <= 28; ++rim) kernel::G<float>(0x4B3910 + rim * 4) = stock_rim_values[rim - 12];
    kernel::G<float>(0x447C18) = .5f;
    kernel::G<float>(0x44CE68) = 10000.0f; // original automatic-profile search's initial error
    Database();
    Call(sub_00270C10);
    assert(rides.size() == 29 && widths.size() == 38 && profiles.size() == 34);
    assert(kernel::ReadCString(rides[15]) == "rh_900" && kernel::ReadCString(rides[28]) == "rh+1200");
    Call(sub_00270C10); assert(rides.size() == 29 && widths.size() == 38 && profiles.size() == 34); // no duplicate codes
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

    // Every new profile, every original rim size and both independent axle bytes agree in physical/visual size.
    // Turning off the extended menu must not reinterpret a car saved with an extended profile.
    assert(kernel::ReadCString(profiles[8]) == "Profile_PC_300" && kernel::ReadCString(profiles[33]) == "Profile_PC_975");
    for (unsigned profile = 8; profile < profiles.size(); ++profile) {
        const float ratio = std::stof(kernel::ReadCString(profiles[profile]).substr(11)) / 1000;
        ctx = Call(sub_00270BC0, 0, {profile});
        assert(ctx.esp == stack + 4 && std::abs(ctx.st[ctx.top] - ratio) < 1e-6);
        for (unsigned rim = 12; rim <= 28; ++rim) {
            const double expected = stock_rim_values[rim - 12] / ratio;
            const auto visual = Call(sub_00270B40, 0, {rim, profile});
            assert(visual.esp == stack + 4 && std::abs(visual.st[visual.top] - expected) < 1e-6);
            for (unsigned axle = 0; axle < 2; ++axle) {
                fixture::memory[cfg + 0x1A + axle] = uint8_t(rim);
                fixture::memory[cfg + 0x1C + axle] = uint8_t(profile);
                const auto physical = Call(sub_00272FE0, cfg, {axle, 0});
                assert(physical.esp == stack + 12 && std::abs(physical.st[physical.top] - expected) < 1e-6);
                assert(std::abs(physical.st[physical.top] - visual.st[visual.top]) < 1e-7);
            }
        }
    }
    // A malformed signed profile byte falls back to the last stock ratio instead of reading unrelated memory.
    fixture::memory[cfg + 0x1A] = 20; fixture::memory[cfg + 0x1C] = 254;
    ctx = Call(sub_00272FE0, cfg, {0, 0});
    assert(std::abs(ctx.st[ctx.top] - .5842f / stock_ratios[7]) < 1e-6);
    for (uint32_t profile : {34u, 127u, 128u, 254u, 0xFFFFFFFFu}) {
        ctx = Call(sub_00270BC0, 0, {profile});
        assert(std::abs(ctx.st[ctx.top] - stock_ratios[7]) < 1e-6);
    }
    auto bits = [](float value) { uint32_t out; std::memcpy(&out, &value, 4); return out; };
    ctx = Call(sub_00270B60, 0, {33, 20, 15, bits(0), bits(2), bits(1)});
    assert((ctx.eax & 255) == 1 && ctx.esp == stack + 4);
    ctx = Call(sub_00270B60, 0, {8, 28, 0, bits(0), bits(2), bits(.5f)});
    assert((ctx.eax & 255) == 0); // the thick profile really exceeds the radius cap
#ifdef MC3_TEST_REAL_PROFILE_READERS
    // Original stock reads and automatic (-1) selection still use the original generated code.
    for (unsigned profile = 0; profile < 8; ++profile) {
        ctx = Call(sub_00270BC0, 0, {profile});
        assert(std::abs(ctx.st[ctx.top] - stock_ratios[profile]) < 1e-7);
        ctx = Call(sub_00270B40, 0, {20, profile});
        assert(std::abs(ctx.st[ctx.top] - double(.5842f) / stock_ratios[profile]) < 1e-6);
    }
    for (unsigned axle = 0; axle < 2; ++axle) {
        fixture::memory[cfg + 0x1A + axle] = 20; fixture::memory[cfg + 0x1C + axle] = 255;
        ctx = Call(sub_00272FE0, cfg, {axle, bits(.8f)});
        assert(ctx.eax < 8 && std::abs(ctx.st[ctx.top] - double(.5842f) / stock_ratios[ctx.eax]) < 1e-6);
        assert(fixture::memory[cfg + 0x1C + axle] == 255); // getter does not rewrite automatic choice
    }
    W32(0x2725EC + (0x19 - 1) * 4, 0x2723A6); // original type-to-case table entry for tire profile
    for (unsigned axle = 0; axle < 2; ++axle)
        for (unsigned profile = 0; profile < profiles.size(); ++profile) {
            W32(cfg + 8, 0);
            ctx = Call(FixtureGarageSetProfile, cfg, {0x19, axle, profile, 0});
            assert(ctx.eax == 1 && ctx.esp == stack + 20);
            assert(fixture::memory[cfg + 0x1C + axle] == profile && kernel::R32(cfg + 8) == 0x81);
        }
#endif

    W32(garage + 0x18, flash); W32(flash + 8, vm); W32(vtable + 0x24, member);
    W32(vm + 0x40, 0x127000); W32(vm + 0x44, 0x128000);
    for (unsigned i = 0; i < 6; ++i) W32(globals[i], 0x90000 + i * 16);
    Pool(100);
    Inline(garage + 0x3E4, "RimSizeNames"); Inline(garage + 0x416, "RimSizeStatus");
    W32(garage + 0x9CC, 0x18); Inline(0x44C498, "length");
    Inline(0x44B5E4, "2");
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

    Inline(garage + 0x3E4, "TiresProfileNames"); Inline(garage + 0x416, "TiresProfileStatus");
    W32(garage + 0x9CC, 0x19);
    std::vector<Value> stock_profiles;
    for (unsigned i = 0; i < 8; ++i) stock_profiles.push_back({profiles[i], 1});
    AddArray("TiresProfileNames", stock_profiles);
    AddArray("TiresProfileStatus", std::vector<Value>(8, {String("2"), 1}));
    AddArray("TiresProfilePrice", std::vector<Value>(8, {125, 3}));
    Pool(93); // 26 extra members in each array + a 16-symbol reserve = 94 required.
    const unsigned profile_before = member_calls;
    Call(sub_001465E0, garage, {0});
    assert(arrays["TiresProfileNames"].items.size() == 8 && member_calls == profile_before);
    Pool(110);
    Call(sub_001465E0, garage, {0});
    assert(arrays["TiresProfileNames"].items.size() == 34 && arrays["TiresProfileStatus"].items.size() == 34);
    assert(arrays["TiresProfilePrice"].items.size() == 34 && arrays["TiresProfilePrice"].items.back().payload == 125);
    for (const auto& status : arrays["TiresProfileStatus"].items)
        assert(status.type == 1 && kernel::ReadCString(status.payload) == "2");
    assert(kernel::ReadCString(arrays["TiresProfileNames"].items.front().payload) == "Profile_PC_300");
    assert(kernel::ReadCString(arrays["TiresProfileNames"].items.back().payload) == "Profile_PC_975");
    std::set<unsigned> profile_codes;
    double last_ratio = 0;
    for (const auto& item : arrays["TiresProfileNames"].items) {
        const auto found = std::find(profiles.begin(), profiles.end(), item.payload);
        assert(found != profiles.end());
        const unsigned code = unsigned(found - profiles.begin());
        assert(profile_codes.insert(code).second);
        const auto value = Call(sub_00270BC0, 0, {code});
#ifdef MC3_TEST_REAL_PROFILE_READERS
        assert(value.st[value.top] >= last_ratio); // menu sorts by thickness; save codes remain append-only
        last_ratio = value.st[value.top];
#else
        (void)value; (void)last_ratio;
#endif
    }
    assert(profile_codes.size() == 34);
    const unsigned profile_refreshed = member_calls;
    const uint32_t free_after_profiles = kernel::R32(vm + 0x24);
    Call(sub_001465E0, garage, {0});
    assert(member_calls == profile_refreshed && kernel::R32(vm + 0x24) == free_after_profiles);
    for (unsigned i = 0; i < 6; ++i) assert(kernel::R32(globals[i]) == 0x90000 + i * 16);

#ifdef MC3_TEST_REAL_STRING_DESTRUCTOR
    // Opening profile/width/ride menus installs all added labels. Purchasing then leaving for save destroys the
    // same string tables through EA9F0 -> EA950 -> delete, the exact generated path in the native crash stack.
    for (unsigned visit = 0, recycled_table = 0; visit < 3; ++visit) {
        const uint32_t table = TextTable(recycled_table);
        const unsigned freed_before = frees;
        fail_label_allocation = visit == 1;
        Call(sub_001465E0, garage, {0});
        if (visit == 1) {
            // A failed allocation leaves its original placeholder owned and retries on the next refresh.
            assert(Wide(kernel::R32(text_entries.at("rh_900") + 8)) == "!!rh_900!!");
            Call(sub_001465E0, garage, {0});
        }
        assert(Wide(kernel::R32(text_entries.at("rh_900") + 8)) == "9\"\"");
        assert(Wide(kernel::R32(text_entries.at("tw_755_desc") + 8)) == "755 mm Tire Width");
        assert(Wide(kernel::R32(text_entries.at("Profile_PC_975") + 8)) == "Profile ratio 97.5%");
        assert(Wide(kernel::R32(text_entries.at("Profile_PC_300_desc") + 8)) ==
               "Game profile ratio: 30%. Larger values make a thinner sidewall.");
        assert(Wide(kernel::R32(text_entries.at("tw_375") + 8)) == "Localized width");
        assert(frees > freed_before); // replaced placeholder allocations are released immediately
        const size_t allocated = allocations.size();
        const unsigned freed = frees;
        Call(sub_001465E0, garage, {0});
        assert(allocations.size() == allocated && frees == freed); // repeated menu refresh keeps one owner
        for (const auto& [key, entry] : text_entries) {
            const uint32_t text = kernel::R32(entry + 8);
            assert(allocations.contains(text) && (text < 0x30000 || text >= 0x38000));
        }
        ctx = Call(FixtureGarageLeaveForSave, string_table);
        assert(ctx.esp == stack + 4 && kernel::R32(string_table) == 0);
        assert(allocations.size() == 1 && allocations.contains(0x30000)); // only the retained name arena survives
        recycled_table = table; // catch a cache that mistakes a reused address for the previous table lifetime
    }
#endif

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

    // A fully built human driving car missed by a leaf-factory hook is identified by its actual controller,
    // including a backpointer. Sharing a human's cfg, or having only a damage/feedback component, is insufficient.
    constexpr uint32_t driving = 0x146000, driving_sim = 0x166000, driving_cfg = 0x154000, controller = 0x168000;
    auto driving_values = [&](uint32_t car, uint32_t sim, uint32_t config, uint32_t control) {
        W32(car + 4, control); W32(car + 0x50, sim); W32(car + 0x64, config);
        fixture::memory[car + 0x5E] = 0;
        W32(control, 0x47684C); W32(control + 0x50, car);
        W32(config + 8, 0);
    };
    driving_values(driving, driving_sim, driving_cfg, controller);
    const unsigned recovery_before = bind_calls, applied_before = cfg_applied_calls;
    W32(controller + 0x50, ai); // a controller belonging to another concrete car must not authorize this one
    GarageRefreshPlayerCar(driving);
    assert(!players.contains(driving_sim) && bind_calls == recovery_before && kernel::R32(driving_cfg + 8) == 0);
    W32(controller + 0x50, driving); W32(controller, 0x454FB8); // damage-style vtable in the wrong slot
    GarageRefreshPlayerCar(driving);
    assert(!players.contains(driving_sim));
    W32(controller, 0x47684C);
    GarageRefreshPlayerCar(driving);
    assert(players[driving_sim] && bindings.at(driving).cfg == driving_cfg && bind_calls == recovery_before + 1);
    assert(kernel::R32(driving_cfg + 8) == 0x81 && cfg_applied_calls == applied_before);
    W32(driving_cfg + 8, 0);
    GarageRefreshPlayerCar(driving);
    assert(bind_calls == recovery_before + 1 && kernel::R32(driving_cfg + 8) == 0);
    // The baseline notification occurs only after the original CarCfg write, never directly on recognition.
    Call(sub_00121970, driving_sim, {driving_cfg, 0});
    assert(cfg_applied_calls == applied_before + 1 && players[driving_sim]);

    constexpr uint32_t driving_ai = 0x147000, driving_ai_sim = 0x167000, ai_controller = 0x169000;
    driving_values(driving_ai, driving_ai_sim, driving_cfg, ai_controller);
    fixture::memory[driving_ai + 0x5E] = 1; // even a matching controller shape cannot override the AI flag
    W32(0x175044, driving_ai); Call(sub_001FAE10, 0x175000);
    const unsigned before_ai = bind_calls;
    GarageRefreshPlayerCar(driving_ai);
    Call(sub_00121970, driving_ai_sim, {driving_cfg, 0});
    assert(!players[driving_ai_sim] && !bindings.contains(driving_ai) && bind_calls == before_ai + 1);

    // A newly spawned driving car using another concrete address and a recycled simulation must survive the old
    // object's delayed destructor. The old record/association is retired before the new binding is published.
    constexpr uint32_t driving_new = 0x148000, driving_new_cfg = 0x155000, new_controller = 0x16A000;
    driving_values(driving_new, driving_sim, driving_new_cfg, new_controller);
    GarageRefreshPlayerCar(driving_new);
    assert(players[driving_sim] && !bindings.contains(driving) && bindings.at(driving_new).cfg == driving_new_cfg);
    Call(sub_000FFBA0, driving);
    assert(players[driving_sim] && bindings.contains(driving_new));
    Call(sub_000FFBA0, driving_new);
    assert(!players.contains(driving_sim) && !bindings.contains(driving_new));
    // A new concrete human may reuse an address previously recorded as AI; actual identity is required again.
    fixture::memory[driving_ai + 0x5E] = 0;
    GarageRefreshPlayerCar(driving_ai);
    assert(players[driving_ai_sim] && bindings.contains(driving_ai));
    fixture::memory[driving_ai + 0x5E] = 1;
    GarageRefreshPlayerCar(driving_ai);
    assert(!players[driving_ai_sim] && !bindings.contains(driving_ai) && kernel::R32(driving_cfg + 8) == 0x81);
    Call(sub_00121970, driving_ai_sim, {driving_cfg, 0});
    assert(!players[driving_ai_sim]); // shared human config cannot undo a concrete switch back to AI
    Call(sub_000FFBA0, driving_ai);

    // The repaired factory may already have registered both a stale address and its replacement. Alias retirement
    // must precede the helper's unchanged fast path, otherwise delayed teardown still forgets the new driver.
    driving_values(driving, driving_sim, driving_cfg, controller);
    W32(0x176044, driving); Call(sub_002CBDE0, 0x176000);
    driving_values(driving_new, driving_sim, driving_new_cfg, new_controller);
    W32(0x177044, driving_new); Call(sub_002CBDE0, 0x177000);
    GarageRefreshPlayerCar(driving_new);
    assert(players[driving_sim] && bindings.contains(driving_new) && !bindings.contains(driving));
    Call(sub_000FFBA0, driving);
    assert(players[driving_sim] && bindings.contains(driving_new));
    Call(sub_000FFBA0, driving_new);

#ifdef _WIN32
    // The real factory is an on-stack local at 0x2C004C..0x2C0059. Reserve a sparse guest address space so the
    // production high-VA guard is tested with actual 0xD... addresses, without allocating 4 GiB of physical RAM.
    auto* sparse = static_cast<uint8_t*>(VirtualAlloc(nullptr, size_t(1) << 32, MEM_RESERVE, PAGE_NOACCESS));
    assert(sparse && VirtualAlloc(sparse, memory.size(), MEM_COMMIT, PAGE_READWRITE) == sparse);
    std::memcpy(sparse, memory.data(), memory.size());
    constexpr uint32_t high_factory = 0xD1234000;
    assert(VirtualAlloc(sparse + high_factory, 0x1000, MEM_COMMIT, PAGE_READWRITE) == sparse + high_factory);
    kernel::g_base = sparse; sparse_guest = true;
    W32(high_factory + 0x44, driving);
    Call(sub_002CBDE0, high_factory);
    assert(players[driving_sim] && bindings.at(driving).cfg == driving_cfg);
    Call(sub_000FFBA0, driving);
    Call(sub_001FAE10, high_factory);
    assert(!players[driving_sim] && !bindings.contains(driving));
    Call(sub_000FFBA0, driving);
    const unsigned before_invalid_factory = bind_calls;
    Call(sub_002CBDE0, 0xFFFFFFF0); // adding factory+0x44 would wrap into a low address
    Call(sub_002CBDE0, high_factory + 0x1000); // reserved but not committed
    assert(bind_calls == before_invalid_factory);
    sparse_guest = false; kernel::g_base = memory.data();
    assert(VirtualFree(sparse, 0, MEM_RELEASE));
#endif
    return 0;
}
