#include "gpu/pbr_light_proof.h"

#include <array>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <type_traits>

#define CHECK(condition) do { if(!(condition)) { std::cerr<<"Check failed: "<<#condition<<" at line "<<__LINE__<<'\n';std::exit(1); } } while(false)

namespace {
namespace lp=gpu::pbr::light_proof;
constexpr uint32_t ok=0,fail=0x8007000eu;
bool Ready(const lp::Proof& proof,uint32_t shadow,bool zero=false) {
    const auto o=proof.Snapshot(shadow,zero);
    return o.light_writes_after_success&&o.light_index_coverage&&o.stateblock_valid;
}
void Enable(lp::Proof& proof,uint32_t index,uint32_t on=1,uint32_t result=ok) {
    proof.BeginEnable(index);proof.EnableResult(index,on,result);
}
lp::Proof Authored(uint32_t index=0) {
    lp::Proof p;p.DeviceReset();p.SetLightResult(index,true,ok);Enable(p,index);
    CHECK(Ready(p,1u<<index));return p;
}
void InitialAndZero() {
    lp::Proof p;
    auto o=p.Snapshot(0,true);
    CHECK(!o.light_writes_after_success&&!o.light_index_coverage&&!o.stateblock_valid);
    p.SetLightResult(0,true,ok);Enable(p,0);CHECK(!Ready(p,1)); // Writes do not invent a device/reset observation.
    p.DeviceReset();o=p.Snapshot(0);
    CHECK(!o.light_writes_after_success&&o.light_index_coverage&&o.stateblock_valid);
    CHECK(o.defined_mask==0&&o.enabled_mask==0&&o.enable_known_mask==0xff);
    CHECK(Ready(p,0,true)); // Optional black scene, with a complete actually reset all-disabled baseline.
    CHECK(!Ready(p,0));
    p.BeginEnable(0);CHECK(!Ready(p,0,true)&&p.Snapshot(0,true).pending_enable_depth==1);
    p.EnableResult(0,0,ok);CHECK(Ready(p,0,true));
    CHECK(!Ready(p,0x100,true)); // The renderer shadow cannot silently add an unrepresented API index.
}
void DefaultSynthesis() {
    lp::Proof p;p.DeviceReset();
    // Actual original LightEnable: undefined record -> nested SetLight(default) -> enable.
    p.BeginEnable(0);p.SetLightResult(0,true,ok);p.EnableResult(0,1,ok);
    CHECK(p.Snapshot(1).defined_mask==0&&!Ready(p,1));
    p.SetLightResult(0,true,ok);CHECK(Ready(p,1)); // Later authored data, same known enable state.
    Enable(p,0,0xffffffffu);CHECK(Ready(p,1)); // Original BOOL uses any nonzero value.
    // Even an apparently old definition must not survive a subsequently observed default overwrite.
    p.BeginEnable(0);p.SetLightResult(0,true,ok);p.EnableResult(0,1,ok);
    CHECK(!Ready(p,1)&&p.Snapshot(1).defined_mask==0);
    p.SetLightResult(0,true,ok);Enable(p,0,0);CHECK(Ready(p,0,true));
    // LightEnable(false) also synthesizes before checking the BOOL in this original implementation.
    p.BeginEnable(1);p.SetLightResult(1,true,ok);p.EnableResult(1,0,ok);
    CHECK(Ready(p,0,true)&&!(p.Snapshot(0,true).defined_mask&2));
    Enable(p,1);CHECK(!Ready(p,2));
}
void FailuresAndMismatches() {
    auto p=Authored();
    CHECK(!Ready(p,0)&&!Ready(p,2));
    p.SetLightResult(0,true,fail);CHECK(!Ready(p,1)&&p.Snapshot(1).defined_mask==0);
    p.SetLightResult(0,true,ok);CHECK(Ready(p,1));
    p.SetLightResult(0,false,ok);CHECK(!Ready(p,1)); // Nonzero pointer alone is not full copied-record proof.
    p.SetLightResult(0,true,1);CHECK(Ready(p,1)); // SUCCEEDED HRESULT convention, including S_FALSE.
    Enable(p,0,0,fail);
    auto o=p.Snapshot(1);CHECK(!Ready(p,1)&&o.enabled_mask==1&&!(o.enable_known_mask&1));
    CHECK(!Ready(p,0,true)); // Do not trust a legacy pre-original shadow update after failure.
    Enable(p,0,0,ok);CHECK(Ready(p,0,true));
    Enable(p,0,1,ok);CHECK(Ready(p,1));
    p.SetLightResult(1,true,fail);CHECK(!Ready(p,1)); // Inactive-slot write failure still invalidates observation.
    p.SetLightResult(1,true,ok);CHECK(Ready(p,1));
    p.BeginEnable(2);p.SetLightResult(2,true,fail);p.EnableResult(2,1,fail);CHECK(!Ready(p,1));
    p.SetLightResult(2,true,ok);CHECK(!Ready(p,1)); // Both failed record and enable obligations must recover.
    Enable(p,2,0);CHECK(Ready(p,1));
}
void ArbitraryIndices() {
    for(uint32_t index:{8u,16u,std::numeric_limits<uint32_t>::max()})for(uint32_t result:{ok,fail}) {
        auto p=Authored(7);p.SetLightResult(index,true,result);
        CHECK(!p.Snapshot(128).light_index_coverage&&!Ready(p,128));
        p.SetLightResult(7,true,ok);Enable(p,7);CHECK(!Ready(p,128));
        p.DeviceReset();p.SetLightResult(7,true,ok);Enable(p,7);CHECK(Ready(p,128));
        Enable(p,index,1,result);CHECK(!Ready(p,128)&&!p.Snapshot(128).light_index_coverage);
        p.SetLightResult(7,true,ok);Enable(p,7,0);CHECK(!Ready(p,0,true));
    }
}
void StateBlockAndCopy() {
    for(auto reason:{lp::Invalidation::StateBlockCapture,lp::Invalidation::StateBlockApply,
                     lp::Invalidation::StateBlockCopy,lp::Invalidation::LightReset,
                     lp::Invalidation::UnknownLightCopy,lp::Invalidation::DeviceFailure}) {
        auto original=Authored();auto copied=original;
        CHECK(Ready(copied,1)); // Ordinary immutable render-State copies retain exactly the same proof.
        copied.Invalidate(reason);
        CHECK(Ready(original,1)&&!copied.Snapshot(1).stateblock_valid&&!Ready(copied,1));
        for(uint32_t i=0;i<8;++i){copied.SetLightResult(i,true,ok);Enable(copied,i,i==0);}
        CHECK(!Ready(copied,1)); // Setters cannot certify an unverified copied/original stateblock.
        copied.DeviceReset();CHECK(Ready(copied,0,true));
    }
}
void ScopeProtocol() {
    lp::Proof p;p.DeviceReset();p.SetLightResult(0,true,ok);p.SetLightResult(1,true,ok);
    p.BeginEnable(0);p.BeginEnable(1);p.EnableResult(1,1,ok);
    CHECK(!Ready(p,2));p.EnableResult(0,1,ok);CHECK(Ready(p,3));
    p.EnableResult(0,1,ok);CHECK(!p.Snapshot(3).stateblock_valid&&!Ready(p,3)); // Missing pre-original scope.
    p.SetLightResult(0,true,ok);Enable(p,0);CHECK(!Ready(p,3));
    p.DeviceReset();p.SetLightResult(0,true,ok);Enable(p,0);CHECK(Ready(p,1));
    p.BeginEnable(0);p.BeginEnable(1);p.EnableResult(0,1,ok);p.EnableResult(1,1,ok);
    CHECK(!Ready(p,3)&&!p.Snapshot(3).stateblock_valid); // Mismatched nesting fails closed.
    p.DeviceReset();for(unsigned i=0;i<9;++i)p.BeginEnable(0);
    CHECK(!Ready(p,0,true)&&!p.Snapshot(0,true).stateblock_valid); // Bounded context storage.
    for(unsigned i=0;i<9;++i)p.EnableResult(0,0,ok);
    CHECK(!Ready(p,0,true));p.DeviceReset();CHECK(Ready(p,0,true));
}
} // namespace

int main() {
    static_assert(std::is_trivially_copyable_v<lp::Proof>);
    static_assert(sizeof(lp::Proof)<=96);
    InitialAndZero();DefaultSynthesis();FailuresAndMismatches();ArbitraryIndices();StateBlockAndCopy();ScopeProtocol();
    std::cout<<"Post-original authored-light observations, synthetic defaults, failure/index/stateblock gates passed\n";
}
