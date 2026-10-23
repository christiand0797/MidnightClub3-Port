// Exercise the actual F6A20 resource-table loads and F11C0 numeric ratio getter with production alias hooks.
// Reuse the existing bounded guest/allocator doubles; the original full garage test remains a separate target.
#define main UnusedGarageFixtureMain
#include "garage_test.cpp"
#undef main
#include "../templates/game/src/game/garage.h"
#include "cpu/x86_ops.h"
#include <array>
#include <iostream>

#undef X86_FUNC
#undef X86_WEAK_FUNC
#define X86_FUNC(name) static void name(X86Context& __restrict ctx, uint8_t* base)
#define X86_WEAK_FUNC(name, impl)
#define __imp__sub_000F11C0 OriginalProfileRatio
#define __imp__sub_000F5E70 UnusedOriginalResourceLoad
#define __imp__sub_000F5270 UnusedOriginalResourceUnload
#include "x86_recomp.26.cpp"
#include "x86_recomp.27.cpp"
#undef __imp__sub_000F11C0
#undef __imp__sub_000F5E70
#undef __imp__sub_000F5270

extern "C" void __imp__sub_000F11C0(X86Context& ctx, uint8_t* base) { OriginalProfileRatio(ctx, base); }
namespace mesh_fixture {
using kernel::R32;
using kernel::W32;
constexpr uint32_t root = 0x50000, original_table = 0x50100, model = 0x150000;
constexpr uint32_t render = 0x160000, cfg = 0x180000, other_render = 0x190000, other_cfg = 0x1B0000;
constexpr uint32_t node = 0x1D0000, zero_descriptor = 0x56000, zero_items = 0x56100;
constexpr std::array<float, 8> stock = {.415532f, .47585f, .538852f, .6f, .66f, .725f, .782f, .83f};
constexpr std::array<float, 26> extra = {.3f,.325f,.35f,.375f,.4f,.425f,.45f,.475f,.5f,.525f,.55f,.575f,
    .625f,.65f,.675f,.7f,.75f,.775f,.8f,.825f,.85f,.875f,.9f,.925f,.95f,.975f};
std::vector<uint32_t> requested;
unsigned generation = 1, unloads = 0;
uint32_t allocation_seen_during_unload = 0;

float Ratio(unsigned p) { return p < 8 ? stock[p] : p < 34 ? extra[p-8] : stock[7]; }
unsigned Nearest(unsigned p) {
    if (p < 8) return p;
    if (p >= 34) return 7;
    unsigned best = 0;
    for (unsigned i = 1; i < 8; ++i)
        if (std::fabs(Ratio(p)-stock[i]) < std::fabs(Ratio(p)-stock[best])) best = i;
    return best;
}
uint32_t Resource(unsigned p, unsigned brand) { return generation * 0x10000 + p * 0x100 + brand * 4; }
void F(uint32_t address, float value) { std::memcpy(fixture::memory.data()+address, &value, 4); }
float F(uint32_t address) { float value; std::memcpy(&value, fixture::memory.data()+address, 4); return value; }
void Load() {
    W32(0x509E1C, root);
    W32(root+0x2C, original_table); W32(root+0x44, 8);
    W32(zero_descriptor, zero_items); W32(zero_descriptor+4, 1); W32(zero_items, 0);
    for (unsigned offset : {0x28,0x30,0x34,0x38}) W32(root+offset, zero_descriptor);
    for (unsigned p = 0; p < 8; ++p) {
        const uint32_t descriptor = 0x51000+p*16, items = 0x52000+p*0x100;
        W32(original_table+4*p, descriptor); W32(descriptor, items); W32(descriptor+4, 11);
        for (unsigned brand = 0; brand < 11; ++brand) W32(items+4*brand, Resource(p,brand));
    }
    // Authentic adjacent values from decal.xbck: these are not profiles 8 onward.
    W32(original_table+32, 0x82A05B60); W32(original_table+36, 0xA);
    W32(original_table+40, 0xCDCDCDCD);
}
void Unload() {
    assert(R32(root+0x2C) == original_table && R32(root+0x44) == 8);
    assert(fixture::allocations.contains(allocation_seen_during_unload));
    W32(0x509E1C, 0); std::memset(fixture::memory.data()+root, 0xDD, 0x48); ++unloads;
}
void SetProfiles(uint32_t r, uint32_t c, unsigned front, unsigned rear) {
    fixture::memory[c+0x1C]=uint8_t(front); fixture::memory[c+0x1D]=uint8_t(rear);
    W32(r+0x11B4, uint32_t(int32_t(int8_t(front)))); W32(r+0x11B8, uint32_t(int32_t(int8_t(rear))));
    W32(r+0x11BC, 20); W32(r+0x11C0, 23);
    F(r+0x12D4, F(0x4B3910+20*4)/Ratio(front)); F(r+0x12CC, F(0x4B3910+23*4)/Ratio(rear));
}
float ReadRatio(uint32_t r, unsigned axle) {
    auto context = fixture::Call(sub_000F11C0, r, {axle});
    assert(context.esp == fixture::stack+8);
    return float(context.st[context.top]);
}
void Request(uint32_t r, unsigned brand, unsigned fp, unsigned rp) {
    W32(model+0x2F0,r); W32(model+0x18,node); fixture::memory[node+0xE4]=0;
    W32(r+0x11B0,brand); W32(r+0x11AC,0); W32(r+0x11E0,0); W32(r+0x11C4,0);
    requested.clear(); auto context = fixture::Call(__imp__sub_000F6A20,model);
    assert(context.esp == fixture::stack+4);
    assert((requested == std::vector<uint32_t>{Resource(Nearest(fp),brand), Resource(Nearest(rp),brand)}));
}
}
extern "C" void sub_001B6A10(X86Context& ctx, uint8_t*) {
    mesh_fixture::requested.push_back(ctx.ecx); ctx.esp += 8;
}
extern "C" void sub_000F5E10(X86Context& ctx, uint8_t*) { ctx.eax=0; ctx.esp+=4; }

int main() {
    using namespace mesh_fixture;
    for (unsigned i=0;i<8;++i) F(0x471958+4*i,stock[i]);
    for (unsigned rim=12;rim<=28;++rim) F(0x4B3910+4*rim, float(rim+3)*.0254f);
    fixture::profile_resource_load=Load; fixture::profile_resource_unload=Unload;
    fixture::Call(sub_000F5E70);
    const uint32_t first_alias=R32(root+0x2C);
    assert(first_alias != original_table && fixture::allocations.at(first_alias)==34*4 && R32(root+0x44)==8);
    for (unsigned i=0;i<34;++i) assert(R32(first_alias+4*i)==R32(original_table+4*Nearest(i)));
    for (unsigned p=0;p<34;++p) {
        const unsigned rear=33-p;
        SetProfiles(render,cfg,p,rear);
        const float fd=F(render+0x12D4), rd=F(render+0x12CC);
        GarageNormalizeProfileMeshes(render,cfg);
        assert(R32(render+0x11B4)==p && R32(render+0x11B8)==rear);
        assert(F(render+0x12D4)==fd && F(render+0x12CC)==rd);
        assert(ReadRatio(render,0)==Ratio(p) && ReadRatio(render,1)==Ratio(rear));
        assert(fixture::memory[cfg+0x1C]==p && fixture::memory[cfg+0x1D]==rear);
        for (unsigned brand=0;brand<11;++brand) Request(render,brand,p,rear);
    }
    allocation_seen_during_unload=first_alias; fixture::Call(sub_000F5270);
    assert(!fixture::allocations.contains(first_alias) && unloads==1);

    // Same resource address, different assets: no stale alias, double-free or descriptor ownership transfer.
    ++generation; fixture::Call(sub_000F5E70);
    const uint32_t second_alias=R32(root+0x2C);
    SetProfiles(render,cfg,8,33); GarageNormalizeProfileMeshes(render,cfg); Request(render,10,8,33);
    // A fresh resource image can also occupy the same address before our next helper call. Do not restore an
    // old table over that new root, and do not retain aliases to descriptors from its previous generation.
    ++generation; Load(); GarageNormalizeProfileMeshes(render,cfg);
    assert(!fixture::allocations.contains(second_alias));
    const uint32_t replaced_alias=R32(root+0x2C);
    assert(replaced_alias!=original_table && R32(root+0x44)==8); Request(render,10,8,33);
    allocation_seen_during_unload=replaced_alias; fixture::Call(sub_000F5270);

    // Allocation failure must never expose codes8..33 to the original eight-entry mesh table. Both independent
    // renders retain their exact numeric profiles, even while retries keep failing for a later render.
    fixture::fail_profile_allocation=true; ++generation; fixture::Call(sub_000F5E70);
    assert(R32(root+0x2C)==original_table);
    SetProfiles(render,cfg,8,33); GarageNormalizeProfileMeshes(render,cfg);
    SetProfiles(other_render,other_cfg,16,25); GarageNormalizeProfileMeshes(other_render,other_cfg);
    assert(R32(render+0x11B4)==Nearest(8) && R32(render+0x11B8)==Nearest(33));
    assert(ReadRatio(render,0)==Ratio(8) && ReadRatio(render,1)==Ratio(33));
    assert(ReadRatio(other_render,0)==Ratio(16) && ReadRatio(other_render,1)==Ratio(25));
    Request(render,5,8,33); Request(other_render,7,16,25);
    // A destroyed/reused render whose cfg or computed diameter no longer matches cannot inherit the fallback.
    fixture::memory[cfg+0x1C]=0; assert(ReadRatio(render,0)==stock[Nearest(8)]);
    fixture::memory[cfg+0x1C]=8; F(render+0x12D4,1.f);
    assert(ReadRatio(render,0)==stock[Nearest(8)]); // Reused render with a different diameter also rejects it.
    SetProfiles(render,cfg,2,5); GarageNormalizeProfileMeshes(render,cfg);
    assert(ReadRatio(render,0)==stock[2] && ReadRatio(render,1)==stock[5]); Request(render,3,2,5);

    // Recovery after a failed allocation restores full mesh codes without requiring another garage purchase.
    fixture::fail_profile_allocation=false;
    GarageNormalizeProfileMeshes(other_render,other_cfg);
    assert(R32(other_render+0x11B4)==16 && R32(other_render+0x11B8)==25);
    assert(ReadRatio(other_render,0)==Ratio(16) && ReadRatio(other_render,1)==Ratio(25));
    Request(other_render,1,16,25);
    const uint32_t recovered=R32(root+0x2C);
    // Signed malformed/unknown codes normalize before the unguarded original resource loads.
    SetProfiles(render,cfg,127,128); GarageNormalizeProfileMeshes(render,cfg);
    assert(R32(render+0x11B4)==7 && R32(render+0x11B8)==7); Request(render,0,127,128);
    allocation_seen_during_unload=recovered; fixture::Call(sub_000F5270);
    assert(fixture::allocations.empty() && unloads==3);
    std::cout << "Profile meshes: 748 original axle/brand lookups, 68 exact ratios, allocation failure/recovery, "
                 "stock descriptors and resource/render reuse: PASS\n";
}
