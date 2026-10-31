#include "gpu/pbr_device.h"
#include "gpu/shader_cache.h"

#include <d3dcompiler.h>
#include <wrl/client.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace gpu::pbr::device {
namespace {
using Microsoft::WRL::ComPtr;
// A whole city's materials with HD albedos: a PC GPU budget, still bounded.
constexpr uint64_t kMaximumResident=2ull<<30,kMaximumUpload=256ull<<20;
uint64_t Align(uint64_t n,uint64_t a) {return (n+a-1)&~(a-1);}
bool Finite(float x){return std::isfinite(x);}
bool LimitsValid(const Limits& l){return l.resident_bytes&&l.resident_bytes<=kMaximumResident&&l.draw_upload_bytes&&l.draw_upload_bytes<=kMaximumUpload&&
    l.materials&&l.materials<=4096&&l.psos&&l.psos<=256&&l.samplers&&l.samplers<=256&&l.srv_descriptors>=5&&l.srv_descriptors<=40960;}
bool Color(MapRole r){return r==MapRole::Albedo||r==MapRole::Emission;}
bool PlanMap(const Map* m,MapRole role,MapPlan& result){
    result={};result.map=m;result.role=role;
    result.format=role==MapRole::Height?DXGI_FORMAT_R16_UNORM:Color(role)?DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:DXGI_FORMAT_R8G8B8A8_UNORM;
    const uint32_t stride=role==MapRole::Height?2:4;
    if(!m){result.width=result.height=result.levels=1;result.pixel_bytes=stride;result.upload_bytes=512;result.resident_estimate=65536;return true;}
    if(m->role!=role||m->color_space!=(Color(role)?ColorSpace::Srgb:ColorSpace::Linear)||
       m->storage!=(role==MapRole::Height?Storage::R16Unorm:Storage::Rgba8)||!controls::ValidDigest(m->sha256)||m->levels.empty()||m->levels.size()>15)return false;
    uint32_t width=m->levels[0].width,height=m->levels[0].height;
    if(!width||!height||width>16384||height>16384)return false;
    result.width=width;result.height=height;result.levels=uint32_t(m->levels.size());
    for(size_t i=0;i<m->levels.size();++i){const auto& level=m->levels[i];
        if(level.width!=width||level.height!=height||uint64_t(width)*height*stride!=level.pixels.size())return false;
        result.pixel_bytes+=level.pixels.size();result.upload_bytes=Align(result.upload_bytes,512)+Align(uint64_t(width)*stride,256)*height;
        if(width==1&&height==1){if(i+1!=m->levels.size())return false;}else if(i+1==m->levels.size())return false;
        width=std::max(1u,width/2);height=std::max(1u,height/2);
    }
    result.upload_bytes=Align(result.upload_bytes,512);result.resident_estimate=Align(result.pixel_bytes,65536);return true;
}
bool OutputValid(const Input& in){
    const auto f=in.original_opaque_state.RTVFormats[0];
    if(in.scene.info[1]==uint32_t(pipeline::OutputPolicy::ToneMappedSrgb))return f==DXGI_FORMAT_R8G8B8A8_UNORM||f==DXGI_FORMAT_B8G8R8A8_UNORM;
    return f==DXGI_FORMAT_R16G16B16A16_FLOAT||f==DXGI_FORMAT_R32G32B32A32_FLOAT;
}
bool OriginalStateValid(const D3D12_GRAPHICS_PIPELINE_STATE_DESC& d){
    return d.NumRenderTargets==1&&d.SampleDesc.Count&&d.SampleDesc.Count<=16&&d.PrimitiveTopologyType==D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE&&
        pipeline::KeepsLegacyBlend(d.BlendState)&&
        !d.GS.pShaderBytecode&&!d.GS.BytecodeLength&&!d.HS.pShaderBytecode&&!d.HS.BytecodeLength&&!d.DS.pShaderBytecode&&!d.DS.BytecodeLength&&
        !d.StreamOutput.NumEntries&&!d.StreamOutput.NumStrides&&!d.NodeMask&&d.IBStripCutValue==D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED;
}
D3D12_RESOURCE_DESC TextureDesc(const MapPlan& p){
    D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;d.Width=p.width;d.Height=p.height;d.DepthOrArraySize=1;
    d.MipLevels=UINT16(p.levels);d.Format=p.format;d.SampleDesc.Count=1;d.Layout=D3D12_TEXTURE_LAYOUT_UNKNOWN;return d;
}
std::vector<uint32_t> SamplerKey(const D3D12_SAMPLER_DESC& s){
    std::vector<uint32_t> k{uint32_t(s.Filter),uint32_t(s.AddressU),uint32_t(s.AddressV),uint32_t(s.AddressW),std::bit_cast<uint32_t>(s.MipLODBias),
        s.MaxAnisotropy,uint32_t(s.ComparisonFunc)};
    for(float f:s.BorderColor)k.push_back(std::bit_cast<uint32_t>(f));k.push_back(std::bit_cast<uint32_t>(s.MinLOD));k.push_back(std::bit_cast<uint32_t>(s.MaxLOD));return k;
}
std::vector<uint32_t> StateKey(const D3D12_GRAPHICS_PIPELINE_STATE_DESC& d,controls::HeightMode mode){
    std::vector<uint32_t> k{uint32_t(mode),d.SampleMask,uint32_t(d.RTVFormats[0]),uint32_t(d.DSVFormat),d.SampleDesc.Count,d.SampleDesc.Quality,d.NodeMask,uint32_t(d.Flags)};
    const auto& r=d.RasterizerState;
    for(uint32_t v:{uint32_t(r.FillMode),uint32_t(r.CullMode),uint32_t(r.FrontCounterClockwise),std::bit_cast<uint32_t>(r.DepthBias),
        std::bit_cast<uint32_t>(r.DepthBiasClamp),std::bit_cast<uint32_t>(r.SlopeScaledDepthBias),uint32_t(r.DepthClipEnable),uint32_t(r.MultisampleEnable),
        uint32_t(r.AntialiasedLineEnable),r.ForcedSampleCount,uint32_t(r.ConservativeRaster)})k.push_back(v);
    const auto& ds=d.DepthStencilState;
    for(uint32_t v:{uint32_t(ds.DepthEnable),uint32_t(ds.DepthWriteMask),uint32_t(ds.DepthFunc),uint32_t(ds.StencilEnable),uint32_t(ds.StencilReadMask),uint32_t(ds.StencilWriteMask)})k.push_back(v);
    for(auto f:{ds.FrontFace,ds.BackFace})for(uint32_t v:{uint32_t(f.StencilFailOp),uint32_t(f.StencilDepthFailOp),uint32_t(f.StencilPassOp),uint32_t(f.StencilFunc)})k.push_back(v);
    const auto& b=d.BlendState.RenderTarget[0];for(uint32_t v:{uint32_t(d.BlendState.IndependentBlendEnable),uint32_t(b.SrcBlend),uint32_t(b.DestBlend),uint32_t(b.BlendOp),
        uint32_t(b.SrcBlendAlpha),uint32_t(b.DestBlendAlpha),uint32_t(b.BlendOpAlpha),uint32_t(b.LogicOp),uint32_t(b.RenderTargetWriteMask)})k.push_back(v);
    return k;
}
struct Failure {Status status;HRESULT error=S_OK;};
void Require(bool b,Status s,HRESULT hr=S_OK){if(!b)throw Failure{s,hr};}
void Check(HRESULT hr,Status s=Status::DeviceFailure){if(FAILED(hr))throw Failure{s,hr};}
}

const char* StatusText(Status s){switch(s){
    case Status::Planned:return "CPU plan only";case Status::Recorded:return "CPU draw call recorded";case Status::Disabled:return "disabled";
    case Status::InvalidLibrary:return "invalid immutable library/key";case Status::MaterialRejected:return "reviewed material rejected";
    case Status::UnsupportedDraw:return "unsupported or unproved draw";case Status::InvalidMesh:return "invalid packed mesh";
    case Status::InvalidConstants:return "invalid constants/output policy";case Status::InvalidSampler:return "invalid material sampler";
    case Status::BudgetExceeded:return "bounded cache/upload/draw budget exceeded";case Status::InvalidContext:return "invalid caller queue/context";
    case Status::OwnershipChanged:return "device/queue/library ownership changed";case Status::DeviceFailure:return "D3D12 creation failed";
    case Status::ShaderFailure:return "dedicated shader compile failed";case Status::UploadFailure:return "caller upload allocation invalid";
    case Status::RetentionFailure:return "actual frame-fence retention failed";case Status::AllocationFailure:return "CPU allocation failed";
    }return "unknown";}
bool ValidSampler(const D3D12_SAMPLER_DESC& s){
    const auto address=[](D3D12_TEXTURE_ADDRESS_MODE a){return a>=D3D12_TEXTURE_ADDRESS_MODE_WRAP&&a<=D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE;};
    const uint32_t filter=uint32_t(s.Filter);
    const bool filter_ok=filter==D3D12_FILTER_MIN_MAG_MIP_POINT||filter==D3D12_FILTER_MIN_MAG_POINT_MIP_LINEAR||
        filter==D3D12_FILTER_MIN_POINT_MAG_LINEAR_MIP_POINT||filter==D3D12_FILTER_MIN_POINT_MAG_MIP_LINEAR||
        filter==D3D12_FILTER_MIN_LINEAR_MAG_MIP_POINT||filter==D3D12_FILTER_MIN_LINEAR_MAG_POINT_MIP_LINEAR||
        filter==D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT||filter==D3D12_FILTER_MIN_MAG_MIP_LINEAR||filter==D3D12_FILTER_ANISOTROPIC;
    if(!filter_ok||!address(s.AddressU)||!address(s.AddressV)||!address(s.AddressW)||!Finite(s.MipLODBias)||s.MipLODBias<-16||s.MipLODBias>15.99f||
       !s.MaxAnisotropy||s.MaxAnisotropy>16||s.ComparisonFunc<D3D12_COMPARISON_FUNC_NEVER||s.ComparisonFunc>D3D12_COMPARISON_FUNC_ALWAYS||
       !Finite(s.MinLOD)||!Finite(s.MaxLOD)||s.MinLOD>s.MaxLOD)return false;
    for(float f:s.BorderColor)if(!Finite(f)||f<0||f>1)return false;return true;
}
Status PlanDraw(const Input& in,const Limits& limits,Plan& result){
    result={};if(!in.evidence.enabled)return Status::Disabled;
    if(!LimitsValid(limits))return Status::BudgetExceeded;
    if(!in.library||in.library->status!=LoadStatus::Ready||!in.library->errors.empty()||in.library->materials.size()>8192||!controls::ValidDigest(in.library->manifest_sha256)||
       !controls::DigestEqual(in.library->manifest_sha256,in.key.approved_manifest_sha256)||!controls::ValidAssignment(in.key.assignment))return Status::InvalidLibrary;
    if(!controls::AssignmentEqual(in.surface.assignment,in.key.assignment)||in.evidence.texture!=in.key.assignment.original_texture||
       in.evidence.vertex_shader!=in.key.assignment.vertex_shader||in.evidence.pixel_shader!=in.key.assignment.pixel_shader)return Status::MaterialRejected;
    const Material* unique=nullptr;bool duplicate=false;
    ForTexture(*in.library,in.key.assignment.original_texture,[&](uint32_t row){const auto& m=in.library->materials[row];
        if(controls::AssignmentEqual(m.assignment,in.key.assignment)){duplicate|=unique!=nullptr;unique=&m;}});
    if(duplicate||!unique||(!unique->game_albedo&&unique->albedo.levels.empty())||!controls::ValidDigest(in.surface.current_alpha_sha256))
        return Status::MaterialRejected;
    const auto selected=Select(*in.library,in.surface);if(selected.reason!=SelectionReason::Ready||!selected.material)return Status::MaterialRejected;
    const auto mode=controls::HeightMode(in.material.options[3]);
    if(pipeline::Eligible(in.evidence,mode)!=pipeline::Status::Ready||!OriginalStateValid(in.original_opaque_state))return Status::UnsupportedDraw;
    if(pipeline::ValidateConstants(in.geometry,in.material,in.scene)!=pipeline::Status::Ready||!OutputValid(in)||!in.coverage_policy_proven||
       !Finite(in.actual_alpha_cutoff)||in.actual_alpha_cutoff<0||in.actual_alpha_cutoff>1||in.material.overrides[2]!=in.actual_alpha_cutoff)return Status::InvalidConstants;
    if(pipeline::ValidateMesh(in.vertices,in.indices,pipeline::NormalSource(in.geometry.options[1]))!=pipeline::Status::Ready)return Status::InvalidMesh;
    if(!ValidSampler(in.actual_sampler))return Status::InvalidSampler;
    const auto& m=*selected.material;
    if(!controls::ValidDigest(m.review_sha256)||(!m.game_albedo&&!controls::ValidDigest(m.baseline_alpha_sha256))||
       in.geometry.options[2]!=(m.game_albedo?1u:0u)||(in.geometry.options[3]!=0)!=(in.game_layer!=nullptr)||
       in.material.maps[2]!=(m.has_height?1u:0u)||in.material.maps[3]!=(m.has_emission?1u:0u)||in.material.coverage[0]!=((m.assignment.cutout||in.draw_alpha_test)?1u:0u)||
       in.material.options[0]!=(m.normal_convention==NormalConvention::DirectX?1u:0u))return Status::MaterialRejected;
    Plan plan;plan.owner=in.library;plan.material=&m;plan.height_mode=mode;
    // A game albedo still gets its own neutral placeholder resource, so the material's own maps keep their slots.
    const Map* maps[]={m.game_albedo?nullptr:&m.albedo,&m.normal,&m.orm,m.has_height?&m.height:nullptr,m.has_emission?&m.emission:nullptr};
    const MapRole roles[]={MapRole::Albedo,MapRole::Normal,MapRole::Orm,MapRole::Height,MapRole::Emission};
    for(size_t i=0;i<5;++i){if(!PlanMap(maps[i],roles[i],plan.maps[i]))return Status::MaterialRejected;
        plan.texture_upload_bytes+=plan.maps[i].upload_bytes;plan.resident_estimate+=plan.maps[i].resident_estimate;}
    plan.geometry_upload_bytes=Align(in.vertices.size_bytes(),256)+Align(in.indices.size_bytes(),256)+
        Align(sizeof(in.geometry),256)+Align(sizeof(in.material),256)+Align(sizeof(in.scene),256);
    plan.triangle_count=uint32_t((in.indices.empty()?in.vertices.size():in.indices.size())/3);
    if(plan.geometry_upload_bytes>limits.draw_upload_bytes||plan.texture_upload_bytes>limits.draw_upload_bytes-plan.geometry_upload_bytes||
       plan.resident_estimate>limits.resident_bytes||
       (mode==controls::HeightMode::Displacement&&pipeline::ValidateTessellationBudget(plan.triangle_count,uint32_t(in.material.height[3]))!=pipeline::Status::Ready))
        return Status::BudgetExceeded;
    result=std::move(plan);return Status::Planned;
}

struct Device::Impl {
    ID3D12Device* device=nullptr;const void* queue=nullptr;Limits limits;
    bool poisoned=false;
    std::shared_ptr<const Library> owner;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12DescriptorHeap> srv_heap,sampler_heap;
    std::array<ComPtr<ID3DBlob>,5> shaders; // VS, patch VS, PS, HS, DS
    uint32_t srv_increment=0,sampler_increment=0,srv_next=0;
    uint64_t resident=0;
    struct Texture {ComPtr<ID3D12Resource> resource;MapPlan plan;uint64_t allocation_bytes=0;};
    // A game albedo table is the material's own maps with the draw's game texture in slot 0, appended again whenever
    // that texture resource changes (append-only heap: never rewrite a table commands may still use).
    struct MaterialEntry {controls::ExactMaterialKey key;std::array<Texture,5> textures;uint32_t srv_start=0;bool uploaded=false;
        // every (game albedo, window layer) pair this material has drawn with: facades sharing a base texture take
        // several window textures, and alternating between them must not append a table each time
        struct GameTable {ComPtr<ID3D12Resource> game,layer;uint32_t srv_start=0;};
        std::vector<GameTable> game_tables;};
    struct SamplerEntry {std::vector<uint32_t> key;uint32_t slot=0;};
    struct PsoEntry {std::vector<uint32_t> key;ComPtr<ID3D12PipelineState> pso;};
    std::vector<MaterialEntry> materials;
    std::unordered_map<uint64_t,std::vector<uint32_t>> material_rows;  // original_texture -> materials indices
    std::vector<SamplerEntry> samplers;
    std::vector<PsoEntry> psos;

    void Initialize(const Context& context,const Plan& plan,const Limits& requested){
        device=context.device;queue=context.queue_identity;owner=plan.owner;limits=requested;
        materials.reserve(limits.materials);samplers.reserve(limits.samplers);psos.reserve(limits.psos);
        const std::string source[]={pipeline::VertexHlsl(),pipeline::VertexHlsl(true),pipeline::PixelHlsl(),pipeline::HullHlsl(),pipeline::DomainHlsl()};
        const char* targets[]={"vs_5_0","vs_5_0","ps_5_0","hs_5_0","ds_5_0"};
        // the disk cache keeps these five out of the first PBR frame after the first run
        for(unsigned i=0;i<5;++i){ComPtr<ID3DBlob> errors;Check(shader_cache::Compile(source[i],"main",targets[i],
            D3DCOMPILE_ENABLE_STRICTNESS|D3DCOMPILE_WARNINGS_ARE_ERRORS|D3DCOMPILE_OPTIMIZATION_LEVEL3,shaders[i],&errors),Status::ShaderFailure);}
        pipeline::RootLayout layout;auto desc=layout.Describe();ComPtr<ID3DBlob> bytes,errors;
        Check(D3D12SerializeRootSignature(&desc,D3D_ROOT_SIGNATURE_VERSION_1,&bytes,&errors));
        Check(device->CreateRootSignature(0,bytes->GetBufferPointer(),bytes->GetBufferSize(),IID_PPV_ARGS(&root)));
        D3D12_DESCRIPTOR_HEAP_DESC heap{};heap.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;heap.NumDescriptors=limits.srv_descriptors;heap.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        Check(device->CreateDescriptorHeap(&heap,IID_PPV_ARGS(&srv_heap)));heap.Type=D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;heap.NumDescriptors=limits.samplers;
        Check(device->CreateDescriptorHeap(&heap,IID_PPV_ARGS(&sampler_heap)));
        srv_increment=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        sampler_increment=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
        Require(srv_increment&&sampler_increment,Status::DeviceFailure);
    }
    pipeline::ShaderSet ShaderSet(controls::HeightMode mode){
        auto code=[&](unsigned i){return D3D12_SHADER_BYTECODE{shaders[i]->GetBufferPointer(),shaders[i]->GetBufferSize()};};
        if(mode==controls::HeightMode::Displacement)return {code(1),code(2),code(3),code(4)};return {code(0),code(2),{}, {}};
    }
    ID3D12PipelineState* Pso(const Input& in,controls::HeightMode mode){
        auto key=StateKey(in.original_opaque_state,mode);for(auto& p:psos)if(p.key==key)return p.pso.Get();
        Require(psos.size()<limits.psos,Status::BudgetExceeded);D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
        Require(pipeline::DescribePipeline(in.original_opaque_state,root.Get(),ShaderSet(mode),mode,desc)==pipeline::Status::Ready,Status::UnsupportedDraw);
        PsoEntry p;p.key=std::move(key);Check(device->CreateGraphicsPipelineState(&desc,IID_PPV_ARGS(&p.pso)));
        psos.push_back(std::move(p));return psos.back().pso.Get();
    }
    uint32_t Sampler(const D3D12_SAMPLER_DESC& desc){
        auto key=SamplerKey(desc);for(const auto& s:samplers)if(s.key==key)return s.slot;
        Require(samplers.size()<limits.samplers,Status::BudgetExceeded);uint32_t slot=uint32_t(samplers.size());
        auto cpu=sampler_heap->GetCPUDescriptorHandleForHeapStart();cpu.ptr+=SIZE_T(slot)*sampler_increment;
        // Append only. A previously submitted descriptor is never overwritten, even for an equal material.
        device->CreateSampler(&desc,cpu);samplers.push_back({std::move(key),slot});return slot;
    }
    void WriteSrv(uint32_t slot,ID3D12Resource* resource,DXGI_FORMAT format,uint32_t levels){
        auto cpu=srv_heap->GetCPUDescriptorHandleForHeapStart();cpu.ptr+=SIZE_T(slot)*srv_increment;
        D3D12_SHADER_RESOURCE_VIEW_DESC view{};view.Format=format;view.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
        view.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;view.Texture2D.MipLevels=levels;
        device->CreateShaderResourceView(resource,&view,cpu);
    }
    // The table for this draw: the material's own, or one with the draw's actual game textures (albedo in slot 0, a
    // window layer in slot 4), appended again whenever either resource changes.
    uint32_t Table(MaterialEntry& entry,ID3D12Resource* game,ID3D12Resource* layer){
        if(!game&&!layer)return entry.srv_start;
        for(const auto& t:entry.game_tables)if(t.game.Get()==game&&t.layer.Get()==layer)return t.srv_start;
        const auto usable=[](ID3D12Resource* r){const auto d=r->GetDesc();
            return d.Dimension==D3D12_RESOURCE_DIMENSION_TEXTURE2D&&d.DepthOrArraySize==1&&d.MipLevels&&d.Format!=DXGI_FORMAT_UNKNOWN;};
        Require((!game||usable(game))&&(!layer||usable(layer))&&srv_next<=limits.srv_descriptors-5,Status::BudgetExceeded);
        for(unsigned i=0;i<5;++i) {
            ID3D12Resource* own=i==0&&game?game:i==4&&layer?layer:nullptr;
            if(own){const auto d=own->GetDesc();WriteSrv(srv_next+i,own,d.Format,d.MipLevels);}
            else WriteSrv(srv_next+i,entry.textures[i].resource.Get(),entry.textures[i].plan.format,entry.textures[i].plan.levels);
        }
        entry.game_tables.push_back({game,layer,srv_next});srv_next+=5;return entry.game_tables.back().srv_start;
    }
    MaterialEntry& MaterialResources(const Input& in,const Plan& plan){
        if(const auto it=material_rows.find(in.key.assignment.original_texture);it!=material_rows.end())
            for(uint32_t row:it->second)if(controls::KeyEqual(materials[row].key,in.key))return materials[row];
        Require(materials.size()<limits.materials&&srv_next<=limits.srv_descriptors-5,Status::BudgetExceeded);
        MaterialEntry entry;entry.key=in.key;entry.srv_start=srv_next;
        uint64_t allocation=0;
        for(unsigned i=0;i<5;++i){auto& t=entry.textures[i];t.plan=plan.maps[i];auto d=TextureDesc(t.plan);
            auto info=device->GetResourceAllocationInfo(0,1,&d);Require(info.SizeInBytes&&info.SizeInBytes!=UINT64_MAX&&info.SizeInBytes<=limits.resident_bytes-allocation,Status::BudgetExceeded);
            allocation+=info.SizeInBytes;t.allocation_bytes=info.SizeInBytes;}
        Require(resident<=limits.resident_bytes-allocation,Status::BudgetExceeded);
        D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;
        for(auto& t:entry.textures){auto d=TextureDesc(t.plan);Check(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&t.resource)));}
        // A failed later operation never recycles descriptors; publication happens only after all resources exist.
        for(unsigned i=0;i<5;++i){auto cpu=srv_heap->GetCPUDescriptorHandleForHeapStart();cpu.ptr+=SIZE_T(srv_next+i)*srv_increment;
            D3D12_SHADER_RESOURCE_VIEW_DESC view{};view.Format=entry.textures[i].plan.format;view.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
            view.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;view.Texture2D.MipLevels=entry.textures[i].plan.levels;
            device->CreateShaderResourceView(entry.textures[i].resource.Get(),&view,cpu);}
        srv_next+=5;resident+=allocation;
        material_rows[in.key.assignment.original_texture].push_back(uint32_t(materials.size()));
        materials.push_back(std::move(entry));return materials.back();
    }
};

namespace {
UploadAllocation Allocate(const Context& c,uint64_t bytes,uint64_t alignment){
    Require(bytes&&bytes<=kMaximumUpload,Status::UploadFailure);auto a=c.allocate_upload(bytes,alignment);
    Require(a.cpu&&a.resource&&a.gpu&&a.capacity>=bytes&&a.offset<=UINT64_MAX-a.capacity&&a.gpu%alignment==0&&a.offset%alignment==0,Status::UploadFailure);
    auto d=a.resource->GetDesc();Require(d.Dimension==D3D12_RESOURCE_DIMENSION_BUFFER&&a.offset<=d.Width&&a.capacity<=d.Width-a.offset,Status::UploadFailure);
    const auto base=a.resource->GetGPUVirtualAddress();Require(base&&base<=UINT64_MAX-a.offset&&base+a.offset==a.gpu,Status::UploadFailure);
    D3D12_HEAP_PROPERTIES heap{};D3D12_HEAP_FLAGS flags{};Check(a.resource->GetHeapProperties(&heap,&flags),Status::UploadFailure);
    Require(heap.Type==D3D12_HEAP_TYPE_UPLOAD,Status::UploadFailure);return a;
}
struct StagedTexture {
    ID3D12Resource* texture=nullptr;UploadAllocation upload;
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints;
};
StagedTexture StageTexture(const Context& context,const MapPlan& p,ID3D12Resource* texture){
    StagedTexture staged;staged.texture=texture;staged.footprints.resize(p.levels);
    std::vector<UINT> rows(p.levels);std::vector<UINT64> row_bytes(p.levels);UINT64 bytes=0;auto d=TextureDesc(p);
    context.device->GetCopyableFootprints(&d,0,p.levels,0,staged.footprints.data(),rows.data(),row_bytes.data(),&bytes);
    Require(bytes&&bytes<=p.upload_bytes,Status::UploadFailure);
    staged.upload=Allocate(context,p.upload_bytes,D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
    std::memset(staged.upload.cpu,0,size_t(p.upload_bytes));
    const std::array<uint8_t,4> neutral_height{0,128,0,0},neutral_emission{0,0,0,255};
    for(uint32_t i=0;i<p.levels;++i){auto& f=staged.footprints[i];uint32_t stride=p.role==MapRole::Height?2:4;
        uint32_t width=p.map?p.map->levels[i].width:1,height=p.map?p.map->levels[i].height:1;
        Require(f.Footprint.Format==p.format&&f.Footprint.Width==width&&f.Footprint.Height==height&&f.Footprint.Depth==1&&
            rows[i]==height&&row_bytes[i]==uint64_t(width)*stride&&f.Footprint.RowPitch>=row_bytes[i]&&
            f.Offset<=p.upload_bytes&&uint64_t(f.Footprint.RowPitch)*height<=p.upload_bytes-f.Offset,Status::UploadFailure);
        const uint8_t* source=p.map?p.map->levels[i].pixels.data():p.role==MapRole::Height?neutral_height.data():neutral_emission.data();
        for(uint32_t row=0;row<height;++row)std::memcpy(staged.upload.cpu+f.Offset+uint64_t(row)*f.Footprint.RowPitch,source+uint64_t(row)*row_bytes[i],size_t(row_bytes[i]));
        f.Offset+=staged.upload.offset;
    }return staged;
}
}

Device::Device()=default;
Device::~Device()=default;
void Device::ResetGpuIdle() noexcept {impl_.reset();}
Result Device::RecordDraw(const Context& context,const Input& input,Limits limits) noexcept {
    Plan plan;Result result;
    try{result.status=PlanDraw(input,limits,plan);}catch(...){result.status=Status::AllocationFailure;}
    if(result.status!=Status::Planned)return result;
    return RecordDraw(context,input,plan,limits);
}
Result Device::RecordDraw(const Context& context,const Input& input,const Plan& plan,Limits limits) noexcept {
    Result result;
    bool commands_started=false;
    try{
        // The plan's own identity must still match this input; its full checks ran in the caller's PlanDraw.
        Require(input.evidence.enabled&&plan.owner&&plan.owner.get()==input.library.get()&&plan.material&&
                controls::AssignmentEqual(plan.material->assignment,input.key.assignment),Status::MaterialRejected);
        result.status=Status::Planned;
        Require(context.device&&context.list&&context.queue_identity&&context.allocate_upload&&context.retain,Status::InvalidContext);
        Require(context.list->GetType()==D3D12_COMMAND_LIST_TYPE_DIRECT,Status::InvalidContext);
        Check(context.device->GetDeviceRemovedReason());
        ComPtr<ID3D12Device> list_device;Check(context.list->GetDevice(IID_PPV_ARGS(&list_device)),Status::InvalidContext);
        Require(list_device.Get()==context.device,Status::InvalidContext);
        if(!impl_){auto created=std::make_unique<Impl>();created->Initialize(context,plan,limits);impl_=std::move(created);}
        auto& c=*impl_;result.resident_bytes=c.resident;
        Require(!c.poisoned,Status::DeviceFailure);
        Require(c.device==context.device&&c.queue==context.queue_identity&&c.owner.get()==plan.owner.get()&&
            c.owner->manifest_sha256==plan.owner->manifest_sha256,Status::OwnershipChanged);
        Require(limits==c.limits,Status::OwnershipChanged);
        Require(plan.material->game_albedo==(input.game_albedo!=nullptr),Status::MaterialRejected);
        Require((input.geometry.options[3]!=0)==(input.game_layer!=nullptr),Status::MaterialRejected);
        auto pso=c.Pso(input,plan.height_mode);uint32_t sampler=c.Sampler(input.actual_sampler);auto& material=c.MaterialResources(input,plan);
        const uint32_t table=c.Table(material,input.game_albedo,input.game_layer);
        result.resident_bytes=c.resident;
        std::vector<StagedTexture> textures;uint64_t upload_bytes=plan.geometry_upload_bytes;
        if(!material.uploaded){textures.reserve(5);for(auto& t:material.textures){textures.push_back(StageTexture(context,t.plan,t.resource.Get()));upload_bytes+=t.plan.upload_bytes;}}
        Require(upload_bytes<=c.limits.draw_upload_bytes,Status::BudgetExceeded);
        auto vertices=Allocate(context,Align(input.vertices.size_bytes(),256),16);
        std::memcpy(vertices.cpu,input.vertices.data(),input.vertices.size_bytes());
        UploadAllocation indices;if(!input.indices.empty()){indices=Allocate(context,Align(input.indices.size_bytes(),256),4);std::memcpy(indices.cpu,input.indices.data(),input.indices.size_bytes());}
        auto geometry=Allocate(context,Align(sizeof(input.geometry),256),256);std::memcpy(geometry.cpu,&input.geometry,sizeof(input.geometry));
        auto factors=Allocate(context,Align(sizeof(input.material),256),256);std::memcpy(factors.cpu,&input.material,sizeof(input.material));
        auto scene=Allocate(context,Align(sizeof(input.scene),256),256);std::memcpy(scene.cpu,&input.scene,sizeof(input.scene));
        std::vector<IUnknown*> retained{c.root.Get(),pso,c.srv_heap.Get(),c.sampler_heap.Get(),vertices.resource,geometry.resource,factors.resource,scene.resource};
        if(input.game_albedo)retained.push_back(input.game_albedo);
        if(input.game_layer)retained.push_back(input.game_layer);
        if(indices.resource)retained.push_back(indices.resource);for(auto& t:material.textures)retained.push_back(t.resource.Get());for(auto& t:textures)retained.push_back(t.upload.resource);
        Require(context.retain(retained),Status::RetentionFailure);
        // No fallible allocations, callbacks or object creation occur after the first command is recorded.
        // All referenced objects are retained already, even if later device-removal diagnostics fail.
        Check(context.device->GetDeviceRemovedReason());
        commands_started=true;
        for(auto& t:textures){for(uint32_t i=0;i<t.footprints.size();++i){D3D12_TEXTURE_COPY_LOCATION src{},dst{};
            src.pResource=t.upload.resource;src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;src.PlacedFootprint=t.footprints[i];
            dst.pResource=t.texture;dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;dst.SubresourceIndex=i;context.list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);}
            D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;barrier.Transition.pResource=t.texture;
            barrier.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;barrier.Transition.StateBefore=D3D12_RESOURCE_STATE_COPY_DEST;
            barrier.Transition.StateAfter=D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE|D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;context.list->ResourceBarrier(1,&barrier);}
        ID3D12DescriptorHeap* heaps[]={c.srv_heap.Get(),c.sampler_heap.Get()};result.legacy_bindings_invalidated=true;context.list->SetDescriptorHeaps(2,heaps);
        context.list->SetGraphicsRootSignature(c.root.Get());context.list->SetPipelineState(pso);
        context.list->SetGraphicsRootConstantBufferView(0,geometry.gpu);context.list->SetGraphicsRootConstantBufferView(1,factors.gpu);context.list->SetGraphicsRootConstantBufferView(2,scene.gpu);
        auto srv=c.srv_heap->GetGPUDescriptorHandleForHeapStart();srv.ptr+=uint64_t(table)*c.srv_increment;
        auto samp=c.sampler_heap->GetGPUDescriptorHandleForHeapStart();samp.ptr+=uint64_t(sampler)*c.sampler_increment;
        context.list->SetGraphicsRootDescriptorTable(3,srv);context.list->SetGraphicsRootDescriptorTable(4,samp);
        D3D12_VERTEX_BUFFER_VIEW vb{vertices.gpu,UINT(input.vertices.size_bytes()),sizeof(pipeline::Vertex)};context.list->IASetVertexBuffers(0,1,&vb);
        context.list->IASetPrimitiveTopology(pipeline::DrawTopology(plan.height_mode));
        if(!input.indices.empty()){D3D12_INDEX_BUFFER_VIEW ib{indices.gpu,UINT(input.indices.size_bytes()),DXGI_FORMAT_R32_UINT};context.list->IASetIndexBuffer(&ib);context.list->DrawIndexedInstanced(UINT(input.indices.size()),1,0,0,0);}
        else context.list->DrawInstanced(UINT(input.vertices.size()),1,0,0);
        result.draw_call_recorded=true;
        Check(context.device->GetDeviceRemovedReason());
        material.uploaded=true;result.status=Status::Recorded;return result;
    }catch(const Failure& f){if(commands_started&&impl_)impl_->poisoned=true;result.status=f.status;result.error=f.error;return result;}
     catch(...){if(commands_started&&impl_)impl_->poisoned=true;result.status=Status::AllocationFailure;return result;}
}

} // namespace gpu::pbr::device
