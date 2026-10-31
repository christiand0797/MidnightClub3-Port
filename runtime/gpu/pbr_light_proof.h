#pragma once
// Observations only: never writes legacy light records/enables, reads guest pointers or infers linear colour.
// The caller serializes these methods with the same lock as its State light shadow. Plain State snapshots
// may copy this value; an unverified ORIGINAL stateblock/light copy must instead call Invalidate explicitly.
#include <array>
#include <cstdint>

namespace gpu::pbr::light_proof {

enum class Invalidation { StateBlockCapture, StateBlockApply, StateBlockCopy, LightReset, UnknownLightCopy, DeviceFailure };
struct Observation {
    bool light_writes_after_success = false;
    bool light_index_coverage = false;
    bool stateblock_valid = false;
    uint32_t defined_mask = 0, enabled_mask = 0, enable_known_mask = 0;
    uint32_t pending_enable_depth = 0;
};

struct Proof {
    // Call only after a real successful new-device/reset operation establishes all lights disabled.
    // A default-constructed tracker has no such observation and cannot approve even a black scene.
    void DeviceReset() noexcept {
        *this={};initialized_=true;enable_known_=kMask;
    }
    // Required around the actual original LightEnable call. Original 0x412270 may call SetLight
    // (0x4122D6 -> 0x411FB0) to synthesize an undefined light; that is NOT an authored-light write.
    void BeginEnable(uint32_t index) noexcept {
        ObserveIndex(index);
        if(depth_>=pending_.size()){protocol_uncertain_=true;return;}
        pending_[depth_++]=index;
    }
    // Observe ONLY after original returns. pointer_valid means the caller actually copied/read the
    // complete original light record, not merely that the guest integer address was nonzero.
    void SetLightResult(uint32_t index,bool pointer_valid,uint32_t hresult) noexcept {
        if(!ObserveIndex(index))return;
        const uint32_t bit=1u<<index;
        if(!Succeeded(hresult)||!pointer_valid) {
            defined_&=~bit;set_uncertain_|=bit;return;
        }
        if(depth_) {
            defined_&=~bit; // Default synthesis must not retain an older authored definition either.
            return;
        }
        defined_|=bit;set_uncertain_&=~bit;
    }
    // on is the original BOOL (any nonzero value means enabled). Pairs with BeginEnable in LIFO order.
    void EnableResult(uint32_t index,uint32_t on,uint32_t hresult) noexcept {
        if(!depth_||pending_[depth_-1]!=index)protocol_uncertain_=true;
        if(depth_)--depth_;
        if(!ObserveIndex(index))return;
        const uint32_t bit=1u<<index;
        if(!Succeeded(hresult)) {
            enable_known_&=~bit;enable_uncertain_|=bit;return;
        }
        enabled_=on?enabled_|bit:enabled_&~bit;
        enable_known_|=bit;enable_uncertain_&=~bit;
    }
    // Conservatively sticky until DeviceReset. Later setters cannot certify an unobserved stateblock
    // or copied light list; this initial contract deliberately provides no guessed reconstruction path.
    void Invalidate(Invalidation) noexcept {
        stateblock_uncertain_=true;defined_=0;enable_known_=0;
    }
    Observation Snapshot(uint32_t actual_shadow_enabled,bool allow_complete_zero_enabled=false) const noexcept {
        Observation out;
        out.defined_mask=defined_;out.enabled_mask=enabled_;out.enable_known_mask=enable_known_;
        out.pending_enable_depth=depth_;
        out.light_index_coverage=initialized_&&!arbitrary_index_;
        out.stateblock_valid=initialized_&&!stateblock_uncertain_&&!protocol_uncertain_;
        out.light_writes_after_success=initialized_&&!depth_&&!set_uncertain_&&!enable_uncertain_&&
            enable_known_==kMask&&!(actual_shadow_enabled&~kMask)&&actual_shadow_enabled==enabled_&&
            (defined_&enabled_)==enabled_&&(enabled_||allow_complete_zero_enabled);
        return out;
    }
private:
    static constexpr uint32_t kMask=0xff;
    static constexpr bool Succeeded(uint32_t result) noexcept { return !(result&0x80000000u); }
    bool ObserveIndex(uint32_t index) noexcept {
        if(index<8)return true;
        arbitrary_index_=true;return false; // Even later supported-index writes cannot repair this coverage gap.
    }
    std::array<uint32_t,8> pending_{};
    uint32_t depth_=0,defined_=0,enabled_=0,enable_known_=0,set_uncertain_=0,enable_uncertain_=0;
    bool initialized_=false,arbitrary_index_=false,stateblock_uncertain_=false,protocol_uncertain_=false;
};

} // namespace gpu::pbr::light_proof
