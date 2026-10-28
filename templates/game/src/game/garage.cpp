// The garage's own wheel and suspension menus, extended, and the player's car for tuning.cpp.
//
// How the game does it (addresses are default.xbe VAs; evidence in docs/notes/garage_tuning.md):
//  - The save's CarCfg holds per axle bytes RideHeight +0x16/+0x17, TireWidth +0x18/+0x19, RimSize +0x1A/+0x1B (inches),
//    TireProfile +0x1C/+0x1D (field registration 0x270F00..).
//  - The car custom database ([0x557F04]) keeps a name list per part type {char** names; u16 count, cap}: rim size
//    (type 0x18) at +0xE8, tire profile (0x19) +0xF0, tire width (0x1A) +0xF8, ride height (0x1F) +0x100. 0x270C10
//    fills them from static tables: ride height rh_0, rh_100..rh_800 (lowered), rh+600..rh+100 (raised), 15 names at
//    0x471A54; tire width tw_195..tw_355, 9 names at 0x471ADC.
//  - Selecting an item looks its name up in that list (0x273EE0) and stores the index with CarCfg::Set 0x272020
//    (ride height and tire width: byte = index; rim: byte = index + 12; profile: byte = index).
//  - CarCfg::GetRideHeight 0x270BE0 turns the byte into metres through 0x471A90 (0, -0.025 .. -0.15, +0.125 .. +0.025);
//    CarCfg::GetTireWidth 0x272FC0 and the model's 0x270C00 through 0x4B3918 (0.195 .. 0.355). vehSim::ApplyCarCfg
//    0x121970 writes them into each wheel (+0x48 SuspensionOffset, +0x08 width, +0x04 radius) when a car spawns/resets.
//  - The lists the garage shows are ActionScript arrays in the car's Flash movie (flash/vs_<car>.xbck), read by name
//    ("RideHeightNames", "RideHeightRearNames", "RimSizeNames", "TiresProfileNames", "TiresWidthNames"; the status
//    array next to each, "...Status", and Flash's own "...Price"). Most cars offer rh_800..rh_0 only, a 4-12 size
//    slice of rim_12..rim_28 and Profile_2..Profile_7. Items are greyed out by Garage::IsOptionAvailable 0x145DC0 (fit
//    check 0xEF590 -> 0x270B60 against vehModel limits; front/rear rims within 2 sizes, ride heights within 5 steps).
//
// What this file adds (Settings::Tuning::garage_extended; on when that field does not exist):
//  - 14 more ride heights (database indices 15..28: rh_900..rh_1600 down to -0.35 m, rh+700..rh+1200 up to +0.275 m)
//    and 29 more tire widths (indices 9..37, spanning 105..755 mm), registered after the game's so
//    the byte a save stores never moves, and the getters for them. These getters stay active with the setting off,
//    so a save made with it on still loads the same car.
//  - The garage lists rewritten to every step: 29 ride heights (16" lower .. 12" higher), all 17 rim sizes 12-28",
//    34 tire profiles, 38 tire widths (105..755 mm); every rim, profile, width and ride height item available.
//  - Labels for the new names (and the game's unlabelled raised ride heights) in the game's string table.
//  - Human players' cars (mcPlayerFactory 0x2CBDE0, vs the other factory 0x1FAE10) told to tuning.cpp, so the F1 ride
//    height offsets apply to them alone, and the garage value is re-based whenever ApplyCarCfg runs.
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "app/app.h"
#include "core/log.h"
#include "kernel/memory.h"
#include "xdk/hle.h"
#include "garage.h"
#include "economy.h"

using kernel::CallGuest;
using kernel::G;
using kernel::R32;
using kernel::W32;

void TuningSetPlayerSim(uint32_t sim, bool player);  // tuning.cpp
void TuningForgetSim(uint32_t sim);
void TuningCarCfgApplied(uint32_t sim);
void TuningBindCar(uint32_t car, uint32_t sim, uint32_t cfg, bool player);

namespace {
// game
constexpr uint32_t kCustomDb = 0x557F04;          // car custom database pointer
constexpr uint32_t kOperatorNew = 0x1B3B90;       // cdecl (size)
constexpr uint32_t kOperatorDelete = 0x1B3C00;    // cdecl (allocation)
constexpr uint32_t kRideNameTable = 0x471A54;     // char*[15]: rh_0, rh_100 .. rh_800, rh+600 .. rh+100
constexpr uint32_t kWidthNameTable = 0x471ADC;    // char*[9]: tw_195 .. tw_355
constexpr uint32_t kRimNameTable = 0x4719C8;      // char*[17]: rim_12 .. rim_28
constexpr uint32_t kProfileNameTable = 0x471938;  // char*[8]: Profile_0 .. Profile_7
constexpr uint32_t kProfileRatioTable = 0x471958; // float[8]: calibrated rim-size value / overall tire diameter
constexpr uint32_t kRimDiameterTable = 0x4B3910;  // float[29], indexed by rim inches (12..28)
constexpr uint32_t kStringTable = 0x507E08;       // the UI string table object (tables at +0, current index +0x28)
constexpr uint32_t kTextLookup = 0x0EA750;        // thiscall table (const char* key) -> entry, wide text at entry+8
constexpr uint32_t kFlashLength = 0x1395F0;       // thiscall flash (array name) -> length or -1
constexpr uint32_t kFlashGet = 0x139650;          // thiscall flash (array name, index, value* out) -> al
constexpr uint32_t kFlashSetString = 0x139540;    // thiscall flash (array name, index, char*), index < length only
constexpr uint32_t kFlashFind = 0x064060;         // thiscall vm (variable name) -> value* {payload, type}
constexpr uint32_t kIndexNames = 0x44C4C0;        // "0".."127", 4 bytes each, the names arrays use for elements
constexpr uint32_t kLengthName = 0x44C498;        // "length"
constexpr uint32_t kStatusAvailable = 0x44B5E4;   // "2", a status the garage writes
constexpr uint32_t kObjectSetMember = 0x24;       // vtable slot: thiscall (const char* name, value*)
constexpr uint32_t kTypeObject = 5, kTypeInt = 3;
// garage object (this of 0x1465E0 / 0x145DC0)
constexpr uint32_t kGarageFlash = 0x18, kGarageNames = 0x3E4, kGarageStatus = 0x416, kGarageType = 0x9CC;
constexpr uint32_t kGarageCars = 0x714, kGarageCurrent = 0x72C; // two preview cars, selected slot 0/1
constexpr uint32_t kTypeRim = 0x18, kTypeProfile = 0x19, kTypeWidth = 0x1A, kTypeRide = 0x1F;
// car component (vtable 0x454F40, constructor 0x0FF160, destructor body 0x0FFBA0, 0x6C bytes)
constexpr uint32_t kCarSim = 0x50, kCarCfg = 0x64, kFactoryCar = 0x44;
constexpr uint32_t kCfgRideHeight = 0x16, kCfgTireWidth = 0x18, kCfgRimSize = 0x1A, kCfgTireProfile = 0x1C;

constexpr unsigned kStockRideSteps = 15, kStockWidths = 9, kStockProfiles = 8;
constexpr unsigned kLowerSteps = 8, kRaiseSteps = 6;  // rh_900..rh_1600, rh+700..rh+1200
// Tire widths this file adds, in database (= save) order from index 9. Append only: a save stores the index.
constexpr unsigned kExtraWidthsMm[] = {375, 395, 415, 435, 455, 175, 155,   // first build
                                       135, 125, 475, 495, 515, 535, 555,   // second build, indices through 22
                                       105, 115, 145, 165, 185,             // narrower/intermediate sizes
                                       575, 595, 615, 635, 655, 675, 695, 715, 735, 755};
constexpr unsigned kExtraWidths = sizeof(kExtraWidthsMm) / sizeof(kExtraWidthsMm[0]);
// Append-only save codes 8..33. Retail 0..7 retain their exact ratios, including the irregular intermediate
// values. These are diameter ratios, not the sidewall/aspect percentage of tire width used on real tire labels.
constexpr unsigned kExtraProfileRatios1000[] = {300, 325, 350, 375, 400, 425, 450, 475, 500, 525, 550, 575,
                                              625, 650, 675, 700, 750, 775, 800, 825, 850, 875, 900, 925,
                                              950, 975};
constexpr unsigned kExtraProfiles = sizeof(kExtraProfileRatios1000) / sizeof(kExtraProfileRatios1000[0]);
static_assert(kStockProfiles + kExtraProfiles < 127); // CarCfg reads int8_t; -1 remains the automatic sentinel.

bool Extended() { return app::Get().tuning.garage_extended; }

bool Committed(uint32_t va, uint32_t bytes) {
    const uint64_t end = uint64_t(va) + bytes;
    if (!bytes || va < 0x10000 || end > (uint64_t(1) << 32)) return false;
    for (uint64_t page = va & ~(uint64_t(mem::kPage) - 1); page < end; page += mem::kPage)
        if (!mem::IsCommitted(uint32_t(page))) return false;
    return true;
}
bool Plausible(uint32_t va) { return va >= 0x10000 && va < 0x84000000 && (va & 3) == 0; }

std::string Str(uint32_t va) { return Committed(va, 1) ? kernel::ReadCString(va, 64) : std::string(); }

uint32_t ThisCall(X86Context& ctx, uint32_t self, uint32_t fn, std::initializer_list<uint32_t> args) {
    const uint32_t ecx = ctx.ecx;
    ctx.ecx = self;
    uint32_t r = CallGuest(ctx, fn, args);
    ctx.ecx = ecx;
    return r;
}

// metres for the database indices this file adds
float ExtendedRideHeight(unsigned index) {  // 15..28
    unsigned k = index - kStockRideSteps;
    if (k < kLowerSteps) return -0.15f - 0.025f * float(k + 1);  // 9" .. 16" lower, past rh_800 (-0.15)
    k -= kLowerSteps;
    if (k < kRaiseSteps) return 0.125f + 0.025f * float(k + 1);  // 7" .. 12" higher, past rh+600 (+0.125)
    return 0;
}
float ExtendedTireWidth(unsigned index) {  // 9..37, 105 .. 755 mm around the game's 195 .. 355
    const unsigned k = index - kStockWidths;
    return k < kExtraWidths ? float(kExtraWidthsMm[k]) / 1000.0f : 0.255f;
}

float ProfileRatio(unsigned index) {
    if (index < kStockProfiles) return G<float>(kProfileRatioTable + index * 4);
    const unsigned k = index - kStockProfiles;
    // Invalid positive/negative save codes must never become an out-of-bounds retail table read.
    return k < kExtraProfiles ? float(kExtraProfileRatios1000[k]) / 1000.0f : G<float>(kProfileRatioTable + 7 * 4);
}

float TireDiameter(unsigned rim, unsigned profile) {
    if (rim < 12 || rim > 28) rim = 20; // only the original verified rim table is supported
    return G<float>(kRimDiameterTable + rim * 4) / ProfileRatio(profile);
}

// The separate, serialized rendering database has only eight profile-to-mesh arrays. Its +0x44 count is
// used by resource relocation, not the garage catalogue. Keep that original count and every owned descriptor
// intact; a separately owned pointer table gains aliases to the nearest existing sidewall mesh. The scoped
// tire draw then fits that mesh's bead to the selected ratio in its own host vertex copy (tire_fit.cpp).
constexpr uint32_t kRenderCustomDb = 0x509E1C, kMeshProfiles = 0x2C, kMeshProfileCount = 0x44;
constexpr unsigned kAllProfiles = kStockProfiles + kExtraProfiles;
struct ProfileAlias { uint32_t root = 0, original = 0, table = 0; };
struct FallbackProfiles {
    uint32_t cfg;
    std::array<uint32_t, 2> full, mesh;
};
std::mutex g_profile_mesh_lock;
ProfileAlias g_profile_alias;
std::unordered_map<uint32_t, FallbackProfiles> g_profile_fallbacks;
bool g_profile_alias_warned = false;

unsigned NearestMeshProfile(unsigned profile) {
    if (profile < kStockProfiles) return profile;
    if (profile >= kAllProfiles) return kStockProfiles - 1;
    const float ratio = ProfileRatio(profile);
    unsigned nearest = 0;
    float distance = std::fabs(ratio - ProfileRatio(0));
    for (unsigned i = 1; i < kStockProfiles; ++i) {
        const float d = std::fabs(ratio - ProfileRatio(i));
        if (d < distance) { distance = d; nearest = i; }
    }
    return nearest;
}

// Caller holds g_profile_mesh_lock. Restore before the game's resource teardown sees the root. This check
// also avoids writing into an old address whose allocation has since been reused for a different object.
uint32_t DetachProfileAliases() {
    if (g_profile_alias.table && Committed(kRenderCustomDb, 4) && R32(kRenderCustomDb) == g_profile_alias.root &&
        Committed(g_profile_alias.root + kMeshProfiles, 4) &&
        R32(g_profile_alias.root + kMeshProfiles) == g_profile_alias.table)
        W32(g_profile_alias.root + kMeshProfiles, g_profile_alias.original);
    const uint32_t owned = g_profile_alias.table;
    g_profile_alias = {};
    g_profile_fallbacks.clear();
    g_profile_alias_warned = false;
    return owned;
}

bool InstallProfileAliases() {
    if (!Committed(kRenderCustomDb, 4)) return false;
    const uint32_t root = R32(kRenderCustomDb);
    if (g_profile_alias.table && root == g_profile_alias.root && Committed(root + kMeshProfiles, 4) &&
        R32(root + kMeshProfiles) == g_profile_alias.table) return true;
    // A resource reload can replace the root without retaining its old address. The alias allocation belongs
    // to the host system allocator, never the serialized image or the game's new[]/delete[] descriptors.
    if (g_profile_alias.table)
        if (const uint32_t old = DetachProfileAliases()) mem::FreeSystem(old);
    if (!Committed(root, kMeshProfileCount + 4) || R32(root + kMeshProfileCount) != kStockProfiles) return false;
    const uint32_t original = R32(root + kMeshProfiles);
    if (!Committed(original, kStockProfiles * 4)) return false;
    for (unsigned i = 0; i < kStockProfiles; ++i)
        if (!Committed(R32(original + 4 * i), 8)) return false;
    const uint32_t table = mem::AllocSystem(kAllProfiles * 4);
    if (!table || !Committed(table, kAllProfiles * 4)) {
        if (table) mem::FreeSystem(table);
        if (!g_profile_alias_warned)
            LOGR("garage: profile mesh alias allocation unavailable; using checked numeric-preserving fallback");
        g_profile_alias_warned = true;
        return false;
    }
    for (unsigned i = 0; i < kAllProfiles; ++i) W32(table + 4 * i, R32(original + 4 * NearestMeshProfile(i)));
    g_profile_alias = {root, original, table};
    W32(root + kMeshProfiles, table); // Publish only the completely initialized pointer table.
    LOGR("garage: %u profile mesh lookups mapped to %u original resource arrays", kAllProfiles, kStockProfiles);
    return true;
}

// Guest memory for retained database/Flash names and source labels. String table entries receive separately owned
// copies: their destructor deletes each text pointer. Taken once while the database is filled, never freed.
struct Arena {
    uint32_t base = 0, used = 0, size = 0;
    uint32_t Alloc(uint32_t n) {
        n = (n + 3) & ~3u;
        if (!base || used + n > size) return 0;
        uint32_t va = base + used;
        used += n;
        return va;
    }
    uint32_t Ascii(const std::string& s) {
        uint32_t va = Alloc(uint32_t(s.size()) + 1);
        if (va) std::memcpy(kernel::HostPtr(va), s.c_str(), s.size() + 1);
        return va;
    }
    uint32_t Wide(const std::string& s) {
        uint32_t va = Alloc(uint32_t(s.size() + 1) * 2);
        if (!va) return 0;
        for (size_t i = 0; i <= s.size(); i++) kernel::W16(va + uint32_t(i) * 2, i < s.size() ? uint8_t(s[i]) : 0);
        return va;
    }
} g_arena;

struct Label {
    uint32_t key, text;  // guest ASCII key, guest UTF-16 text
    uint32_t bytes;      // includes the UTF-16 terminator
};
std::vector<Label> g_labels;

// names this file adds to one of the database's lists, after the game's own
struct Extra {
    const char* what;
    uint32_t list;         // offset of {names, count, cap} in the database
    uint32_t add;          // thiscall db (const char*): appends a copy
    unsigned stock;        // names the game puts there
    std::vector<uint32_t> names;
    bool registered = false;  // the database's indices stock.. are these names
};
Extra g_ride{"ride heights", 0x100, 0x274A80, kStockRideSteps, {}};
Extra g_width{"tire widths", 0xF8, 0x2749C0, kStockWidths, {}};
Extra g_profile{"tire profiles", 0xF0, 0x274980, kStockProfiles, {}};

uint32_t g_value = 0;           // guest scratch: one Flash value {payload, type}
uint32_t g_price_var = 0;       // guest scratch: a 64 byte variable name
uint32_t g_labelled_table = 0;  // the string table the labels were put in

const char* const kNumbers[] = {"One", "Two",    "Three",    "Four",     "Five",    "Six",     "Seven",  "Eight",
                                "Nine", "Ten", "Eleven", "Twelve", "Thirteen", "Fourteen", "Fifteen", "Sixteen"};

uint32_t AddLabel(const std::string& key, const std::string& text, const std::string& desc) {
    Label l{g_arena.Ascii(key), g_arena.Wide(text), uint32_t(text.size() + 1) * 2};
    Label d{g_arena.Ascii(key + "_desc"), g_arena.Wide(desc), uint32_t(desc.size() + 1) * 2};
    if (l.key && l.text) g_labels.push_back(l);
    if (d.key && d.text) g_labels.push_back(d);
    return l.key;
}

bool BuildStrings(X86Context& ctx) {
    if (g_arena.base) return true;
    // Keys, descriptions and retained UTF-16 labels share this arena. The larger width list exceeds 4 KiB.
    constexpr uint32_t kSize = 0x8000;
    uint32_t va = CallGuest(ctx, kOperatorNew, {kSize});
    if (!va || !Committed(va, kSize)) {
        LOGR("garage: no guest memory for the extended steps");
        return false;
    }
    g_arena = {va, 0, kSize};
    g_value = g_arena.Alloc(16);
    g_price_var = g_arena.Alloc(64);
    for (unsigned in = 9; in < 9 + kLowerSteps; in++)
        g_ride.names.push_back(AddLabel("rh_" + std::to_string(in * 100), std::to_string(in) + "\"\"",
                                        std::string("Lower ") + kNumbers[in - 1] + " inches"));
    for (unsigned in = 7; in < 7 + kRaiseSteps; in++)
        g_ride.names.push_back(AddLabel("rh+" + std::to_string(in * 100), "+" + std::to_string(in) + "\"\"",
                                        std::string("Raise ") + kNumbers[in - 1] + " inches"));
    // the game's own raised steps have names but no text
    for (unsigned in = 1; in <= 6; in++)
        AddLabel("rh+" + std::to_string(in * 100), "+" + std::to_string(in) + "\"\"",
                 std::string("Raise ") + kNumbers[in - 1] + (in == 1 ? " inch" : " inches"));
    for (unsigned k = 0; k < kExtraWidths; k++) {
        const std::string mm = std::to_string(kExtraWidthsMm[k]);
        g_width.names.push_back(AddLabel("tw_" + mm, mm + " mm", mm + " mm Tire Width"));
    }
    for (unsigned k = 0; k < kExtraProfiles; k++) {
        const unsigned ratio = kExtraProfileRatios1000[k];
        const std::string percent = std::to_string(ratio / 10) + (ratio % 10 ? ".5" : "");
        g_profile.names.push_back(AddLabel("Profile_PC_" + std::to_string(ratio), "Profile ratio " + percent + "%",
            "Game profile ratio: " + percent + "%. Larger values make a thinner sidewall."));
    }
    for (Extra* e : {&g_ride, &g_width, &g_profile})
        if (std::find(e->names.begin(), e->names.end(), 0u) != e->names.end()) e->names.clear();
    return g_value && g_price_var;
}

// True when the database already has e's names right after the game's.
bool HasNames(uint32_t db, const Extra& e) {
    const uint32_t names = R32(db + e.list);
    const unsigned count = kernel::R16(db + e.list + 4);
    if (e.names.empty() || count < e.stock + e.names.size() || !Committed(names, count * 4)) return false;
    for (size_t i = 0; i < e.names.size(); i++)
        if (Str(R32(names + uint32_t(e.stock + i) * 4)) != Str(e.names[i])) return false;
    return true;
}

void Register(X86Context& ctx, uint32_t db, Extra& e) {
    if (HasNames(db, e)) {
        e.registered = true;
        return;
    }
    const unsigned count = kernel::R16(db + e.list + 4);
    if (e.names.empty() || count != e.stock) {
        e.registered = false;
        LOGR("garage: %u %s in the database, expected %u; extra steps not added", count, e.what, e.stock);
        return;
    }
    for (uint32_t name : e.names) ThisCall(ctx, db, e.add, {name});
    e.registered = HasNames(db, e);
    LOGR("garage: %s %s (%u in the database)", e.what, e.registered ? "extended" : "NOT extended",
         unsigned(kernel::R16(db + e.list + 4)));
}

// ---- the player's car ----
struct Car {
    uint32_t sim, cfg;
    bool player;
    uint32_t driving_controller = 0; // nonzero only after concrete human-controller/backpointer verification
};
std::mutex g_cars_lock;
std::unordered_map<uint32_t, Car> g_cars;                               // car component -> what it owns
std::unordered_set<uint32_t> g_player_cfgs;                             // CarCfgs of human players' cars
std::unordered_map<uint32_t, std::unordered_set<uint32_t>> g_cfg_sims;  // CarCfg -> every vehSim it was applied to

// Caller holds g_cars_lock. A vehSim address can be reused after destruction, or rebound to another cfg while
// previewing. Old cfg ownership must never later demote/forget the new occupant of the same address.
void RemoveSimAssociations(uint32_t sim) {
    for (auto it = g_cfg_sims.begin(); it != g_cfg_sims.end();) {
        it->second.erase(sim);
        if (it->second.empty()) it = g_cfg_sims.erase(it);
        else ++it;
    }
}

void RegisterCar(uint32_t car, bool player) {
    if (!Plausible(car) || !Committed(car, 0x6C)) return;
    const uint32_t sim = R32(car + kCarSim), cfg = R32(car + kCarCfg);
    {
        std::lock_guard<std::mutex> lock(g_cars_lock);
        g_cars[car] = {sim, cfg, player};
        if (player && cfg) g_player_cfgs.insert(cfg);
    }
    if (sim) TuningSetPlayerSim(sim, player);
    TuningBindCar(car, sim, cfg, player);
    LOGR("garage: %s car %08X vehSim %08X CarCfg %08X", player ? "player" : "computer", car, sim, cfg);
}

void RegisterGarageCar(uint32_t garage) {
    if (!Plausible(garage) || !Committed(garage + kGarageCars, kGarageCurrent + 4 - kGarageCars)) return;
    const uint32_t slot = R32(garage + kGarageCurrent);
    if (slot >= 2) return;
    const uint32_t car = R32(garage + kGarageCars + 4 * slot);
    if (!Plausible(car) || !Committed(car, 0x6C)) return;
    {
        std::lock_guard<std::mutex> lock(g_cars_lock);
        // The selected garage preview is an explicit owner even before it has a driving controller. A known
        // computer-owned car must never be promoted merely because it shares a human's cfg.
        if (g_cars.count(car)) return;
    }
    RegisterCar(car, true);
    const uint32_t sim = R32(car + kCarSim);
    if (sim) TuningCarCfgApplied(sim);
}

// A player's suspension as the physics will see it after ApplyCarCfg and tuning.cpp: logged when it changes, so a
// playtest can tell how far the body is lowered against the bump stop (SuspensionLimit) and the wheels' travel.
void LogSuspension(uint32_t sim, uint32_t cfg) {
    if (!Plausible(sim) || !Committed(sim + 0x54, 8)) return;
    auto f32 = [](uint32_t va) {
        float f = 0;
        if (Committed(va, 4)) {
            const uint32_t b = R32(va);
            std::memcpy(&f, &b, 4);
        }
        return f;
    };
    const uint32_t front = R32(sim + 0x54), rear = R32(sim + 0x58);
    if (!Plausible(front) || !Plausible(rear)) return;
    const unsigned rh0 = Committed(cfg + kCfgRideHeight, 4) ? G<uint8_t>(cfg + kCfgRideHeight) : 255;
    const unsigned rh1 = Committed(cfg + kCfgRideHeight, 4) ? G<uint8_t>(cfg + kCfgRideHeight + 1) : 255;
    char line[256];
    std::snprintf(line, sizeof(line),
                  "ride %u/%u: front offset %.3f stop %.3f travel %.3f ext %.3f, rear offset %.3f stop %.3f travel "
                  "%.3f ext %.3f, size y %.3f, model offset y %.3f",
                  rh0, rh1, f32(front + 0x48), f32(front + 0x4C), f32(front + 0x40), f32(front + 0x44),
                  f32(rear + 0x48), f32(rear + 0x4C), f32(rear + 0x40), f32(rear + 0x44), f32(sim + 0x2C),
                  f32(sim + 0x10));
    static std::mutex lock;
    static std::unordered_map<uint32_t, std::string> last;
    std::lock_guard<std::mutex> guard(lock);
    std::string& seen = last[sim];
    if (seen == line) return;
    seen = line;
    LOGR("garage: player vehSim %08X %s", sim, line);
}

void UnregisterCar(uint32_t car) {
    std::vector<uint32_t> sims;
    std::vector<uint32_t> no_longer_player;
    {
        std::lock_guard<std::mutex> lock(g_cars_lock);
        auto it = g_cars.find(car);
        if (it == g_cars.end()) return;
        const Car c = it->second;
        g_cars.erase(it);
        RemoveSimAssociations(c.sim);
        sims.push_back(c.sim);
        bool cfg_in_use = false, cfg_has_player = false;
        for (const auto& [other, oc] : g_cars) {
            cfg_in_use |= oc.cfg == c.cfg;
            cfg_has_player |= oc.cfg == c.cfg && oc.player;
        }
        if (!cfg_has_player) {
            g_player_cfgs.erase(c.cfg);
            auto s = g_cfg_sims.find(c.cfg);
            if (s != g_cfg_sims.end())
                no_longer_player.assign(s->second.begin(), s->second.end());
        }
        if (!cfg_in_use) {
            g_player_cfgs.erase(c.cfg);
            auto s = g_cfg_sims.find(c.cfg);
            if (s != g_cfg_sims.end()) {
                sims.insert(sims.end(), s->second.begin(), s->second.end());
                g_cfg_sims.erase(s);
            }
        }
    }
    for (uint32_t sim : no_longer_player)
        if (sim) TuningSetPlayerSim(sim, false);
    for (uint32_t sim : sims)
        if (sim) TuningForgetSim(sim);
}

// ---- Flash arrays ----
// the object behind an ActionScript variable that holds an array, or 0
uint32_t FlashObject(X86Context& ctx, uint32_t flash, uint32_t var) {
    const uint32_t vm = R32(flash + 8);
    if (!Plausible(vm)) return 0;
    const uint32_t value = ThisCall(ctx, vm, kFlashFind, {var});
    if (!Plausible(value) || !Committed(value, 8) || R32(value + 4) != kTypeObject) return 0;
    const uint32_t obj = R32(value);
    return Plausible(obj) && Committed(obj, 4) && Committed(R32(obj), kObjectSetMember + 4) ? obj : 0;
}

// The movie player keeps its "current VM" in globals while it runs: setting a member that does not exist yet takes a
// node from the symbol pool [0x5014D8]. The game's own element setter 0x641C0 (and the variable lookup 0x64060) fill
// these six from the VM object and zero them after; outside those calls they are 0, and a new member then faults in
// 0x65D70 (the crash of the first build). SetMember does the same as 0x641C0 but puts back whatever was there, in
// case this runs while the player is executing.
constexpr uint32_t kVmGlobals[6] = {0x5014D0, 0x5014D8, 0x5014D4, 0x501410, 0x501420, 0x50142C};
constexpr uint32_t kVmSymbolPool = 0x1C;  // VM + 0x1C: {.., u16 count +6, node* free +8}, nodes of 0x10 linked at +0

struct VmContext {
    uint32_t saved[6];
    explicit VmContext(uint32_t vm) {
        const uint32_t now[6] = {vm + 0x10, vm + 0x1C, vm + 0x28, vm + 0x34, R32(vm + 0x40), R32(vm + 0x44)};
        for (int i = 0; i < 6; i++) {
            saved[i] = R32(kVmGlobals[i]);
            W32(kVmGlobals[i], now[i]);
        }
    }
    ~VmContext() {
        for (int i = 0; i < 6; i++) W32(kVmGlobals[i], saved[i]);
    }
};

// the VM object a Flash wrapper plays, if it looks whole
uint32_t FlashVm(uint32_t flash) {
    const uint32_t vm = R32(flash + 8);
    if (!Plausible(vm) || !Committed(vm, 0x48)) return 0;
    if (!Plausible(R32(vm + 0x40)) || !Plausible(R32(vm + 0x44))) return 0;
    return vm;
}

// free nodes in the VM's symbol pool, counted up to `need`
unsigned PoolFree(uint32_t vm, unsigned need) {
    uint32_t node = R32(vm + kVmSymbolPool + 8);
    unsigned n = 0;
    while (n < need && Plausible(node) && Committed(node, 0x10)) {
        n++;
        node = R32(node);
    }
    return n;
}

void SetMember(X86Context& ctx, uint32_t vm, uint32_t obj, uint32_t name, uint32_t payload, uint32_t type) {
    W32(g_value, payload);
    W32(g_value + 4, type);
    VmContext context(vm);
    ThisCall(ctx, obj, R32(R32(obj) + kObjectSetMember), {name, g_value});
}

int FlashLength(X86Context& ctx, uint32_t flash, uint32_t var) { return int(ThisCall(ctx, flash, kFlashLength, {var})); }

// an element as {payload, type}; false when there is none
bool FlashGet(X86Context& ctx, uint32_t flash, uint32_t var, int index, uint32_t& payload, uint32_t& type) {
    W32(g_value, 0);
    W32(g_value + 4, 7);  // undefined, as the game initialises it
    if (!(ThisCall(ctx, flash, kFlashGet, {var, uint32_t(index), g_value}) & 0xFF)) return false;
    payload = R32(g_value);
    type = R32(g_value + 4);
    return true;
}

// every step, in the order the cars' movies list them (largest / lowest first)
std::vector<uint32_t> WantedList(uint32_t type) {
    std::vector<uint32_t> v;
    if (type == kTypeRim) {
        for (int i = 16; i >= 0; i--) v.push_back(R32(kRimNameTable + 4 * i));  // rim_28 .. rim_12
    } else if (type == kTypeProfile) {
        std::vector<std::pair<float, uint32_t>> profiles;
        for (unsigned i = 0; i < kStockProfiles; i++) profiles.push_back({ProfileRatio(i), R32(kProfileNameTable + 4 * i)});
        if (g_profile.registered)
            for (unsigned k = 0; k < kExtraProfiles; k++)
                profiles.push_back({ProfileRatio(kStockProfiles + k), g_profile.names[k]});
        std::sort(profiles.begin(), profiles.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        for (const auto& p : profiles) v.push_back(p.second); // thickest sidewall first, independently of save order
    } else if (type == kTypeWidth) {
        // widest first, as the movies list them: the game's 195 .. 355 (20 mm apart) and this file's
        std::vector<std::pair<unsigned, uint32_t>> widths;
        for (unsigned i = 0; i < kStockWidths; i++) widths.push_back({195 + 20 * i, R32(kWidthNameTable + 4 * i)});
        if (g_width.registered)
            for (unsigned k = 0; k < kExtraWidths; k++) widths.push_back({kExtraWidthsMm[k], g_width.names[k]});
        std::sort(widths.begin(), widths.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        for (const auto& w : widths) v.push_back(w.second);
    } else if (type == kTypeRide) {
        if (g_ride.registered)
            for (int k = int(kLowerSteps) - 1; k >= 0; k--) v.push_back(g_ride.names[k]);  // rh_1600 .. rh_900
        for (int i = 8; i >= 0; i--) v.push_back(R32(kRideNameTable + 4 * i));            // rh_800 .. rh_0
        for (int i = 14; i >= 9; i--) v.push_back(R32(kRideNameTable + 4 * i));           // rh+100 .. rh+600
        if (g_ride.registered)
            for (unsigned k = 0; k < kRaiseSteps; k++) v.push_back(g_ride.names[kLowerSteps + k]);  // rh+700 ..
    }
    return v;
}

// a status or price array as long as the list: new entries copy the first one (status: "2"). The caller checked the
// array exists with length `len` and the pool has room.
void Lengthen(X86Context& ctx, uint32_t vm, uint32_t flash, uint32_t var, int len, int n, bool status) {
    if (len < 0 || len >= n || n > 128) return;
    const uint32_t obj = FlashObject(ctx, flash, var);
    if (!obj) return;
    uint32_t payload = kStatusAvailable, type = 1;
    if (!status) {
        if (!len || !FlashGet(ctx, flash, var, 0, payload, type)) return;
        if (type != 1 && type != 2 && type != 3 && type != 4) {  // a string the movie owns: not shared, use 0
            payload = 0;
            type = kTypeInt;
        }
    }
    SetMember(ctx, vm, obj, kLengthName, uint32_t(n), kTypeInt);
    for (int i = len; i < n; i++) SetMember(ctx, vm, obj, kIndexNames + 4 * uint32_t(i), payload, type);
}

void PutLabels(X86Context& ctx) {
    const uint32_t which = R32(kStringTable + 0x28);
    const uint32_t table = which < 10 ? R32(kStringTable + 4 * which) : 0;
    if (!table || table == g_labelled_table) return;
    bool complete = true;
    for (const Label& l : g_labels) {
        const uint32_t entry = ThisCall(ctx, kStringTable, kTextLookup, {l.key});
        if (!Plausible(entry) || !Committed(entry, 0x18)) continue;
        const uint32_t text = R32(entry + 8);
        if (text == l.text) continue;
        // A key the table does not have comes back as a placeholder made from the key: the key itself in mode 1, or
        // "!!key!!" in mode 2 ([0x4B1158], set to 2 at 0x205DA8, the mode the game runs in; 0xEA7C0..0xEA867).
        // Only such placeholders are replaced, never a real translation.
        if (!Committed(text, 2)) continue;
        std::string have;
        for (uint32_t i = 0; i < 80 && Committed(text + i * 2, 2); i++) {
            const uint16_t c = kernel::R16(text + i * 2);
            if (!c) break;
            have += c < 0x80 ? char(c) : '?';
        }
        while (!have.empty() && have.front() == '!') have.erase(have.begin());
        while (!have.empty() && have.back() == '!') have.pop_back();
        const std::string key = Str(l.key);
        const bool placeholder =
            have.size() == key.size() && std::equal(have.begin(), have.end(), key.begin(), [](char a, char b) {
                return std::tolower(uint8_t(a)) == std::tolower(uint8_t(b));
            });
        if (placeholder) {
            // 0xEA950 deletes entry+8 on menu teardown (including the purchase/save transition). A pointer into
            // g_arena is not a heap allocation and freeing it interprets UTF-16 label text as allocator metadata.
            // Transfer an ordinary guest allocation to this entry, then release the placeholder it owned.
            const uint32_t owned = CallGuest(ctx, kOperatorNew, {l.bytes});
            if (!owned || !Committed(owned, l.bytes)) {
                complete = false;
                continue;
            }
            std::memcpy(kernel::HostPtr(owned), kernel::HostPtr(l.text), l.bytes);
            W32(entry + 8, owned);
            CallGuest(ctx, kOperatorDelete, {text});
        }
    }
    if (complete) g_labelled_table = table;
}

bool ListFor(uint32_t type, const std::string& var) {
    switch (type) {
    case kTypeRim: return var == "RimSizeNames";
    case kTypeProfile: return var == "TiresProfileNames";
    case kTypeWidth: return var == "TiresWidthNames";
    case kTypeRide: return var == "RideHeightNames" || var == "RideHeightRearNames";
    default: return false;
    }
}

void ExtendList(X86Context& ctx, uint32_t garage) {
    if (!g_arena.base || !Plausible(garage) || !Committed(garage + kGarageFlash, kGarageType + 4 - kGarageFlash))
        return;
    const uint32_t type = R32(garage + kGarageType);
    const uint32_t names = garage + kGarageNames, status = garage + kGarageStatus;
    const std::string var = Str(names);
    if (!ListFor(type, var)) return;
    const uint32_t flash = R32(garage + kGarageFlash);
    if (!Plausible(flash) || !Committed(flash, 12)) return;
    if (type == kTypeRide || type == kTypeWidth || type == kTypeProfile) PutLabels(ctx);

    const std::vector<uint32_t> want = WantedList(type);
    const int n = int(want.size());
    const int len = FlashLength(ctx, flash, names);
    if (len <= 0 || len > 128 || n > 128) return;  // no list (a bike) or not an array
    std::vector<std::string> wanted;
    for (uint32_t w : want) wanted.push_back(Str(w));
    bool same = len == n;
    for (int i = 0; i < len; i++) {
        uint32_t payload, t;
        if (!FlashGet(ctx, flash, names, i, payload, t) || (t != 0 && t != 1)) return;
        const std::string have = Str(payload);
        if (std::find(wanted.begin(), wanted.end(), have) == wanted.end()) return;  // not a list this file knows
        // A longer original list may contain duplicate known names. Only index our expected list when lengths
        // matched; &= evaluates the right side even after same became false.
        same = same && have == wanted[size_t(i)];
    }
    // Everything below is all or nothing: the status array must exist (the garage counts items by it), and the VM's
    // symbol pool must hold every member that will be created, or nothing is touched.
    const int status_len = FlashLength(ctx, flash, status);
    if (status_len < 0 || status_len > 128) return;
    const std::string price = var.substr(0, var.size() - 5) + "Price";  // ...Names -> ...Price
    int price_len = -1;
    if (price.size() < 64) {
        std::memcpy(kernel::HostPtr(g_price_var), price.c_str(), price.size() + 1);
        price_len = FlashLength(ctx, flash, g_price_var);
        if (price_len > 128) price_len = -1;
    }
    auto grow = [n](int l) { return l >= 0 && l < n ? unsigned(n - l) : 0u; };
    const unsigned need = (same ? 0u : grow(len)) + grow(status_len) + grow(price_len);
    if (same && !need) return;
    const uint32_t vm = FlashVm(flash);
    if (!vm) return;
    if (need && PoolFree(vm, need + 16) < need + 16) {
        static int reported = 0;
        if (reported++ < 4) LOGR("garage: %s not extended, the movie's symbol pool is nearly full", var.c_str());
        return;
    }
    if (!same) {
        const uint32_t obj = FlashObject(ctx, flash, names);
        if (!obj) return;
        SetMember(ctx, vm, obj, kLengthName, uint32_t(n), kTypeInt);
        // the game's setter makes its own VM context for each element
        for (int i = 0; i < n; i++) ThisCall(ctx, flash, kFlashSetString, {names, uint32_t(i), want[size_t(i)]});
        LOGR("garage: %s now %d steps (was %d)", var.c_str(), n, len);
    }
    Lengthen(ctx, vm, flash, status, status_len, n, true);
    Lengthen(ctx, vm, flash, g_price_var, price_len, n, false);
}
}  // namespace

bool GarageTireFitForDraw(uint32_t render, uint32_t resources, uint32_t handle, unsigned axle,
                         float& source_ratio, float& target_ratio) {
    if (axle > 1 || !Committed(render, 0x1804) || !Committed(resources, 0xE5) || !handle ||
        G<uint8_t>(resources + 0xE4) != 0 || R32(resources + 0xC4 + 4 * axle) != handle) return false;
    const uint32_t model = R32(render + 0x1800);
    if (!Committed(model, 0x1C) || R32(model + 0x18) != resources) return false;
    const uint32_t car = R32(model + 4);
    uint32_t cfg = 0;
    {
        std::lock_guard<std::mutex> lock(g_cars_lock);
        const auto found = g_cars.find(car);
        if (found == g_cars.end() || !found->second.player || !Committed(car, 0x6C) ||
            R32(car + kCarSim) != found->second.sim || R32(car + kCarCfg) != found->second.cfg ||
            R32(car + 0x68) != render) return false;
        cfg = found->second.cfg;
    }
    if (!Committed(cfg, 0x20)) return false;
    const unsigned profile = G<uint8_t>(cfg + kCfgTireProfile + axle);
    if (profile < kStockProfiles || profile >= kAllProfiles) return false;
    const unsigned nearest = NearestMeshProfile(profile), brand = R32(render + 0x11B0);
    const unsigned rim = G<uint8_t>(cfg + kCfgRimSize + axle);
    const uint32_t mesh_code = R32(render + 0x11B4 + 4 * axle);
    if (brand >= 11 || (mesh_code != profile && mesh_code != nearest) || rim < 12 || rim > 28 ||
        R32(render + 0x11BC + 4 * axle) != rim) return false;
    const float expected = TireDiameter(rim, profile);
    const float diameter = G<float>(render + (axle ? 0x12CC : 0x12D4));
    if (!std::isfinite(expected) || !std::isfinite(diameter) ||
        std::fabs(expected - diameter) > 0.00001f * std::max(1.0f, expected)) return false;
    // Verify the exact nearest-stock resource, independently of the extended alias table/fallback code.
    std::lock_guard<std::mutex> lock(g_profile_mesh_lock);
    const uint32_t root = Committed(kRenderCustomDb, 4) ? R32(kRenderCustomDb) : 0;
    if (!Committed(root, kMeshProfileCount + 4) || R32(root + kMeshProfileCount) != kStockProfiles) return false;
    const uint32_t table = R32(root + kMeshProfiles);
    if (!Committed(table, kStockProfiles * 4)) return false;
    const uint32_t descriptor = R32(table + 4 * nearest);
    if (!Committed(descriptor, 8) || R32(descriptor + 4) != 11) return false;
    const uint32_t items = R32(descriptor);
    if (!Committed(items, 11 * 4) || R32(items + 4 * brand) != handle) return false;
    source_ratio = ProfileRatio(nearest);
    target_ratio = ProfileRatio(profile);
    return std::isfinite(source_ratio) && std::isfinite(target_ratio) && source_ratio > 0 &&
           source_ratio < 1 && target_ratio > 0 && target_ratio < 1;
}

// The old factory guard incorrectly rejected valid stack factories. In addition to fixing that guard below,
// recover concrete driving ownership defensively across later rebind/reset paths, never from a shared CarCfg.
// 0x2CB320 installs car+4; ctor0x2C9970 writes vtable0x47684C; init0x2C89F0 writes controller+0x50=car.
// The human/AI factories write car+0x5E=0/1 respectively. Garage previews remain explicitly registered above.
void GarageRefreshPlayerCar(uint32_t car) {
    if (!Plausible(car) || !Committed(car, 0x6C)) return;
    const uint32_t sim = R32(car + kCarSim), cfg = R32(car + kCarCfg), controller = R32(car + 4);
    const bool ai = G<uint8_t>(car + 0x5E) != 0;
    const bool human = !ai && Plausible(sim) && Committed(sim, 0x5C) && Plausible(cfg) && Committed(cfg, 0x20) &&
        Plausible(controller) && Committed(controller, 0x54) && R32(controller) == 0x47684C &&
        R32(controller + 0x50) == car;
    if (human) app::EndGarageNavigation(); // actual street driver; never carry garage repeat gating into steering
    std::vector<uint32_t> retire;
    {
        std::lock_guard<std::mutex> lock(g_cars_lock);
        auto it = g_cars.find(car);
        if (!human) {
            // Do not demote a preview or an incompletely built factory car merely because it has no controller.
            // A previously verified driver becoming AI or losing its concrete identity must release stale tuning.
            if (it != g_cars.end() && it->second.player && (ai || it->second.driving_controller)) retire.push_back(car);
        } else {
            // vehSim is per-car, not shared. A stale concrete owner at another car address must not later destroy
            // the new binding when its delayed destructor runs after that simulation address was recycled.
            for (const auto& [other, owner] : g_cars)
                if (other != car && owner.sim == sim) retire.push_back(other);
            if (retire.empty() && it != g_cars.end() && it->second.player && it->second.sim == sim && it->second.cfg == cfg) {
                it->second.driving_controller = controller;
                return; // never rebase offsets or rebuild tuning every frame
            }
            if (it != g_cars.end()) retire.push_back(car);
        }
    }
    for (uint32_t old : retire) UnregisterCar(old);
    if (!human) {
        if (ai && !retire.empty() && Plausible(sim) && Committed(sim, 0x5C) && Plausible(cfg) && Committed(cfg, 0x20)) {
            RegisterCar(car, false); // keep concrete AI ownership stronger than a shared human cfg
            W32(cfg + 8, R32(cfg + 8) | 0x81u); // refresh the newly AI-owned instance from original purchased values
        }
        return;
    }
    {
        std::lock_guard<std::mutex> lock(g_cars_lock);
        RemoveSimAssociations(sim); // an old preview cfg must not later demote this concrete driving owner
    }
    RegisterCar(car, true);
    {
        std::lock_guard<std::mutex> lock(g_cars_lock);
        g_cars[car].driving_controller = controller;
        g_cfg_sims[cfg].insert(sim);
    }
    // Called at 0xFFD90 entry: request its authoritative same-frame CarCfg refresh. 0x121970 then writes the
    // purchased axle offsets before TuningCarCfgApplied rebases them. Rebasing already tuned values here would
    // double-apply the live offsets, particularly when leaving the garage.
    W32(cfg + 8, R32(cfg + 8) | 0x81u);
}

// String-table contents are destroyed when the menu changes. The heap may reuse the table address on the next
// visit; invalidate the cache at its lifetime boundary so the rebuilt table receives its own label allocations.
X86_EXTERN_FUNC(__imp__sub_000EA950);
X86_FUNC(sub_000EA950) {
    if (ctx.ecx == g_labelled_table) g_labelled_table = 0;
    __imp__sub_000EA950(ctx, base);
}

// The database's part names, filled at start (and again when the game rebuilds it): the extra names go right after
// the game's so their indices, which saves store, never move.
X86_EXTERN_FUNC(__imp__sub_00270C10);
X86_FUNC(sub_00270C10) {
    __imp__sub_00270C10(ctx, base);
    if (!BuildStrings(ctx)) return;
    const uint32_t db = R32(kCustomDb);
    if (!Plausible(db) || !Committed(db, 0x188)) return;
    Register(ctx, db, g_ride);
    Register(ctx, db, g_width);
    Register(ctx, db, g_profile);
}

// Serialized tire resources are loaded independently of the garage part-name database.
X86_EXTERN_FUNC(__imp__sub_000F5E70);
X86_FUNC(sub_000F5E70) {
    {
        std::lock_guard<std::mutex> lock(g_profile_mesh_lock);
        if (const uint32_t old = DetachProfileAliases()) mem::FreeSystem(old);
    }
    __imp__sub_000F5E70(ctx, base);
    std::lock_guard<std::mutex> lock(g_profile_mesh_lock);
    InstallProfileAliases();
}

X86_EXTERN_FUNC(__imp__sub_000F5270);
X86_FUNC(sub_000F5270) {
    uint32_t owned;
    {
        std::lock_guard<std::mutex> lock(g_profile_mesh_lock);
        owned = DetachProfileAliases();
    }
    struct ReleaseOwnedTable {
        uint32_t table;
        ~ReleaseOwnedTable() { if (table) mem::FreeSystem(table); }
    } release{owned};
    __imp__sub_000F5270(ctx, base); // Sees its original table/count and owns only its original allocations.
}

void GarageNormalizeProfileMeshes(uint32_t render, uint32_t cfg) {
    if (!Committed(render, 0x12DC)) return;
    std::lock_guard<std::mutex> lock(g_profile_mesh_lock);
    const bool aliased = InstallProfileAliases();
    FallbackProfiles fallback{cfg, {}, {}};
    bool needs_fallback = false;
    for (unsigned axle = 0; axle < 2; ++axle) {
        uint32_t profile = R32(render + 0x11B4 + 4 * axle);
        // Recover a full saved code when an earlier allocation failure temporarily selected a retail mesh.
        // Automatic 0xFF still uses the original resolved profile already written by F32A0.
        if (Committed(cfg, kCfgTireProfile + 2)) {
            const unsigned saved = G<uint8_t>(cfg + kCfgTireProfile + axle);
            if (saved >= kStockProfiles && saved < kAllProfiles) profile = saved;
        }
        if (profile >= kAllProfiles) profile = kStockProfiles - 1;
        const uint32_t mesh = aliased ? profile : NearestMeshProfile(profile);
        W32(render + 0x11B4 + 4 * axle, mesh);
        fallback.full[axle] = profile;
        fallback.mesh[axle] = mesh;
        needs_fallback |= profile != mesh;
    }
    if (needs_fallback) g_profile_fallbacks[render] = fallback;
    else g_profile_fallbacks.erase(render);
}

// F11C0 is a numeric ratio getter used by the draw matrices, not a mesh getter. On allocation failure the
// mesh slot is normalized, but its exact numeric profile survives here. Matching cfg, mesh and diameter
// reject stale bindings after destruction/address reuse; a fresh F32A0 also replaces or erases the binding.
X86_EXTERN_FUNC(__imp__sub_000F11C0);
X86_FUNC(sub_000F11C0) {
    const uint32_t render = ctx.ecx, axle = R32(ctx.esp + 4);
    if (axle < 2) {
        std::lock_guard<std::mutex> lock(g_profile_mesh_lock);
        const auto it = g_profile_fallbacks.find(render);
        if (it != g_profile_fallbacks.end()) {
            const auto& f = it->second;
            const unsigned full = f.full[axle];
            if (full >= kStockProfiles && full < kAllProfiles &&
                Committed(f.cfg, kCfgTireProfile + 2) &&
                G<uint8_t>(f.cfg + kCfgTireProfile + axle) == full &&
                Committed(render + 0x11B4, 0x128) && R32(render + 0x11B4 + 4 * axle) == f.mesh[axle]) {
                const unsigned rim = R32(render + 0x11BC + 4 * axle);
                const float diameter = G<float>(render + (axle ? 0x12CC : 0x12D4));
                const float expected = TireDiameter(rim, full);
                if (rim >= 12 && rim <= 28 && std::isfinite(diameter) &&
                    std::fabs(diameter - expected) <= std::max(0.00001f, expected * 0.00001f)) {
                    ctx.eax = full; ctx.ecx = full;
                    x87_push(ctx, double(ProfileRatio(full)));
                    ctx.esp += 8;
                    return;
                }
            }
        }
    }
    __imp__sub_000F11C0(ctx, base);
}

// CarCfg::GetRideHeight(axle), thiscall, ret 4, metres on the x87 stack: the added steps
X86_EXTERN_FUNC(__imp__sub_00270BE0);
X86_FUNC(sub_00270BE0) {
    const uint32_t cfg = ctx.ecx, axle = R32(ctx.esp + 4);
    const unsigned index = axle < 2 && Committed(cfg + kCfgRideHeight, 2) ? G<uint8_t>(cfg + kCfgRideHeight + axle) : 0;
    if (index < kStockRideSteps) {
        __imp__sub_00270BE0(ctx, base);
        return;
    }
    ctx.eax = axle;
    ctx.ecx = index;
    x87_push(ctx, double(ExtendedRideHeight(index)));
    ctx.esp += 8;
}

// CarCfg::GetTireWidth(axle), thiscall, ret 4, metres on the x87 stack (the physics wheel +0x08)
X86_EXTERN_FUNC(__imp__sub_00272FC0);
X86_FUNC(sub_00272FC0) {
    const uint32_t cfg = ctx.ecx, axle = R32(ctx.esp + 4);
    const unsigned index = axle < 2 && Committed(cfg + kCfgTireWidth, 2) ? G<uint8_t>(cfg + kCfgTireWidth + axle) : 0;
    if (index < kStockWidths) {
        __imp__sub_00272FC0(ctx, base);
        return;
    }
    ctx.eax = axle;
    ctx.ecx = index;
    x87_push(ctx, double(ExtendedTireWidth(index)));
    ctx.esp += 8;
}

// tire width of a width index, cdecl, ret, metres on the x87 stack (the car model's tires, 0x0F32A0)
X86_EXTERN_FUNC(__imp__sub_00270C00);
X86_FUNC(sub_00270C00) {
    const uint32_t index = R32(ctx.esp + 4);
    if (index < kStockWidths) {
        __imp__sub_00270C00(ctx, base);
        return;
    }
    ctx.eax = index;
    x87_push(ctx, double(ExtendedTireWidth(index)));
    ctx.esp += 4;
}

// All direct readers of the retail eight-entry ratio table need an extended path. Both the visual tire builder
// and vehSim::ApplyCarCfg consume diameter, then halve it for radius; changing a menu label alone does nothing.
// Keep these getters active when the menu option is disabled so saved append-only profile codes remain valid.
X86_EXTERN_FUNC(__imp__sub_00270BC0);
X86_FUNC(sub_00270BC0) {
    const unsigned profile = R32(ctx.esp + 4);
    if (profile < kStockProfiles) { __imp__sub_00270BC0(ctx, base); return; }
    ctx.eax = profile;
    x87_push(ctx, double(ProfileRatio(profile)));
    ctx.esp += 4;
}

X86_EXTERN_FUNC(__imp__sub_00270B40);
X86_FUNC(sub_00270B40) {
    const unsigned rim = R32(ctx.esp + 4), profile = R32(ctx.esp + 8);
    if (profile < kStockProfiles) { __imp__sub_00270B40(ctx, base); return; }
    ctx.eax = rim; ctx.ecx = profile;
    x87_push(ctx, double(TireDiameter(rim, profile)));
    ctx.esp += 4;
}

X86_EXTERN_FUNC(__imp__sub_00272FE0);
X86_FUNC(sub_00272FE0) {
    const uint32_t cfg = ctx.ecx, axle = R32(ctx.esp + 4);
    const unsigned profile = axle < 2 && Committed(cfg + kCfgRimSize, 4) ? G<uint8_t>(cfg + kCfgTireProfile + axle) : 0;
    if (profile < kStockProfiles || profile == 255) { __imp__sub_00272FE0(ctx, base); return; }
    const unsigned rim = G<uint8_t>(cfg + kCfgRimSize + axle);
    ctx.eax = profile; ctx.edx = axle; ctx.ecx = rim;
    x87_push(ctx, double(TireDiameter(rim, profile)));
    ctx.esp += 12;
}

// Garage fit query (profile, rim, ride index, min radius+ride, max radius-ride, max tire radius), cdecl.
// This path is also used outside IsOptionAvailable. Passing new codes into its original table loads is unsafe.
X86_EXTERN_FUNC(__imp__sub_00270B60);
X86_FUNC(sub_00270B60) {
    const uint32_t stack = ctx.esp;
    const unsigned profile = R32(stack + 4), rim = R32(stack + 8), ride = R32(stack + 12);
    if (profile < kStockProfiles && ride < kStockRideSteps) { __imp__sub_00270B60(ctx, base); return; }
    const float radius = TireDiameter(rim, profile) * 0.5f;
    const float height = ride < kStockRideSteps ? G<float>(0x471A90 + ride * 4) : ExtendedRideHeight(ride);
    const bool fits = radius + height >= G<float>(stack + 16) && radius - height <= G<float>(stack + 20) &&
                      radius <= G<float>(stack + 24);
    ctx.eax = (ctx.eax & ~0xFFu) | unsigned(fits);
    ctx.esp += 4;
}

// Garage::IsOptionAvailable(item, axle), thiscall, ret 8, al: every rim size, tire profile, width and ride height fits
X86_EXTERN_FUNC(__imp__sub_00145DC0);
X86_FUNC(sub_00145DC0) {
    const uint32_t garage = ctx.ecx;
    if (Extended() && Plausible(garage) && Committed(garage + kGarageType, 4)) {
        const uint32_t type = R32(garage + kGarageType);
        if (type == kTypeRim || type == kTypeProfile || type == kTypeWidth || type == kTypeRide) {
            ctx.eax = (ctx.eax & ~0xFFu) | 1;
            ctx.esp += 12;
            return;
        }
    }
    __imp__sub_00145DC0(ctx, base);
}

// Garage: sets each item's status in the current list (thiscall, ret 4). The list is made whole first.
X86_EXTERN_FUNC(__imp__sub_001465E0);
X86_FUNC(sub_001465E0) {
    RegisterGarageCar(ctx.ecx);
    if (Extended()) ExtendList(ctx, ctx.ecx);
    __imp__sub_001465E0(ctx, base);
}

// The garage's regular update, shared by every vehicle movie. A short-lived native button makes the extra PC
// tuning discoverable without consuming any of Flash's scarce symbols or changing its saved part categories.
X86_EXTERN_FUNC(__imp__sub_001519D0);
X86_FUNC(sub_001519D0) {
    const uint32_t garage = ctx.ecx;
    app::GarageTuningActive();
    __imp__sub_001519D0(ctx, base);
    mc3::economy::RefreshGarageWallet(ctx, base, garage);
}

// vehSim::ApplyCarCfg(CarCfg*, model), thiscall, ret 8: the garage ride height has just replaced SuspensionOffset
X86_EXTERN_FUNC(__imp__sub_00121970);
X86_FUNC(sub_00121970) {
    const uint32_t sim = ctx.ecx;
    const uint32_t cfg = Committed(ctx.esp + 4, 4) ? R32(ctx.esp + 4) : 0;
    __imp__sub_00121970(ctx, base);
    bool player = false;
    uint32_t known_car = 0;
    {
        std::lock_guard<std::mutex> lock(g_cars_lock);
        RemoveSimAssociations(sim);
        if (cfg) {
            g_cfg_sims[cfg].insert(sim);
            player = g_player_cfgs.count(cfg) != 0;
            // A shared CarCfg also appears on garage previews, but it must never turn a known computer-driven
            // car into a player car. Its concrete owner takes precedence over the preview fallback.
            for (auto& [car, owner] : g_cars)
                if (owner.sim == sim) {
                    player = owner.player;
                    const uint32_t old_cfg = owner.cfg;
                    owner.cfg = cfg;
                    known_car = car;
                    if (player) g_player_cfgs.insert(cfg);
                    if (old_cfg != cfg && std::none_of(g_cars.begin(), g_cars.end(), [old_cfg](const auto& entry) {
                            return entry.second.player && entry.second.cfg == old_cfg;
                        })) g_player_cfgs.erase(old_cfg);
                    break;
                }
        }
    }
    TuningSetPlayerSim(sim, player);
    if (known_car) TuningBindCar(known_car, sim, cfg, player);
    TuningCarCfgApplied(sim);
    if (player) LogSuspension(sim, cfg);
}

// mcPlayerFactory: makes a human player's car component; it is left at factory+0x44
X86_EXTERN_FUNC(__imp__sub_002CBDE0);
X86_FUNC(sub_002CBDE0) {
    const uint32_t factory = ctx.ecx;
    __imp__sub_002CBDE0(ctx, base);
    // Factories are also stack locals (0x2C004C..0x2C0059), and guest stacks live at 0xD0000000+. Committed
    // checks the full prefix with a 64-bit end calculation, so this accepts a valid stack without VA wraparound.
    if (Committed(factory, kFactoryCar + 4)) RegisterCar(R32(factory + kFactoryCar), true);
}

// the other car factory (vtable 0x46C730): computer driven cars
X86_EXTERN_FUNC(__imp__sub_001FAE10);
X86_FUNC(sub_001FAE10) {
    const uint32_t factory = ctx.ecx;
    __imp__sub_001FAE10(ctx, base);
    if (Committed(factory, kFactoryCar + 4)) RegisterCar(R32(factory + kFactoryCar), false);
}

// the car component's destructor body
X86_EXTERN_FUNC(__imp__sub_000FFBA0);
X86_FUNC(sub_000FFBA0) {
    UnregisterCar(ctx.ecx);
    __imp__sub_000FFBA0(ctx, base);
}
