#include "gpu/pbr_pipeline.h"
#include <d3dcompiler.h>
#include <d3d11shader.h>
#include <wrl/client.h>
#include <cassert>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>

using namespace gpu::pbr;
using namespace gpu::pbr::pipeline;
using Microsoft::WRL::ComPtr;
namespace {
constexpr float eps=2e-5f;
void Near(float a,float b,float e=eps){assert(std::isfinite(a)&&std::isfinite(b)&&std::abs(a-b)<=e);}
void Near(Vec3 a,Vec3 b,float e=eps){Near(a.x,b.x,e);Near(a.y,b.y,e);Near(a.z,b.z,e);}
struct Fixture {
    GeometryConstants geometry;MaterialConstants material;SceneConstants scene;
    controls::Parameters parameters;
    geometry::AffineBridge bridge;
    Fixture(){assert(geometry::BuildAffineBridge(geometry::RowMatrix4::Identity(),geometry::RowMatrix4::Identity(),bridge)==geometry::Status::Ready);
        assert(BuildGeometry(bridge,geometry::RowMatrix4::Identity(),geometry::RowMatrix4::Identity(),1920,1080,640,480,
            TessellationPolicy::SymmetricViewEdges,16,geometry)==Status::Ready);
        assert(BuildMaterial(parameters,NormalConvention::OpenGl,false,false,false,.5f,material)==Status::Ready);
        assert(BuildScene({}, {1,2,3},1,OutputPolicy::ToneMappedSrgb,scene)==Status::Ready);}
};
void Constants(){
    Fixture f;assert(ValidateConstants(f.geometry,f.material,f.scene)==Status::Ready);
    assert(f.geometry.raster[0]==1920&&f.geometry.surface[0]==640);
    f.geometry.model_to_clip.values[12]=37;assert(ValidateConstants(f.geometry,f.material,f.scene)==Status::Ready);
    // Four distinct matrices are kept, with row-major bytes; the ordinary VS consumes model_to_clip directly.
    assert(f.geometry.model_to_view.values[12]==0&&f.geometry.model_to_clip.values[12]==37);
    f.parameters.base_color={2,.5f,1};f.parameters.emission_color={.1f,2,4};
    f.parameters.roughness_factor=3;f.parameters.metallic_factor=2;f.parameters.normal_strength=4;
    f.parameters.ao_strength=2;f.parameters.emission_strength=16;f.parameters.roughness_override=true;
    f.parameters.roughness=.2f;f.parameters.metallic_override=true;f.parameters.metallic=.8f;
    f.parameters.height_mode=controls::HeightMode::Displacement;f.parameters.displacement_height=.25f;f.parameters.tessellation=16;
    assert(BuildMaterial(f.parameters,NormalConvention::DirectX,true,true,true,.123f,f.material)==Status::Ready);
    assert(f.material.base_color[3]==1&&f.material.emission_color[3]==0&&f.material.surface[2]==4);
    assert((f.material.options==std::array<uint32_t,4>{1,1,1,2}));Near(f.material.overrides[2],.123f);
    assert(ValidateConstants(f.geometry,f.material,f.scene)==Status::Ready);
    f.parameters.normal_strength=std::numeric_limits<float>::quiet_NaN();
    assert(BuildMaterial(f.parameters,NormalConvention::OpenGl,true,false,false,.5f,f.material)==Status::InvalidConstants);
    f.parameters={};f.parameters.height_mode=controls::HeightMode::Parallax;
    assert(BuildMaterial(f.parameters,NormalConvention::OpenGl,false,false,false,.5f,f.material)==Status::MissingHeight);
    f.parameters={};assert(BuildMaterial(f.parameters,NormalConvention::OpenGl,false,false,false,.5f,f.material)==Status::Ready);
    f.material.base_color[3]=.5f;assert(ValidateConstants(f.geometry,f.material,f.scene)==Status::InvalidConstants);
    f.material.base_color[3]=1;f.material.options[3]=999;assert(ValidateConstants(f.geometry,f.material,f.scene)==Status::InvalidConstants);
    Fixture q;q.geometry.normal_to_view.values[0]=2;assert(ValidateConstants(q.geometry,q.material,q.scene)==Status::InvalidConstants);
    q=Fixture{};q.geometry.view_to_clip={};assert(ValidateConstants(q.geometry,q.material,q.scene)==Status::InvalidConstants);
    q=Fixture{};q.geometry.model_to_clip={};assert(ValidateConstants(q.geometry,q.material,q.scene)==Status::InvalidConstants);
    q=Fixture{};q.scene.info[0]=9;assert(ValidateConstants(q.geometry,q.material,q.scene)==Status::InvalidConstants);
    // Face normals are the only other geometry normal source.
    q=Fixture{};q.geometry.options[1]=1;assert(ValidateConstants(q.geometry,q.material,q.scene)==Status::Ready);
    q.geometry.options[1]=2;assert(ValidateConstants(q.geometry,q.material,q.scene)==Status::InvalidConstants);
    // Baked mode: no lights, no uniform irradiance, exposure and policy kept.
    q=Fixture{};assert(BuildBakedScene(2,OutputPolicy::ToneMappedSrgb,q.scene)==Status::Ready);
    assert((q.scene.info==std::array<uint32_t,4>{0,1,1,0})&&q.scene.irradiance_exposure[3]==2&&q.scene.irradiance_exposure[0]==0);
    assert(ValidateConstants(q.geometry,q.material,q.scene)==Status::Ready);
    q.scene.info[0]=1;assert(ValidateConstants(q.geometry,q.material,q.scene)==Status::InvalidConstants);q.scene.info[0]=0;
    q.scene.irradiance_exposure[1]=1;assert(ValidateConstants(q.geometry,q.material,q.scene)==Status::InvalidConstants);
    q.scene.irradiance_exposure[1]=0;q.scene.info[2]=2;assert(ValidateConstants(q.geometry,q.material,q.scene)==Status::InvalidConstants);
    assert(BuildBakedScene(65,OutputPolicy::LinearHdr,q.scene)==Status::InvalidConstants&&q.scene.info[2]==0);
    assert(BuildBakedScene(1,OutputPolicy(2),q.scene)==Status::InvalidConstants);
    // Fog: the legacy table modes, a finite linear range and an in-range colour; mode 0 stores nothing.
    q=Fixture{};assert(SetFog(3,10,200,0,{.25f,.5f,1},q.scene)==Status::Ready);
    assert((q.scene.fog==std::array<float,4>{10,200,0,3})&&(q.scene.fog_color==std::array<float,4>{.25f,.5f,1,0}));
    assert(ValidateConstants(q.geometry,q.material,q.scene)==Status::Ready);
    const auto fogged=q.scene;
    assert(SetFog(3,10,10,0,{0,0,0},q.scene)==Status::InvalidConstants&&q.scene.fog==fogged.fog);
    assert(SetFog(4,0,1,0,{0,0,0},q.scene)==Status::InvalidConstants);
    assert(SetFog(1,0,0,-1,{0,0,0},q.scene)==Status::InvalidConstants);
    assert(SetFog(2,0,0,std::numeric_limits<float>::quiet_NaN(),{0,0,0},q.scene)==Status::InvalidConstants);
    assert(SetFog(1,0,0,.01f,{1.5f,0,0},q.scene)==Status::InvalidConstants);
    assert(SetFog(2,0,0,.01f,{1,1,1},q.scene)==Status::Ready&&q.scene.fog[3]==2);
    assert((SetFog(0,5,6,7,{1,1,1},q.scene)==Status::Ready&&q.scene.fog==std::array<float,4>{}&&q.scene.fog_color==std::array<float,4>{}));
    q.scene.fog={0,0,0,5};assert(ValidateConstants(q.geometry,q.material,q.scene)==Status::InvalidConstants);
    q.scene.fog={1,0,0,0};assert(ValidateConstants(q.geometry,q.material,q.scene)==Status::InvalidConstants);
    geometry::RowMatrix4 projective=geometry::RowMatrix4::Identity();projective.values[11]=1;
    assert(BuildGeometry(q.bridge,projective,projective,16384,16384,640,480,TessellationPolicy::UniformSharedEdges,4,q.geometry)==Status::Ready);
    assert(BuildGeometry(q.bridge,projective,projective,16385,16384,640,480,TessellationPolicy::UniformSharedEdges,4,q.geometry)==Status::InvalidConstants);
}
void Mesh(){
    std::array<Vertex,3> v{{{{0,0,0},{0,0,1},{0,0}},{{1,0,0},{0,0,2},{1,0}},{{0,1,0},{0,0,4},{0,1}}}};
    assert(ValidateMesh(v)==Status::Ready);std::array<uint32_t,3> i{0,2,1};assert(ValidateMesh(v,i)==Status::Ready);
    i[2]=3;assert(ValidateMesh(v,i)==Status::InvalidMesh);i[2]=1;
    auto bad=v;bad[1].uv={0,0};assert(ValidateMesh(bad)==Status::Ready); // collapsed UVs: one texel, no frame
    bad=v;bad[2].model_position={2,0,0};assert(ValidateMesh(bad)==Status::InvalidMesh);
    bad=v;bad[1].model_normal={0,0,-1};assert(ValidateMesh(bad)==Status::InvalidMesh);
    bad=v;bad[0].model_normal={};assert(ValidateMesh(bad)==Status::InvalidMesh);
    bad=v;bad[0].uv.x=std::numeric_limits<float>::infinity();assert(ValidateMesh(bad)==Status::InvalidMesh);
    assert(ValidateMesh({})==Status::InvalidMesh);assert(ValidateMesh({v.data(),2})==Status::InvalidMesh);
    bad=v;bad[2].color[1]=1.5f;assert(ValidateMesh(bad)==Status::InvalidMesh);
    bad=v;bad[2].color[3]=std::numeric_limits<float>::quiet_NaN();assert(ValidateMesh(bad)==Status::InvalidMesh);
    // Face normals: packed normals need only be unit; each triangle's own plane must give a frame.
    bad=v;bad[1].model_normal={0,0,-1};
    assert(ValidateMesh(bad,{},NormalSource::Face)==Status::Ready&&ValidateMesh(bad,i,NormalSource::Face)==Status::Ready);
    bad[0].model_normal={};assert(ValidateMesh(bad,{},NormalSource::Face)==Status::InvalidMesh);
    bad=v;bad[2].model_position={2,0,0};assert(ValidateMesh(bad,{},NormalSource::Face)==Status::InvalidMesh);
    bad=v;bad[1].uv={0,0};assert(ValidateMesh(bad,{},NormalSource::Face)==Status::Ready);
    assert(ValidateMesh(v,{},NormalSource(2))==Status::InvalidMesh);
    // A triangle without a frame (collinear UVs, or a sliver in face mode) is allowed beside a framed one.
    std::array<Vertex,5> quad{{{{0,0,0},{0,0,1},{0,0}},{{1,0,0},{0,0,1},{1,0}},{{0,1,0},{0,0,1},{0,1}},
                               {{1,1,0},{0,0,1},{.5f,.5f}},{{2,2,0},{0,0,1},{.25f,.25f}}}};
    std::array<uint32_t,6> smeared{0,1,2, 0,3,4}; // second triangle: UVs on one line
    std::string why;
    assert(ValidateMesh(quad,smeared,NormalSource::Authored,&why)==Status::Ready&&why.empty());
    assert(ValidateMesh(quad,smeared,NormalSource::Face)==Status::Ready);
    quad[4].model_position={1,1+1e-7f,0}; // a near-collinear sliver, not exactly zero area
    assert(ValidateMesh(quad,smeared,NormalSource::Face)==Status::Ready);
    // ... but a mesh with no area at all is rejected, and says so
    auto flat=v;flat[2].model_position={2,0,0};
    assert(ValidateMesh(flat,{},NormalSource::Face,&why)==Status::InvalidMesh&&why.find("no triangle has area")!=std::string::npos);
    // Authored normals still have to agree, and the reason names the triangle and the count.
    auto creased=quad;creased[3].model_normal={0,0,-1};quad[4].model_position={2,2,0};creased[4].model_position={2,2,0};
    assert(ValidateMesh(creased,smeared,NormalSource::Authored,&why)==Status::InvalidMesh);
    assert(why=="triangle 1: vertex normals disagree (1 of 2 triangles fail)");
}
void Gates(){
    Evidence e;assert(Eligible(e,controls::HeightMode::None)==Status::Disabled);
    e.enabled=e.approved_assignment=true;e.texture=e.expected_texture=1;e.vertex_shader=e.expected_vertex_shader=2;e.pixel_shader=e.expected_pixel_shader=3;
    assert(Eligible(e,controls::HeightMode::None)==Status::UnsupportedDraw);
    e.fixed_transform=e.triangle_list=e.opaque=true;assert(Eligible(e,controls::HeightMode::None)==Status::MissingGeometry);
    e.packed_mesh_validated=e.original_normal_uv_proven=e.affine_bridge_proven=e.view_projection_proven=true;
    assert(Eligible(e,controls::HeightMode::None)==Status::MissingLighting);e.linear_lights_proven=true;
    assert(Eligible(e,controls::HeightMode::None)==Status::MissingOutput);e.output_policy_proven=true;
    assert(Eligible(e,controls::HeightMode::None)==Status::MissingBindings);e.all_five_bindings=true;
    assert(Eligible(e,controls::HeightMode::None)==Status::Ready);
    for(auto member:{&Evidence::programmable,&Evidence::vertex_blended,&Evidence::pretransformed,&Evidence::screen_space}){
        e.*member=true;assert(Eligible(e,controls::HeightMode::None)==Status::UnsupportedDraw);e.*member=false;}
    e.pixel_shader=9;assert(Eligible(e,controls::HeightMode::None)==Status::IdentityMismatch);e.pixel_shader=3;
    assert(Eligible(e,controls::HeightMode::Parallax)==Status::MissingHeight);e.height_map=e.height_units_proven=true;
    assert(Eligible(e,controls::HeightMode::Parallax)==Status::Ready);
    assert(Eligible(e,controls::HeightMode::Displacement)==Status::MissingDisplacementProof);
    e.tessellation_supported=e.shared_edges_and_uv_continuity_proven=e.displacement_bounds_and_culling_proven=true;
    assert(Eligible(e,controls::HeightMode::Displacement)==Status::MissingDisplacementProof);
    e.displacement_depth_and_multipass_proven=e.displacement_budget_proven=true;
    assert(Eligible(e,controls::HeightMode::Displacement)==Status::Ready);
    e.shared_edges_and_uv_continuity_proven=false;assert(Eligible(e,controls::HeightMode::Displacement)==Status::MissingDisplacementProof);
    assert(ValidateTessellationBudget(4096,16)==Status::Ready);
    assert(ValidateTessellationBudget(4097,16)==Status::MissingDisplacementProof);
    assert(ValidateTessellationBudget(0,1)==Status::MissingDisplacementProof);
    assert(ValidateTessellationBudget(1,17)==Status::MissingDisplacementProof);
}
void Numeric(){
    Light l;l.type=LightType::Point;l.position={0,0,2};l.range=10;l.linear_radiance={4,8,12};l.attenuation0=0;l.attenuation2=1;Vec3 direction;
    Near(LightRadiance(l,{0,0,0},direction),{1,2,3});Near(direction,{0,0,1});
    Near(LightRadiance(l,{0,0,-2},direction),{.25f,.5f,.75f});
    Near(LightRadiance(l,{0,0,-10},direction),{});Near(LightRadiance(l,l.position,direction),{});
    l.type=LightType::Spot;l.direction={0,0,-1};l.cos_inner=.9f;l.cos_outer=.7f;
    Near(LightRadiance(l,{0,0,0},direction),{1,2,3});l.direction={0,0,1};Near(LightRadiance(l,{0,0,0},direction),{});
    l.falloff=0;Near(LightRadiance(l,{0,0,0},direction),{}); // Outside cone remains dark; never 0^0 = 1.
    l.direction={0,0,-1};Near(LightRadiance(l,{0,0,0},direction),{1,2,3});
    l.type=LightType::Directional;l.direction={0,0,-7};Near(LightRadiance(l,{500,700,900},direction),{4,8,12});Near(direction,{0,0,1});
    Near(EncodeOutput({1,2,3},2,OutputPolicy::LinearHdr),{2,4,6});
    Near(EncodeOutput({1,0,-1},1,OutputPolicy::ToneMappedSrgb),{.73535698f,0,0});
    Vec3 moved;assert(DisplaceViewPosition({0,0,2},{1,0,0},1,.2f,.5f,moved)==Status::Ready);Near(moved,{.1f,0,2});
    assert(DisplaceViewPosition({0,0,2},{0,0,9},1,.2f,.5f,moved)==Status::Ready);Near(moved,{0,0,2.1f});
    assert(DisplaceViewPosition({0,0,2},{0,0,1},0,.2f,.5f,moved)==Status::Ready);Near(moved,{0,0,1.9f});
    assert(DisplaceViewPosition({0,0,2},{},1,.2f,.5f,moved)==Status::InvalidConstants);
    // Real geometry shifts both projection and depth. Parallax has no geometry or depth-output API.
    assert(DomainHlsl().find("o.view_position+=o.view_normal")!=std::string::npos);
    assert(PixelHlsl().find("SV_Depth")==std::string::npos);
    assert(VertexHlsl().find("mul(float4(i.model_position,1),pbr_model_to_clip)")!=std::string::npos);
}
ComPtr<ID3DBlob> Compile(const std::string& source,const char* target){
    ComPtr<ID3DBlob> code,error;HRESULT hr=D3DCompile(source.data(),source.size(),"separate-pbr-pipeline",nullptr,nullptr,"main",target,
        D3DCOMPILE_ENABLE_STRICTNESS|D3DCOMPILE_WARNINGS_ARE_ERRORS|D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&code,&error);
    if(FAILED(hr)){if(error)std::cerr.write(static_cast<const char*>(error->GetBufferPointer()),error->GetBufferSize());assert(false);}return code;
}
ComPtr<ID3D11ShaderReflection> Reflect(ID3DBlob* b){ComPtr<ID3D11ShaderReflection> r;assert(SUCCEEDED(D3DReflect(b->GetBufferPointer(),b->GetBufferSize(),IID_PPV_ARGS(&r))));return r;}
void Buffer(ID3D11ShaderReflection* r,const char* name,uint32_t bytes,std::initializer_list<std::pair<const char*,uint32_t>> fields){
    auto b=r->GetConstantBufferByName(name);D3D11_SHADER_BUFFER_DESC d{};assert(SUCCEEDED(b->GetDesc(&d))&&d.Size==bytes);
    for(auto [field,offset]:fields){D3D11_SHADER_VARIABLE_DESC v{};assert(SUCCEEDED(b->GetVariableByName(field)->GetDesc(&v))&&v.StartOffset==offset);}
}
void ShaderContracts(){
    auto vs=Compile(VertexHlsl(),"vs_5_0"),ps=Compile(PixelHlsl(),"ps_5_0"),patch=Compile(VertexHlsl(true),"vs_5_0"),hs=Compile(HullHlsl(),"hs_5_0"),ds=Compile(DomainHlsl(),"ds_5_0");
    auto vr=Reflect(vs.Get()),pr=Reflect(ps.Get()),hr=Reflect(hs.Get()),dr=Reflect(ds.Get());
    Buffer(vr.Get(),"PbrGeometry",sizeof(GeometryConstants),{{"pbr_model_to_view",0},{"pbr_normal_to_view",64},{"pbr_model_to_clip",128},{"pbr_view_to_clip",192},{"pbr_raster",256},{"pbr_surface_size",272},{"pbr_geometry_options",288}});
    auto matrix=vr->GetConstantBufferByName("PbrGeometry")->GetVariableByName("pbr_model_to_clip")->GetType();D3D11_SHADER_TYPE_DESC type{};
    assert(SUCCEEDED(matrix->GetDesc(&type))&&type.Class==D3D_SVC_MATRIX_ROWS&&type.Rows==4&&type.Columns==4);
    Buffer(pr.Get(),"PbrMaterial",sizeof(MaterialConstants),{{"pbr_base_color",0},{"pbr_emission_color",16},{"pbr_surface",32},{"pbr_overrides",48},{"pbr_height",64},{"pbr_options",80},{"pbr_maps",96},{"pbr_coverage",112}});
    Buffer(pr.Get(),"PbrScene",sizeof(SceneConstants),{{"pbr_irradiance_exposure",0},{"pbr_scene_info",16},{"pbr_lights",32},
        {"pbr_fog",672},{"pbr_fog_color",688}});
    Buffer(vr.Get(),"PbrScene",sizeof(SceneConstants),{{"pbr_fog",672}}); // Per-vertex fog, like the legacy path.
    const char* maps[]={"pbr_albedo","pbr_normal","pbr_orm","pbr_height_map","pbr_emission"};
    for(unsigned i=0;i<5;++i){D3D11_SHADER_INPUT_BIND_DESC b{};assert(SUCCEEDED(pr->GetResourceBindingDescByName(maps[i],&b))&&b.Type==D3D_SIT_TEXTURE&&b.BindPoint==i&&b.Dimension==D3D_SRV_DIMENSION_TEXTURE2D);}
    D3D11_SHADER_INPUT_BIND_DESC b{};assert(SUCCEEDED(dr->GetResourceBindingDescByName("pbr_height_map",&b))&&b.BindPoint==3);
    D3D11_SHADER_DESC h{},d{};assert(SUCCEEDED(hr->GetDesc(&h))&&h.cControlPoints==3&&h.TessellatorDomain==D3D_TESSELLATOR_DOMAIN_TRI);
    assert(SUCCEEDED(dr->GetDesc(&d))&&d.TessellatorDomain==D3D_TESSELLATOR_DOMAIN_TRI);
    D3D11_SHADER_DESC vd{};assert(SUCCEEDED(vr->GetDesc(&vd))&&vd.InputParameters==4);
    RootLayout root;auto desc=root.Describe();assert(desc.NumParameters==5&&desc.pParameters==root.parameters.data());
    assert(root.parameters[2].ShaderVisibility==D3D12_SHADER_VISIBILITY_ALL&&root.parameters[3].ShaderVisibility==D3D12_SHADER_VISIBILITY_ALL);
    assert(root.ranges[0].NumDescriptors==5&&root.ranges[1].NumDescriptors==1);
    assert(DrawTopology(controls::HeightMode::None)==D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    assert(DrawTopology(controls::HeightMode::Displacement)==D3D_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST);
    ComPtr<ID3DBlob> serialized,error;assert(SUCCEEDED(D3D12SerializeRootSignature(&desc,D3D_ROOT_SIGNATURE_VERSION_1,&serialized,&error)));
    auto byte=[](ID3DBlob* c){return D3D12_SHADER_BYTECODE{c->GetBufferPointer(),c->GetBufferSize()};};
    D3D12_GRAPHICS_PIPELINE_STATE_DESC legacy{},pso{};legacy.NumRenderTargets=1;legacy.RTVFormats[0]=DXGI_FORMAT_R8G8B8A8_UNORM;legacy.SampleDesc.Count=1;
    legacy.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;legacy.RasterizerState.FrontCounterClockwise=TRUE;
    legacy.DepthStencilState.DepthFunc=D3D12_COMPARISON_FUNC_EQUAL;
    auto fake_root=reinterpret_cast<ID3D12RootSignature*>(uintptr_t(1));
    ShaderSet shaders{byte(vs.Get()),byte(ps.Get())};
    assert(DescribePipeline(legacy,fake_root,shaders,controls::HeightMode::None,pso)==Status::Ready);
    assert(pso.pRootSignature==fake_root&&pso.InputLayout.NumElements==4&&pso.DepthStencilState.DepthFunc==D3D12_COMPARISON_FUNC_EQUAL&&pso.RasterizerState.FrontCounterClockwise);
    assert(pso.PrimitiveTopologyType==D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE&&!pso.HS.pShaderBytecode);
    shaders={byte(patch.Get()),byte(ps.Get()),byte(hs.Get()),byte(ds.Get())};
    assert(DescribePipeline(legacy,fake_root,shaders,controls::HeightMode::Displacement,pso)==Status::Ready&&pso.PrimitiveTopologyType==D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH);
    assert(DescribePipeline(legacy,fake_root,shaders,controls::HeightMode::Parallax,pso)==Status::InvalidPipeline);
    shaders.hs={};assert(DescribePipeline(legacy,fake_root,shaders,controls::HeightMode::Displacement,pso)==Status::InvalidPipeline);
    legacy.BlendState.RenderTarget[0].BlendEnable=TRUE;assert(DescribePipeline(legacy,fake_root,{},controls::HeightMode::None,pso)==Status::InvalidPipeline);
}
}
int main(){Constants();Mesh();Gates();Numeric();ShaderContracts();std::cout<<"pbr pipeline: guarded fixed mesh, exact raster matrices, linear light/output math, strict eligibility, real VS/PS/HS/DS compile/reflection and root/PSO contracts passed; no GPU execution\n";}
