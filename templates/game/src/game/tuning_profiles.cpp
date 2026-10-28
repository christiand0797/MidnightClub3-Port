// The native profile owns 30 CarCfg slots at profile+0x508, stride0x108.
// 0x26BD10 loads/saves the exact profile image; 0x26BB80 removes a slot by
// 0x271E20 copies and compacts the roster. There is no proven saved unique ID.
// PC-only IDs follow the actual slot/copy lineage, never a model-only match.
#include "tuning_profiles.h"
#include "app/tuning_profiles.h"
#include "kernel/memory.h"
#include "xdk/hle.h"

#include <algorithm>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

X86_EXTERN_FUNC(__imp__sub_00271D20);
X86_EXTERN_FUNC(__imp__sub_00271E20);
X86_EXTERN_FUNC(__imp__sub_0026BB80);
X86_EXTERN_FUNC(__imp__sub_0026BD10);

namespace {
using kernel::R32;
namespace profiles = app::tuning_profiles;
constexpr uint32_t kProfile = 0x53F308, kCars = kProfile + 0x508, kStride = 0x108;
constexpr uint32_t kCarVtable = 0x4483DC, kState = 0x53F2D4, kCareer = 0x557F0C;
constexpr uint32_t kCareerVtable = 0x473B98, kDataVtable = 0x474288;
struct Roster { std::string career, label; std::vector<profiles::Car> cars; uint32_t selected = 0; };
struct Binding { std::string key, model; uint64_t generation = 0; };
// Lock order is always UiLock then this lock. No guest writes or guest calls under either lock.
std::mutex g_lock;
std::unordered_map<uint32_t, Binding> g_bindings;
std::unordered_map<uint32_t, uint64_t> g_generations;
std::string g_career;

bool Committed(uint32_t va, uint32_t size) {
    const uint64_t end = uint64_t(va) + size;
    if (!kernel::g_base || !size || va < 0x10000 || end > 0x84000000ull) return false;
    for (uint64_t p = va & ~(uint64_t(mem::kPage)-1); p < end; p += mem::kPage)
        if (!mem::IsCommitted(uint32_t(p))) return false;
    return true;
}
bool Text(uint32_t va, uint32_t capacity, std::string& out) {
    out.clear();
    if (!Committed(va, capacity)) return false;
    for (uint32_t i=0; i<capacity; ++i) {
        const auto c = kernel::G<uint8_t>(va+i);
        if (!c) return !out.empty();
        if (c<32 || c>=127) return false;
        out.push_back(char(c));
    }
    return false;
}
bool ReadCar(uint32_t cfg, profiles::Car& out) {
    if ((cfg&3) || !Committed(cfg,kStride) || R32(cfg)!=kCarVtable || !Text(cfg+0xE3,22,out.model)) return false;
    // Copy/serialized fields exclude the vtable, unknown runtime dword+4, dirty+8,
    // and tail padding. Parts are mutable; the signature authenticates reloads.
    static constexpr char hex[]="0123456789abcdef";
    out.signature.clear(); out.signature.reserve(500);
    for (uint32_t i=0x0C;i<0x106;++i) {
        const auto b=kernel::G<uint8_t>(cfg+i);
        out.signature.push_back(hex[b>>4]);out.signature.push_back(hex[b&15]);
    }
    return true;
}
bool ReadRoster(Roster& out, bool allow_empty=false) {
    if (!Committed(kState,4) || (R32(kState)!=2 && R32(kState)!=5) ||
        !Committed(kCareer,4) || !Committed(kProfile+0x504,2)) return false;
    const auto career=R32(kCareer);
    if ((career&3) || !Committed(career,0x10) || R32(career)!=kCareerVtable) return false;
    const auto data=R32(career+0x0C);
    if ((data&3) || !Committed(data,0x54) || R32(data)!=kDataVtable || R32(data+0x50)>=13) return false;
    const auto count=kernel::G<uint8_t>(kProfile+0x504);
    out.selected=kernel::G<uint8_t>(kProfile+0x505);
    if (count>30 || (!count && !allow_empty) || (count ? out.selected>=count : out.selected!=0) ||
        !Text(kProfile+0x2E8,16,out.label)) return false;
    out.career="hdd"+std::to_string(app::Instance()>1?app::Instance():0)+"/"+out.label;
    out.cars.clear();out.cars.reserve(count);
    for (uint32_t i=0;i<count;++i) {
        profiles::Car car;
        if (!ReadCar(kCars+i*kStride,car)) return false;
        out.cars.push_back(std::move(car));
    }
    return true;
}
void Clear() {
    std::lock_guard ui(app::UiLock());
    std::lock_guard lock(g_lock);
    g_bindings.clear();g_generations.clear();g_career.clear();profiles::ClearSelection();
}
void Observe(const Roster& roster) {
    std::lock_guard ui(app::UiLock());
    std::lock_guard lock(g_lock);
    if (g_career!=roster.career) { g_bindings.clear();g_generations.clear();g_career=roster.career; }
    profiles::Observe(roster.career,roster.label,roster.cars,roster.selected);
    std::erase_if(g_bindings,[&](const auto& entry) {
        return entry.first>=kCars+roster.cars.size()*kStride && entry.first<kCars+30*kStride;
    });
    for (uint32_t i=0;i<roster.cars.size();++i) {
        const auto cfg=kCars+i*kStride;
        g_bindings[cfg]={profiles::KeyForSlot(i),roster.cars[i].model,g_generations[cfg]};
    }
}
Binding BindingFor(uint32_t cfg) {
    auto it=g_bindings.find(cfg);profiles::Car current;
    if (it==g_bindings.end() || !ReadCar(cfg,current) || current.model!=it->second.model ||
        g_generations[cfg]!=it->second.generation) return {};
    return it->second;
}
}  // namespace

namespace mc3::car_tuning {
void ObserveFrame() { Roster roster;if(ReadRoster(roster)) Observe(roster);else Clear(); }
Bank CaptureBank() {
    std::lock_guard ui(app::UiLock());
    std::lock_guard lock(g_lock);
    Bank out;out.global=app::Get().tuning;
    for (auto it=g_bindings.begin();it!=g_bindings.end();) {
        const auto binding=BindingFor(it->first);
        if (binding.key.empty()) {it=g_bindings.erase(it);continue;}
        auto value=profiles::EffectiveForKey(binding.key);
        // Garage database/model limits apply to every model; only the per-instance
        // physical/render controls can be overridden by an owned-car profile.
        value.garage_extended=out.global.garage_extended;value.wheel_limit=out.global.wheel_limit;
        out.configs.emplace(it->first,value);++it;
    }
    return out;
}
app::Settings::Tuning EffectiveForCfg(uint32_t cfg) { return CaptureBank().ForCfg(cfg); }
}  // namespace mc3::car_tuning

X86_FUNC(sub_00271D20) {
    const auto cfg=ctx.ecx;
    {std::lock_guard ui(app::UiLock());std::lock_guard lock(g_lock);g_bindings.erase(cfg);++g_generations[cfg];}
    __imp__sub_00271D20(ctx,base);
}
X86_FUNC(sub_00271E20) {
    const auto destination=ctx.ecx;
    const auto source=Committed(ctx.esp+4,4)?R32(ctx.esp+4):0;
    Binding binding;
    {std::lock_guard ui(app::UiLock());std::lock_guard lock(g_lock);binding=BindingFor(source);}
    __imp__sub_00271E20(ctx,base);
    profiles::Car current;
    std::lock_guard ui(app::UiLock());std::lock_guard lock(g_lock);
    g_bindings.erase(destination);
    if (!binding.key.empty() && ReadCar(destination,current) && current.model==binding.model)
        g_bindings[destination]={binding.key,binding.model,g_generations[destination]};
}
X86_FUNC(sub_0026BB80) {
    Roster before,after;
    const auto slot=Committed(ctx.esp+4,4)?R32(ctx.esp+4):UINT32_MAX;
    const bool observed=ctx.ecx==kProfile && ReadRoster(before) && slot<before.cars.size();
    if (observed) Observe(before);
    __imp__sub_0026BB80(ctx,base);
    if (observed && ReadRoster(after,true) && before.career==after.career) {
        profiles::Removed(before.career,slot,before.cars,after.cars,after.selected);
        Observe(after);
    } else if (observed) Clear();
}
X86_FUNC(sub_0026BD10) {
    const bool load=ctx.ecx==kProfile;
    const uint32_t source=Committed(ctx.esp+4,4)?R32(ctx.esp+4):0;
    const uint32_t caller=Committed(ctx.esp,4)?R32(ctx.esp):0;
    const bool serialize=!load && source==kProfile && (caller==0x1A5A4 || caller==0x1A6CE);
    if(load) Clear();
    __imp__sub_0026BD10(ctx,base);
    if(load) mc3::car_tuning::ObserveFrame();
    if(serialize) {
        // These two original save paths just copied the profile to their save
        // buffer. Persist only PC metadata alongside that normal flow, never
        // request a game save, modify its bytes or change the original result.
        mc3::car_tuning::ObserveFrame();
        profiles::Save();
    }
}
