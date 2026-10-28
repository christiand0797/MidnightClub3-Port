#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "app/tuning_profiles.h"
#include "../templates/game/src/game/tuning_profiles.h"
#include "kernel/memory.h"
#include "xdk/hle.h"
#include <array>
#include <cassert>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <mutex>
#include <vector>

namespace fixture {
std::filesystem::path folder;
std::vector<uint8_t> guest(0x600000);
constexpr uint32_t profile=0x53F308,cars=profile+0x508,stack=0x70000,clone=0x20000;
int instance=7;
}
namespace kernel {uint8_t* g_base=fixture::guest.data();}
namespace mem {bool IsCommitted(uint32_t va){return va<fixture::guest.size();}}
namespace app {
Settings& Get(){static Settings s;return s;}
std::filesystem::path ExeDir(){return fixture::folder;}
int Instance(){return fixture::instance;}
std::recursive_mutex& UiLock(){static std::recursive_mutex lock;return lock;}
}
namespace p=app::tuning_profiles;
namespace broker=mc3::car_tuning;
namespace fs=std::filesystem;
using kernel::W32;
using kernel::R32;
X86_EXTERN_FUNC(sub_00271D20);
X86_EXTERN_FUNC(sub_00271E20);
X86_EXTERN_FUNC(sub_0026BB80);
X86_EXTERN_FUNC(sub_0026BD10);

X86_FUNC(__imp__sub_00271D20){W32(ctx.ecx,0x4483DC);ctx.eax=ctx.ecx;ctx.esp+=4;}
X86_FUNC(__imp__sub_00271E20){
    const auto src=R32(ctx.esp+4);std::memmove(base+ctx.ecx+0x0C,base+src+0x0C,0x106-0x0C);
    W32(ctx.ecx+8,0x3FFF);ctx.eax=ctx.ecx;ctx.esp+=8;
}
X86_FUNC(sub_00270A90){
    std::memset(base+ctx.ecx+0xF9,0,13);ctx.esp+=8;
}
void Invoke(X86Func* fn,uint32_t self,uint32_t arg=0,uint32_t caller=0xBEEF){
    X86Context ctx{};ctx.ecx=self;ctx.esp=fixture::stack;
    W32(ctx.esp,caller);W32(ctx.esp+4,arg);fn(ctx,fixture::guest.data());
    assert(ctx.esp==fixture::stack+8);
}
bool Near(float a,float b){return std::abs(a-b)<.00001f;}
void Select(unsigned n){kernel::G<uint8_t>(fixture::profile+0x505)=uint8_t(n);broker::ObserveFrame();}
void SetRide(float height){std::lock_guard lock(app::UiLock());p::Editor().ride_front=height;assert(p::CommitEditor());}

int main(){
    const auto temp=fs::canonical(fs::temp_directory_path());
    fixture::folder=temp/("mc3-car-profile-game-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(GetTickCount64()));
    assert(!fs::exists(fixture::folder)&&fs::create_directory(fixture::folder));assert(fs::equivalent(fixture::folder.parent_path(),temp));
    W32(0x53F2D4,5);W32(0x557F0C,0x30000);W32(0x30000,0x473B98);W32(0x3000C,0x40000);W32(0x40000,0x474288);W32(0x40050,0);
    std::memcpy(fixture::guest.data()+fixture::profile+0x2E8,"Player 1",9);
    kernel::G<uint8_t>(fixture::profile+0x504)=2;
    for(unsigned i=0;i<2;++i){const auto cfg=fixture::cars+i*0x108;W32(cfg,0x4483DC);std::memcpy(fixture::guest.data()+cfg+0xE3,"vp_belair_57",12);}
    p::Load();Select(0);const auto first=p::Snapshot().key;SetRide(-.3f);
    Select(1);const auto second=p::Snapshot().key;assert(second!=first);SetRide(.5f);assert(p::Save());
    auto bank=broker::CaptureBank();assert(Near(bank.ForCfg(fixture::cars).ride_front,-.3f));assert(Near(bank.ForCfg(fixture::cars+0x108).ride_front,.5f));
    W32(fixture::clone,0x4483DC);Invoke(sub_00271E20,fixture::clone,fixture::cars+0x108);
    assert(Near(broker::CaptureBank().ForCfg(fixture::clone).ride_front,.5f));
    // An unrelated same-model configuration never inherits a selected car's setup.
    W32(0x21000,0x4483DC);std::memcpy(fixture::guest.data()+0x21000+0xE3,"vp_belair_57",12);
    assert(broker::CaptureBank().ForCfg(0x21000).ride_front==0);
    X86Context ctor{};ctor.ecx=fixture::clone;ctor.esp=fixture::stack;sub_00271D20(ctor,fixture::guest.data());
    assert(ctor.esp==fixture::stack+4&&broker::CaptureBank().ForCfg(fixture::clone).ride_front==0);
    // Actual save-image copy persists a changed roster signature without F1 reopening.
    kernel::G<uint8_t>(fixture::cars+0x108+0x18)=5;broker::ObserveFrame();assert(p::Dirty());
    const auto live_before=std::vector<uint8_t>(fixture::guest.begin()+fixture::profile,fixture::guest.begin()+fixture::profile+0x18340);
    Invoke(sub_0026BD10,0x100000,fixture::profile,0x1A5A4);
    assert(!p::Dirty()&&std::equal(live_before.begin(),live_before.end(),fixture::guest.begin()+0x100000));
    assert(std::equal(live_before.begin(),live_before.end(),fixture::guest.begin()+fixture::profile));
    W32(0x53F2D4,0);broker::ObserveFrame();p::Load();W32(0x53F2D4,5);Select(1);
    assert(p::Snapshot().key==second&&Near(p::Effective().ride_front,.5f));
    // Execute original compaction, retain the second twin's PC ID after selling first.
    Invoke(sub_0026BB80,fixture::profile,0);assert(kernel::G<uint8_t>(fixture::profile+0x504)==1);
    assert(p::Snapshot().key==second&&Near(broker::CaptureBank().ForCfg(fixture::cars).ride_front,.5f));
    assert(broker::CaptureBank().ForCfg(fixture::cars+0x108).ride_front==0);
    assert(p::Save());
    // Reloading a different external roster cannot silently retarget a saved override.
    W32(0x53F2D4,0);broker::ObserveFrame();p::Load();kernel::G<uint8_t>(fixture::cars+0x18)=7;W32(0x53F2D4,5);Select(0);
    assert(p::Snapshot().key!=second&&!p::Snapshot().override_enabled);
    // Per-car settings cannot alter the shared garage database controls.
    app::Get().tuning.wheel_limit=2.25f;{std::lock_guard lock(app::UiLock());auto& t=p::Editor();t.wheel_limit=3;t.garage_extended=false;assert(p::CommitEditor());}
    assert(broker::CaptureBank().ForCfg(fixture::cars).wheel_limit==2.25f&&broker::CaptureBank().ForCfg(fixture::cars).garage_extended);
    const auto last_car=p::Snapshot().key;
    Invoke(sub_0026BB80,fixture::profile,0);assert(kernel::G<uint8_t>(fixture::profile+0x504)==0&&!p::Snapshot().ready);
    kernel::G<uint8_t>(fixture::profile+0x504)=1;Select(0);
    assert(p::Snapshot().key!=last_car&&!p::Snapshot().override_enabled);
    assert(fs::remove(fixture::folder/"config/tuning_profiles.toml")&&fs::remove(fixture::folder/"config")&&fs::remove(fixture::folder));
    std::cout<<"per-config broker: distinct twins, copy lineage, constructor reuse, original save-copy/compaction and safe reload passed\n";
}
