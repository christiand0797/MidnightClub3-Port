#include "gpu/pbr_device.h"
#include <d3dcompiler.h>
#include <d3d11shader.h>
#include <wrl/client.h>

#include <cassert>
#include <iostream>
#include <limits>
#include <vector>

using namespace gpu::pbr;
using namespace gpu::pbr::device;
namespace {
using Microsoft::WRL::ComPtr;
Map MakeMap(MapRole role,std::initializer_list<uint8_t> bytes){
    Map m;m.role=role;m.sha256=std::string(64,'c');m.color_space=role==MapRole::Albedo||role==MapRole::Emission?ColorSpace::Srgb:ColorSpace::Linear;
    m.storage=role==MapRole::Height?Storage::R16Unorm:Storage::Rgba8;m.levels.push_back({1,1,bytes});return m;
}
struct Fixture {
    std::shared_ptr<Library> library=std::make_shared<Library>();Input in;
    std::array<pipeline::Vertex,3> vertices{{{{0,0,2},{0,0,-1},{0,0}},{{1,0,2},{0,0,-1},{1,0}},{{0,1,2},{0,0,-1},{0,1}}}};
    Fixture(){
        library->status=LoadStatus::Ready;library->manifest_sha256=std::string(64,'a');
        Material m;m.assignment={1,2,3,0,0,SurfaceClass::World,false};m.review_sha256=std::string(64,'b');
        m.albedo=MakeMap(MapRole::Albedo,{100,150,200,255});m.normal=MakeMap(MapRole::Normal,{128,128,255,255});m.orm=MakeMap(MapRole::Orm,{255,128,0,255});
        m.baseline_alpha_sha256=AlphaDigest(m.albedo.levels[0]);library->materials.push_back(std::move(m));
        in.key=controls::ExactKey(library->materials[0].assignment,library->manifest_sha256);
        in.surface.assignment=in.key.assignment;in.surface.color_width=in.surface.color_height=1;in.surface.current_alpha_sha256=library->materials[0].baseline_alpha_sha256;
        in.surface.texture_2d=in.surface.geometric_position=in.surface.geometric_normal=in.surface.tangent_basis=in.surface.linear_lighting=true;
        auto& e=in.evidence;e.enabled=e.approved_assignment=true;e.texture=e.expected_texture=1;e.vertex_shader=e.expected_vertex_shader=2;e.pixel_shader=e.expected_pixel_shader=3;
        e.fixed_transform=e.triangle_list=e.opaque=e.packed_mesh_validated=e.original_normal_uv_proven=e.affine_bridge_proven=e.view_projection_proven=true;
        e.linear_lights_proven=e.output_policy_proven=e.all_five_bindings=true;
        geometry::AffineBridge affine;assert(geometry::BuildAffineBridge(geometry::RowMatrix4::Identity(),geometry::RowMatrix4::Identity(),affine)==geometry::Status::Ready);
        assert(pipeline::BuildGeometry(affine,geometry::RowMatrix4::Identity(),geometry::RowMatrix4::Identity(),1920,1080,640,480,
            pipeline::TessellationPolicy::UniformSharedEdges,16,in.geometry)==pipeline::Status::Ready);
        assert(pipeline::BuildMaterial({},NormalConvention::OpenGl,false,false,false,.5f,in.material)==pipeline::Status::Ready);
        assert(pipeline::BuildScene({}, {1,1,1},1,pipeline::OutputPolicy::ToneMappedSrgb,in.scene)==pipeline::Status::Ready);
        auto& d=in.original_opaque_state;d.NumRenderTargets=1;d.RTVFormats[0]=DXGI_FORMAT_B8G8R8A8_UNORM;d.SampleDesc.Count=1;d.SampleMask=UINT_MAX;
        d.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;d.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;d.RasterizerState.CullMode=D3D12_CULL_MODE_BACK;d.RasterizerState.DepthClipEnable=TRUE;
        d.DepthStencilState.DepthEnable=TRUE;d.DepthStencilState.DepthWriteMask=D3D12_DEPTH_WRITE_MASK_ALL;d.DepthStencilState.DepthFunc=D3D12_COMPARISON_FUNC_LESS_EQUAL;
        auto& b=d.BlendState.RenderTarget[0];b.SrcBlend=b.SrcBlendAlpha=D3D12_BLEND_ONE;b.DestBlend=b.DestBlendAlpha=D3D12_BLEND_ZERO;b.BlendOp=b.BlendOpAlpha=D3D12_BLEND_OP_ADD;b.RenderTargetWriteMask=15;
        auto& s=in.actual_sampler;s.Filter=D3D12_FILTER_MIN_MAG_MIP_LINEAR;s.AddressU=s.AddressV=s.AddressW=D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        s.MaxAnisotropy=1;s.ComparisonFunc=D3D12_COMPARISON_FUNC_ALWAYS;s.MaxLOD=D3D12_FLOAT32_MAX;
        in.coverage_policy_proven=true;in.actual_alpha_cutoff=.5f;
    }
    Status Get(Plan& p,Limits limits={}){in.library=library;in.vertices=vertices;return PlanDraw(in,limits,p);}
};
void DisabledAndIdentity(){
    Device device;Context context;unsigned callbacks=0;
    context.allocate_upload=[&](uint64_t,uint64_t){++callbacks;return UploadAllocation{};};
    context.retain=[&](std::span<IUnknown* const>){++callbacks;return false;};
    Input off;auto r=device.RecordDraw(context,off);assert(r.status==Status::Disabled&&!r.draw_call_recorded&&!r.legacy_bindings_invalidated&&!r.gpu_execution_observed&&callbacks==0);
    Fixture f;Plan p;assert(f.Get(p)==Status::Planned);
    r=device.RecordDraw(context,f.in);assert(r.status==Status::InvalidContext&&!r.draw_call_recorded&&!r.legacy_bindings_invalidated&&callbacks==0);
    device.ResetGpuIdle(); // Empty CPU-only cache; no hardware was ever created.
    f.in.key.approved_manifest_sha256=std::string(64,'d');assert(f.Get(p)==Status::InvalidLibrary);
    f=Fixture{};f.library->status=LoadStatus::Rejected;assert(f.Get(p)==Status::InvalidLibrary);
    f=Fixture{};f.library->errors.push_back("rejected");assert(f.Get(p)==Status::InvalidLibrary);
    f=Fixture{};f.in.key.assignment.uv_set=1;assert(f.Get(p)==Status::MaterialRejected);
    f=Fixture{};f.in.evidence.pixel_shader=9;assert(f.Get(p)==Status::MaterialRejected);
    f=Fixture{};f.in.surface.blended=true;assert(f.Get(p)==Status::MaterialRejected);
    f=Fixture{};f.in.surface.current_alpha_sha256=std::string(64,'d');assert(f.Get(p)==Status::MaterialRejected);
    f=Fixture{};f.library->materials.push_back(f.library->materials[0]);assert(f.Get(p)==Status::MaterialRejected);
    f=Fixture{};f.library->materials[0].albedo.levels.clear();assert(f.Get(p)==Status::MaterialRejected); // Do not invoke legacy Select::front on malformed data.
    f=Fixture{};f.library->materials[0].review_sha256="";assert(f.Get(p)==Status::MaterialRejected);
}
void FormatsAndOwnership(){
    Fixture f;Plan p;assert(f.Get(p)==Status::Planned);assert(p.owner.get()==f.library.get()&&p.material==&f.library->materials[0]);
    assert(p.maps[0].format==DXGI_FORMAT_R8G8B8A8_UNORM_SRGB&&p.maps[1].format==DXGI_FORMAT_R8G8B8A8_UNORM&&p.maps[2].format==DXGI_FORMAT_R8G8B8A8_UNORM);
    assert(p.maps[3].format==DXGI_FORMAT_R16_UNORM&&!p.maps[3].map&&p.maps[3].pixel_bytes==2);
    assert(p.maps[4].format==DXGI_FORMAT_R8G8B8A8_UNORM_SRGB&&!p.maps[4].map);
    assert(p.maps[3].upload_bytes==512&&p.resident_estimate==5*65536&&p.triangle_count==1);
    auto keep=p.owner;f.library.reset();assert(p.material->assignment.original_texture==1&&keep.use_count()>1); // Plan retains the exact selected owner.
    f=Fixture{};f.library->materials[0].normal.color_space=ColorSpace::Srgb;assert(f.Get(p)==Status::MaterialRejected);
    f=Fixture{};f.library->materials[0].orm.storage=Storage::R16Unorm;assert(f.Get(p)==Status::MaterialRejected);
    f=Fixture{};f.library->materials[0].albedo.levels[0].pixels.pop_back();assert(f.Get(p)==Status::MaterialRejected);
    f=Fixture{};f.library->materials[0].normal.levels.push_back(f.library->materials[0].normal.levels[0]);assert(f.Get(p)==Status::MaterialRejected);
    f=Fixture{};f.library->materials[0].normal.levels[0].width=2;assert(f.Get(p)==Status::MaterialRejected);
    f=Fixture{};f.in.material.maps[3]=1;assert(f.Get(p)==Status::MaterialRejected);
    f=Fixture{};f.in.material.options[0]=1;assert(f.Get(p)==Status::MaterialRejected);
    f=Fixture{};f.library->materials[0].normal.sha256="not pinned";assert(f.Get(p)==Status::MaterialRejected);
}
void BudgetsAndOutput(){
    Fixture f;Plan p;Limits l;assert(f.Get(p,l)==Status::Planned);
    const auto upload=p.geometry_upload_bytes+p.texture_upload_bytes;
    l.draw_upload_bytes=upload-1;assert(f.Get(p,l)==Status::BudgetExceeded);l.draw_upload_bytes=upload;assert(f.Get(p,l)==Status::Planned);
    l={};l.resident_bytes=5*65536-1;assert(f.Get(p,l)==Status::BudgetExceeded);l.resident_bytes=5*65536;assert(f.Get(p,l)==Status::Planned);
    l={};l.resident_bytes=(2ull<<30)+1;assert(f.Get(p,l)==Status::BudgetExceeded);
    l={};l.samplers=257;assert(f.Get(p,l)==Status::BudgetExceeded);l={};l.srv_descriptors=4;assert(f.Get(p,l)==Status::BudgetExceeded);
    f.in.original_opaque_state.RTVFormats[0]=DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;assert(f.Get(p)==Status::InvalidConstants); // No double sRGB encoding.
    f=Fixture{};f.in.scene.info[1]=uint32_t(pipeline::OutputPolicy::LinearHdr);assert(f.Get(p)==Status::InvalidConstants);
    f.in.original_opaque_state.RTVFormats[0]=DXGI_FORMAT_R16G16B16A16_FLOAT;assert(f.Get(p)==Status::Planned);
    f=Fixture{};f.in.coverage_policy_proven=false;assert(f.Get(p)==Status::InvalidConstants);
    f=Fixture{};f.in.actual_alpha_cutoff=.1f;assert(f.Get(p)==Status::InvalidConstants);
    // Kept compositing: ONE/ZERO is opaque, SRC_ALPHA/INV_SRC_ALPHA draws cutouts over what is behind; others stay legacy.
    {auto& b=f.in.original_opaque_state.BlendState.RenderTarget[0];
     f=Fixture{};b.BlendEnable=TRUE;b.BlendOp=D3D12_BLEND_OP_ADD;b.SrcBlend=D3D12_BLEND_ONE;b.DestBlend=D3D12_BLEND_ZERO;assert(f.Get(p)==Status::Planned);
     b.SrcBlend=D3D12_BLEND_SRC_ALPHA;b.DestBlend=D3D12_BLEND_INV_SRC_ALPHA;assert(f.Get(p)==Status::Planned);
     b.SrcBlend=D3D12_BLEND_ONE;b.DestBlend=D3D12_BLEND_ONE;assert(f.Get(p)==Status::UnsupportedDraw);
     b.SrcBlend=D3D12_BLEND_SRC_ALPHA;b.DestBlend=D3D12_BLEND_INV_SRC_ALPHA;b.BlendOp=D3D12_BLEND_OP_REV_SUBTRACT;assert(f.Get(p)==Status::UnsupportedDraw);}
    // The draw's own alpha test sets coverage for any material; a cutoff without either stays rejected.
    f=Fixture{};f.in.draw_alpha_test=true;f.in.actual_alpha_cutoff=f.in.material.overrides[2]=45.5f/255;
    assert(f.Get(p)==Status::MaterialRejected);
    f.in.material.coverage[0]=1;assert(f.Get(p)==Status::Planned);
    f.in.draw_alpha_test=false;assert(f.Get(p)==Status::MaterialRejected);
    f=Fixture{};f.in.original_opaque_state.NodeMask=2;assert(f.Get(p)==Status::UnsupportedDraw);
    f=Fixture{};f.vertices[2].uv={0,0};assert(f.Get(p)==Status::Planned); // collapsed UVs draw one texel
    f=Fixture{};f.vertices[2].model_position=f.vertices[1].model_position;assert(f.Get(p)==Status::InvalidMesh);
}
void SamplersAndHeight(){
    Fixture f;Plan p;assert(ValidSampler(f.in.actual_sampler));
    f.in.actual_sampler.Filter=D3D12_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR;assert(f.Get(p)==Status::InvalidSampler);
    f=Fixture{};f.in.actual_sampler.AddressU=D3D12_TEXTURE_ADDRESS_MODE(99);assert(f.Get(p)==Status::InvalidSampler);
    f=Fixture{};f.in.actual_sampler.MaxAnisotropy=17;assert(f.Get(p)==Status::InvalidSampler);
    f=Fixture{};f.in.actual_sampler.MipLODBias=std::numeric_limits<float>::infinity();assert(f.Get(p)==Status::InvalidSampler);
    f=Fixture{};f.in.actual_sampler.MinLOD=2;f.in.actual_sampler.MaxLOD=1;assert(f.Get(p)==Status::InvalidSampler);
    f=Fixture{};f.in.actual_sampler.BorderColor[0]=2;assert(f.Get(p)==Status::InvalidSampler);
    f=Fixture{};auto& m=f.library->materials[0];m.has_height=true;m.height=MakeMap(MapRole::Height,{0,128});
    m.has_emission=true;m.emission=MakeMap(MapRole::Emission,{50,100,150,255});
    controls::Parameters parameters;parameters.height_mode=controls::HeightMode::Parallax;parameters.parallax_height=.05f;
    assert(pipeline::BuildMaterial(parameters,NormalConvention::OpenGl,true,true,false,.5f,f.in.material)==pipeline::Status::Ready);
    f.in.evidence.height_map=f.in.evidence.height_units_proven=true;assert(f.Get(p)==Status::Planned&&p.maps[3].map==&m.height&&p.maps[4].map==&m.emission);
    parameters.height_mode=controls::HeightMode::Displacement;parameters.displacement_height=.2f;parameters.tessellation=16;
    assert(pipeline::BuildMaterial(parameters,NormalConvention::OpenGl,true,true,false,.5f,f.in.material)==pipeline::Status::Ready);
    assert(f.Get(p)==Status::UnsupportedDraw);
    auto& e=f.in.evidence;e.tessellation_supported=e.shared_edges_and_uv_continuity_proven=e.displacement_bounds_and_culling_proven=true;
    e.displacement_depth_and_multipass_proven=e.displacement_budget_proven=true;assert(f.Get(p)==Status::Planned);
    std::vector<uint32_t> indices;indices.reserve(4097*3);for(unsigned i=0;i<4097;++i){indices.push_back(0);indices.push_back(1);indices.push_back(2);}
    f.in.indices=indices;assert(f.Get(p)==Status::BudgetExceeded);
    f.in.indices={};m.height.storage=Storage::Rgba8;assert(f.Get(p)==Status::MaterialRejected);
}
void CompileActualStagesWithoutDevice(){
    const std::string sources[]={pipeline::VertexHlsl(),pipeline::VertexHlsl(true),pipeline::PixelHlsl(),pipeline::HullHlsl(),pipeline::DomainHlsl()};
    const char* profiles[]={"vs_5_0","vs_5_0","ps_5_0","hs_5_0","ds_5_0"};
    for(unsigned i=0;i<5;++i){ComPtr<ID3DBlob> code,error;
        const HRESULT hr=D3DCompile(sources[i].data(),sources[i].size(),"device-helper-cpu-contract",nullptr,nullptr,"main",profiles[i],
            D3DCOMPILE_ENABLE_STRICTNESS|D3DCOMPILE_WARNINGS_ARE_ERRORS|D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&code,&error);
        if(FAILED(hr)&&error)std::cerr.write(static_cast<const char*>(error->GetBufferPointer()),error->GetBufferSize());assert(SUCCEEDED(hr));
        ComPtr<ID3D11ShaderReflection> reflection;assert(SUCCEEDED(D3DReflect(code->GetBufferPointer(),code->GetBufferSize(),IID_PPV_ARGS(&reflection))));
        auto binding=[&](const char* name,D3D_SHADER_INPUT_TYPE type,unsigned slot){D3D11_SHADER_INPUT_BIND_DESC b{};
            assert(SUCCEEDED(reflection->GetResourceBindingDescByName(name,&b))&&b.Type==type&&b.BindPoint==slot);};
        if(i==0||i==1||i==3||i==4)binding("PbrGeometry",D3D_SIT_CBUFFER,0);
        if(i==2||i==3||i==4)binding("PbrMaterial",D3D_SIT_CBUFFER,1);
        if(i==2){binding("PbrScene",D3D_SIT_CBUFFER,2);const char* maps[]={"pbr_albedo","pbr_normal","pbr_orm","pbr_height_map","pbr_emission"};
            for(unsigned n=0;n<5;++n)binding(maps[n],D3D_SIT_TEXTURE,n);binding("pbr_sampler",D3D_SIT_SAMPLER,0);}
        if(i==4){binding("pbr_height_map",D3D_SIT_TEXTURE,3);binding("pbr_sampler",D3D_SIT_SAMPLER,0);}
    }
    pipeline::RootLayout layout;auto desc=layout.Describe();ComPtr<ID3DBlob> serialized,error;
    assert(SUCCEEDED(D3D12SerializeRootSignature(&desc,D3D_ROOT_SIGNATURE_VERSION_1,&serialized,&error)));
    assert(desc.NumParameters==5&&layout.ranges[0].NumDescriptors==5&&layout.ranges[1].NumDescriptors==1);
}
}
int main(){DisabledAndIdentity();FormatsAndOwnership();BudgetsAndOutput();SamplersAndHeight();CompileActualStagesWithoutDevice();
    std::cout<<"pbr device: CPU-only exact-owner/material planning, format/mip/upload/resident/tess budgets, real output/sampler contracts and default-off/invalid-context no-callback checks passed; actual D3D12 creation/draw code compiled, no GPU device created\n";}
