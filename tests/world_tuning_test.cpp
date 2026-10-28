#include "app/app.h"
#include "kernel/memory.h"
#include "xdk/hle.h"
#include "world_tuning.h"

#include <array>
#include <cassert>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <vector>

namespace {
std::vector<uint8_t> guest(0x600000);
std::array<bool, 0x600> committed;
constexpr uint32_t kStack=0x70000, kMission=0x10000, kTraffic=0x11000, kRoads=0x12000;
constexpr uint32_t kNodes=0x13000, kPaths=0x14000, kPedWrapper=0x15000, kPeds=0x16000;
constexpr uint32_t kTypes=0x17000, kRegions=0x18000, kController=0x19000, kCar=0x1A000;
void Float(uint32_t va,float value) { std::memcpy(guest.data()+va,&value,4); }
float Float(uint32_t va) { float v; std::memcpy(&v,guest.data()+va,4);return v; }
void Word(uint32_t va,uint16_t v) { std::memcpy(guest.data()+va,&v,2); }
void Dword(uint32_t va,uint32_t v) { std::memcpy(guest.data()+va,&v,4); }
bool Near(float a,float b) { return std::fabs(a-b)<1e-6f; }
void Call(X86Func* fn,uint32_t object,float value) {
    X86Context ctx{};ctx.ecx=object;ctx.esp=kStack;
    Dword(kStack,0x12345678);Float(kStack+4,value);
    std::array<uint8_t,4> arg;std::memcpy(arg.data(),guest.data()+kStack+4,4);
    fn(ctx,guest.data());
    assert(ctx.esp==kStack+8); // Original thiscall's ret4 is retained.
    assert(!std::memcmp(arg.data(),guest.data()+kStack+4,4)); // Caller-owned bytes restored.
}
void Apply() {
    X86Context ctx{};ctx.esp=kStack;ctx.eax=0xABC;ctx.ecx=0xDEF;ctx.cf=1;
    const X86Context before=ctx;
    std::array<uint8_t,128> stack;std::memcpy(stack.data(),guest.data()+kStack-128,128);
    mc3::world::Apply(ctx,guest.data());
    assert(!std::memcmp(&before,&ctx,sizeof(ctx)));
    assert(!std::memcmp(stack.data(),guest.data()+kStack-128,128));
}
}  // namespace
namespace app {
Settings& Get() { static Settings s;return s; }
std::recursive_mutex& UiLock() { static std::recursive_mutex lock;return lock; }
}
namespace kernel { uint8_t* g_base=guest.data(); }
namespace mem { bool IsCommitted(uint32_t va) { return va<guest.size()&&committed[va/kPage]; } }
// Controller init has unrelated car setup; the fixture retains its exact observed
// +0x50 backpointer/ret8 contract. Population arithmetic below uses retail bodies.
X86_FUNC(__imp__sub_002C89F0) { Dword(ctx.ecx+0x50,kernel::R32(ctx.esp+4));ctx.esp+=12; }
X86_EXTERN_FUNC(sub_002B9670);
X86_EXTERN_FUNC(sub_00222770);
X86_EXTERN_FUNC(sub_002C89F0);
X86_EXTERN_FUNC(FixturePopulationTrafficRequest);
X86_EXTERN_FUNC(FixturePopulationAvailableAmbient);

int main() {
    committed.fill(true);
    Dword(0x53F2D4,2); // Original career/player dispatch state, also observed on native streets.
    Dword(0x53F2E0,kMission);Dword(kMission+0x18,0);
    Dword(0x5587E0,kTraffic);Word(kTraffic+0x28,1);Dword(kTraffic+0x2C,kRoads);Dword(kTraffic+0x3C,kPaths);
    guest[kRoads+4]=1;Dword(kRoads+0x2C,kNodes);Word(kNodes,0);Word(kPaths+8,1000);
    Dword(0x53891C,kPedWrapper);Dword(kPedWrapper,kPeds);
    Dword(kPeds+0x0C,kTypes);Dword(kPeds+0x1C,kRegions);Word(kPeds+0x20,2);
    Float(kTypes+0x24,32);Float(kTypes+0x28+0x24,200);
    guest[kRegions+2]=10;guest[kRegions+5]=0xFF; // Positive authored cap versus uncapped region.
    Float(0x447E50,0.01f);Float(0x44AC74,127);Float(0x447C10,0.75f);
    app::Get().world={2,3};
    Call(sub_002B9670,kRoads,0.02f);Call(sub_00222770,kPeds,0.5f);
    const float original_target=Float(kRoads+0x28);
    assert(Near(Float(kRoads+0x20),0.02f));
    assert(guest[kRegions+1]==10&&guest[kRegions+4]==100);
    Apply();assert(!mc3::world::GetSnapshot().free_roam_player);

    Dword(kController,0x47684C);Dword(kCar+4,kController);
    X86Context init{};init.ecx=kController;init.esp=kStack;Dword(kStack+4,kCar);
    sub_002C89F0(init,guest.data());assert(init.esp==kStack+12);
    Apply();
    assert(mc3::world::GetSnapshot().free_roam_player);
    assert(mc3::world::GetSnapshot().traffic_roads_ready==1&&mc3::world::GetSnapshot().pedestrians_ready);
    assert(Near(Float(kRoads+0x20),0.04f)&&Near(Float(kRoads+0x28),original_target*2));
    // The retail positive authored cap10 and global target cap127 remain in
    // force even when the density multiplier passes the original slider's1.
    assert(guest[kRegions+1]==10&&guest[kRegions+4]==127);
    Apply();assert(Near(Float(kRoads+0x20),0.04f)); // Never compound2x on each frame.

    // Ordinary career streets after leaving the garage use find_hook (mode1),
    // not the mode0 name "roam". This matches the bounded native R3 capture:
    // state2, mode1, typed human controller, reciprocal car links and AI flag0.
    Dword(kMission+0x18,1);Apply();
    assert(mc3::world::GetSnapshot().free_roam_player);
    assert(Near(Float(kRoads+0x20),0.04f)&&guest[kRegions+4]==127);
    Apply();assert(Near(Float(kRoads+0x20),0.04f));
    // The actual original setter hooks must accept mode1 too, not just replay
    // previously observed mode0 baselines when a menu value changes.
    Call(sub_002B9670,kRoads,0.03f);Call(sub_00222770,kPeds,0.1f);
    assert(Near(Float(kRoads+0x20),0.06f)&&guest[kRegions+4]==60);
    app::Get().world={4,4};Apply();
    assert(Near(Float(kRoads+0x20),0.12f)&&guest[kRegions+4]==80);
    // Unproven hookman_cruise (mode2), follow and every race/story mode retain
    // their authored densities. Returning to the proven street mode reapplies
    // from the native baseline, with no compounded multiplier.
    for (uint32_t mode=2;mode<23;++mode) {
        Dword(kMission+0x18,mode);Apply();
        assert(!mc3::world::GetSnapshot().free_roam_player);
        assert(Near(Float(kRoads+0x20),0.03f)&&guest[kRegions+4]==20);
        Dword(kMission+0x18,1);Apply();
        assert(mc3::world::GetSnapshot().free_roam_player);
        assert(Near(Float(kRoads+0x20),0.12f)&&guest[kRegions+4]==80);
    }
    Dword(kMission+0x18,0xFFFFFFFF);Apply();
    assert(!mc3::world::GetSnapshot().free_roam_player&&Near(Float(kRoads+0x20),0.03f));
    // A lingering mission/player pointer in garage/title/other states must not
    // give a preview scene population authority, for either allowed mode.
    for (uint32_t mode : {0u,1u}) {
        Dword(kMission+0x18,mode);
        for (uint32_t state : {0u,1u,3u,4u,5u,6u,0xFFFFFFFFu}) {
            Dword(0x53F2D4,state);Apply();
            assert(!mc3::world::GetSnapshot().free_roam_player);
            assert(Near(Float(kRoads+0x20),0.03f)&&guest[kRegions+4]==20);
        }
        Dword(0x53F2D4,2);Apply();
        assert(mc3::world::GetSnapshot().free_roam_player);
        assert(Near(Float(kRoads+0x20),0.12f)&&guest[kRegions+4]==80);
    }
    // Mode1 does not relax concrete player ownership or memory validity.
    guest[kCar+0x5E]=1;Apply();
    assert(!mc3::world::GetSnapshot().free_roam_player&&Near(Float(kRoads+0x20),0.03f));
    guest[kCar+0x5E]=0;Dword(kCar+4,0);Apply();
    assert(!mc3::world::GetSnapshot().free_roam_player);
    Dword(kCar+4,kController);Dword(kController,0);Apply();
    assert(!mc3::world::GetSnapshot().free_roam_player);
    Dword(kController,0x47684C);Dword(kController+0x50,kCar+4);Apply();
    assert(!mc3::world::GetSnapshot().free_roam_player);
    Dword(kController+0x50,kCar);Dword(0x53F2E0,0);Apply();
    assert(!mc3::world::GetSnapshot().free_roam_player);
    Dword(0x53F2E0,kMission);committed[kMission/mem::kPage]=false;Apply();
    assert(!mc3::world::GetSnapshot().free_roam_player);
    committed[kMission/mem::kPage]=true;
    app::Get().world={2,3};Dword(kMission+0x18,0);Apply();
    Call(sub_002B9670,kRoads,0.02f);Call(sub_00222770,kPeds,0.5f);

    app::Get().world={0,0};Apply();
    assert(Float(kRoads+0x20)==0&&Float(kRoads+0x28)==0&&guest[kRegions+1]==0&&guest[kRegions+4]==0);
    app::Get().world={1,1};Apply();
    assert(Near(Float(kRoads+0x20),0.02f)&&Near(Float(kRoads+0x28),original_target));
    assert(guest[kRegions+1]==10&&guest[kRegions+4]==100);
    app::Get().world={4,4};Apply();
    Dword(kMission+0x18,7);Apply(); // Scripted checkpoints restore the original densities.
    assert(!mc3::world::GetSnapshot().free_roam_player&&Near(Float(kRoads+0x20),0.02f));
    assert(guest[kRegions+4]==100);
    Call(sub_002B9670,kRoads,0);Call(sub_00222770,kPeds,0);
    Dword(kMission+0x18,0);Apply();
    assert(Float(kRoads+0x20)==0&&guest[kRegions+4]==0); // Mission/no-traffic zero stays zero.

    Call(sub_002B9670,kRoads,0.01f);Call(sub_00222770,kPeds,0.25f);
    assert(Near(Float(kRoads+0x20),0.04f));
    guest[kCar+0x5E]=1;Apply(); // A recycled AI car cannot keep player authority.
    assert(!mc3::world::GetSnapshot().free_roam_player&&Near(Float(kRoads+0x20),0.01f));
    guest[kCar+0x5E]=0;
    app::Get().world={std::numeric_limits<float>::quiet_NaN(),std::numeric_limits<float>::infinity()};Apply();
    assert(Near(Float(kRoads+0x20),0.01f)&&guest[kRegions+4]==50);
    app::Get().world={99,-1};Apply();assert(Near(Float(kRoads+0x20),0.04f)&&guest[kRegions+4]==0);
    Call(sub_002B9670,kRoads,0.9f);assert(Near(Float(kRoads+0x20),3.6f));
    const uint32_t other=kRoads+0x100;
    guest[other+4]=0;Call(sub_002B9670,other,0.25f);assert(Float(other+0x20)==0.25f);

    // Native topology and setter writes supersede stale tracked data.
    Dword(kRoads+0x2C,kNodes+4);Word(kNodes+4,0);Apply();
    assert(mc3::world::GetSnapshot().traffic_roads_ready==0&&Near(Float(kRoads+0x20),3.6f));
    Call(sub_002B9670,kRoads,0.01f);assert(Near(Float(kRoads+0x20),0.04f));
    Float(kRoads+0x20,0.123f);Apply();
    assert(mc3::world::GetSnapshot().traffic_roads_ready==0&&Float(kRoads+0x20)==0.123f);
    Call(sub_002B9670,kRoads,0.01f);
    app::Get().world={2,2};committed[(kStack-128)/mem::kPage]=false;Apply();
    assert(Near(Float(kRoads+0x20),0.04f)); // Unavailable scratch stack: defer; no caller corruption.
    committed[(kStack-128)/mem::kPage]=true;Apply();assert(Near(Float(kRoads+0x20),0.02f));

    // Source-density1 must still allow a real2x/4x target, rather than becoming
    // a no-op at the original UI maximum. Exercise the actual target consumer.
    app::Get().world={1,1};Float(kTypes+0x28+0x24,16);
    Call(sub_002B9670,kRoads,1);Call(sub_00222770,kPeds,1);
    const float maximum_original_target=Float(kRoads+0x28);
    assert(guest[kRegions+4]==16);
    Float(kRoads+0x24,maximum_original_target);Float(0x476028,0.5f);
    X86Context request{};request.esp=kStack;request.ecx=kRoads;
    FixturePopulationTrafficRequest(request,guest.data());assert((request.eax&0xFF)==0);
    app::Get().world={2,2};Apply();
    assert(Float(kRoads+0x20)==2&&Near(Float(kRoads+0x28),maximum_original_target*2));
    assert(guest[kRegions+4]==32&&guest[kRegions+1]==10);
    request={};request.esp=kStack;request.ecx=kRoads;
    FixturePopulationTrafficRequest(request,guest.data());assert((request.eax&0xFF)==1);
    app::Get().world={4,4};Apply();
    assert(Float(kRoads+0x20)==4&&Near(Float(kRoads+0x28),maximum_original_target*4));
    assert(guest[kRegions+4]==64&&guest[kRegions+1]==10);
    // A stronger request still goes through the existing ambient free-pool
    // accessor. An empty pool returns0, populated pools retain their capacity
    // and storage. Density does not size/index this separate allocation path.
    constexpr uint32_t pool=0x1B000,available=0x1C000;
    Dword(pool,available);Word(pool+4,0);Word(pool+6,2);
    Dword(available,0x21000);Dword(available+4,0x22000);
    X86Context allocation{};allocation.esp=kStack;allocation.ecx=pool;Dword(kStack+4,0);
    FixturePopulationAvailableAmbient(allocation,guest.data());assert(allocation.eax==0);
    Word(pool+4,2);allocation={};allocation.esp=kStack;allocation.ecx=pool;Dword(kStack+4,0);
    FixturePopulationAvailableAmbient(allocation,guest.data());assert(allocation.eax==0x22000);
    assert(kernel::G<uint16_t>(pool+4)==2&&kernel::G<uint16_t>(pool+6)==2&&kernel::R32(pool)==available);
    assert(kernel::R32(available)==0x21000&&kernel::R32(available+4)==0x22000);
    std::cout<<"world population: native state2 roam/find_hook guard, scripted-mode restore, retail writers, zero/caps, ownership, topology and stack preservation passed\n";
}
