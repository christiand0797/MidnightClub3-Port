#pragma once
// Real dedicated D3D12 resource/draw implementation, explicitly opt-in and caller-queue-owned.
#include "gpu/pbr_pipeline.h"

#include <functional>
#include <memory>

namespace gpu::pbr::device {

enum class Status {
    Planned, Recorded, Disabled, InvalidLibrary, MaterialRejected, UnsupportedDraw, InvalidMesh, InvalidConstants,
    InvalidSampler, BudgetExceeded, InvalidContext, OwnershipChanged, DeviceFailure, ShaderFailure,
    UploadFailure, RetentionFailure, AllocationFailure
};
const char* StatusText(Status);
struct Limits {
    // Default-heap map allocations are accounted with GetResourceAllocationInfo; driver-internal PSO
    // storage is not queryable here. Descriptor heaps/PSOs additionally have hard entry/count bounds.
    uint64_t resident_bytes=1536ull<<20, draw_upload_bytes=256ull<<20;
    uint32_t materials=4096, psos=256, samplers=256, srv_descriptors=40960;
    bool operator==(const Limits&) const = default;
};
struct Input {
    std::shared_ptr<const Library> library; // Must be the immutable result of LoadReviewedManifest.
    controls::ExactMaterialKey key;
    SurfaceEvidence surface;
    pipeline::Evidence evidence;
    std::span<const pipeline::Vertex> vertices;
    std::span<const uint32_t> indices;
    pipeline::GeometryConstants geometry;
    pipeline::MaterialConstants material;
    pipeline::SceneConstants scene;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC original_opaque_state{};
    D3D12_SAMPLER_DESC actual_sampler{};
    bool coverage_policy_proven=false;
    bool draw_alpha_test=false; // the original draw tests alpha (actual_alpha_cutoff): coverage as it is, any material
    float actual_alpha_cutoff=0;
    // The draw's own accepted colour texture, for a game_albedo material (bound as t0, gamma decoded in the shader).
    // The caller guarantees it is the texture whose content hash the assignment names, in a shader-readable state.
    ID3D12Resource* game_albedo=nullptr;
};
struct MapPlan {
    const Map* map=nullptr; // null denotes an explicit synthetic neutral optional resource.
    MapRole role=MapRole::Albedo;
    DXGI_FORMAT format=DXGI_FORMAT_UNKNOWN;
    uint32_t width=0,height=0,levels=0;
    uint64_t pixel_bytes=0,upload_bytes=0,resident_estimate=0;
};
struct Plan {
    std::shared_ptr<const Library> owner;
    const Material* material=nullptr;
    std::array<MapPlan,pipeline::kMapCount> maps{};
    uint64_t geometry_upload_bytes=0,texture_upload_bytes=0,resident_estimate=0;
    uint32_t triangle_count=0;
    controls::HeightMode height_mode=controls::HeightMode::None;
};
// Pure CPU preflight. Disabled returns before inspecting the library or any spans/callbacks.
// Planned here means a bounded plan, never created resources, GPU execution or native acceptance.
Status PlanDraw(const Input&,const Limits&,Plan& result);
bool ValidSampler(const D3D12_SAMPLER_DESC&);

struct UploadAllocation {
    uint8_t* cpu=nullptr;
    D3D12_GPU_VIRTUAL_ADDRESS gpu=0;
    ID3D12Resource* resource=nullptr;
    uint64_t offset=0,capacity=0;
};
struct Context {
    ID3D12Device* device=nullptr; // Borrowed, must stay alive during the call.
    ID3D12GraphicsCommandList* list=nullptr; // Open DIRECT list on the one caller-owned queue.
    // Every recorded list must be submitted in order. After discarding a recorded list/device loss,
    // invalidate this cache via the caller's GPU-idle/recreation path before another draw uses it.
    const void* queue_identity=nullptr; // Stable actual queue identity; cross-queue cache use is rejected.
    // CPU points at resource's persistently mapped upload bytes at offset. gpu=resource VA+offset.
    // Allocation must not alias live/in-flight bytes and must reserve capacity through the actual frame fence.
    // It must NOT Submit, close/replace the borrowed list, switch queues or reset a cache during this call.
    std::function<UploadAllocation(uint64_t bytes,uint64_t alignment)> allocate_upload;
    // Called BEFORE any command references these objects. AddRef/retain every object until the actual
    // submitted frame fence completes, even on a later caller failure. False prevents command recording.
    std::function<bool(std::span<IUnknown* const>)> retain;
};
struct Result {
    Status status=Status::Disabled;
    HRESULT error=S_OK;
    bool draw_call_recorded=false,legacy_bindings_invalidated=false;
    // Recorded reports CPU API calls only. ResourceBarrier/Draw are void; root debug-layer diagnostics,
    // actual submission/fence and visual validation remain required. Device removal prevents Recorded.
    bool gpu_execution_observed=false;
    uint64_t resident_bytes=0;
};

// Render-thread-owned cache; no internal queue, fence, Submit, worker, file I/O or automatic reset.
// Dedicated shader-visible descriptor heaps append only: no slot wrap/reuse while commands may be live.
class Device {
public:
    Device();
    ~Device();
    Device(const Device&)=delete;
    Device& operator=(const Device&)=delete;
    Result RecordDraw(const Context&,const Input&,Limits={}) noexcept;
    // plan MUST be PlanDraw's result for this same, unmodified input and limits, made by the caller just before.
    // Planning once per draw instead of twice halves the dedicated path's CPU preflight.
    Result RecordDraw(const Context&,const Input&,const Plan& plan,Limits={}) noexcept;
    // Caller MUST have independently waited for GPU idle. Never call to bypass an in-flight lifetime.
    void ResetGpuIdle() noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace gpu::pbr::device
