#include "gpu/pbr_pipeline.h"
#include "gpu/pbr_brdf_shader.h"
#include "gpu/pbr_pipeline_shader.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <unordered_set>

namespace gpu::pbr::pipeline {
namespace {
bool Finite(float v) { return std::isfinite(v); }
bool Finite(Vec3 v) { return Finite(v.x)&&Finite(v.y)&&Finite(v.z); }
template<class T> bool FiniteArray(const T& a) { return std::all_of(a.begin(),a.end(),[](float v){return Finite(v);}); }
bool Range(float v,float lo,float hi) { return Finite(v)&&v>=lo&&v<=hi; }
Vec3 Scale(Vec3 a,float b) { return {a.x*b,a.y*b,a.z*b}; }
Vec3 Sub(Vec3 a,Vec3 b) { return {a.x-b.x,a.y-b.y,a.z-b.z}; }
float Dot(Vec3 a,Vec3 b) { return a.x*b.x+a.y*b.y+a.z*b.z; }
Vec3 Cross(Vec3 a,Vec3 b) { return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x}; }
// Mirrors the legacy fogfactor(): mode 3 linear, 1 exp, 2 exp2, 0 none (stored as all zero).
bool ValidFog(const std::array<float,4>& fog,const std::array<float,4>& color) {
    if(!FiniteArray(fog)||!FiniteArray(color)||color[3]!=0)return false;
    for(size_t i=0;i<3;++i)if(!Range(color[i],0,1))return false;
    const float mode=fog[3];
    if(mode==0)return fog[0]==0&&fog[1]==0&&fog[2]==0&&color[0]==0&&color[1]==0&&color[2]==0;
    if(mode==3)return fog[0]!=fog[1]&&std::isfinite(1/(fog[1]-fog[0]));
    return (mode==1||mode==2)&&Range(fog[2],0,1e6f);
}
bool Unit(Vec3 a,Vec3& out) {
    if(!Finite(a))return false;
    const float scale=std::max({std::abs(a.x),std::abs(a.y),std::abs(a.z)});
    if(!(scale>0))return false;
    a=Scale(a,1/scale);const float length=std::sqrt(Dot(a,a));
    if(!(length>0)||!Finite(length))return false;out=Scale(a,1/length);return true;
}
bool ValidLight(const Light& l) {
    if(static_cast<uint32_t>(l.type)>2||!Finite(l.position)||!Finite(l.direction)||!Finite(l.linear_radiance))return false;
    for(float v:{l.range,l.attenuation0,l.attenuation1,l.attenuation2,l.falloff,l.cos_inner,l.cos_outer})if(!Finite(v))return false;
    for(float v:{l.linear_radiance.x,l.linear_radiance.y,l.linear_radiance.z})if(!Range(v,0,1e6f))return false;
    Vec3 n;if((l.type!=LightType::Point)&&!Unit(l.direction,n))return false;
    if(l.type!=LightType::Directional) {
        if(!Range(l.range,1e-6f,1e12f)||!Range(l.attenuation0,0,1e12f)||!Range(l.attenuation1,0,1e12f)||
           !Range(l.attenuation2,0,1e12f)||!(l.attenuation0+l.attenuation1+l.attenuation2>0))return false;
    }
    return l.type!=LightType::Spot||(Range(l.falloff,0,128)&&Range(l.cos_inner,-1,1)&&Range(l.cos_outer,-1,1)&&l.cos_inner>=l.cos_outer);
}
bool Bytecode(D3D12_SHADER_BYTECODE b) { return b.pShaderBytecode&&b.BytecodeLength; }
bool Zero(D3D12_SHADER_BYTECODE b) { return !b.pShaderBytecode&&!b.BytecodeLength; }
bool Invertible(const geometry::RowMatrix4& matrix) {
    double a[4][4];double scale=0;
    for(unsigned r=0;r<4;++r)for(unsigned c=0;c<4;++c){a[r][c]=matrix.values[r*4+c];scale=std::max(scale,std::abs(a[r][c]));}
    if(!(scale>0))return false;
    for(unsigned c=0;c<4;++c){unsigned pivot=c;for(unsigned r=c+1;r<4;++r)if(std::abs(a[r][c])>std::abs(a[pivot][c]))pivot=r;
        if(std::abs(a[pivot][c])<=scale*1e-12)return false;
        for(unsigned k=0;k<4;++k)std::swap(a[c][k],a[pivot][k]);
        for(unsigned r=c+1;r<4;++r){double f=a[r][c]/a[c][c];for(unsigned k=c;k<4;++k)a[r][k]-=f*a[c][k];}}
    return true;
}
std::string CommonHlsl() { return std::string(kBrdfHlsl)+std::string(geometry::GeometryHlsl())+std::string(shader::kCommon); }
}

const char* StatusText(Status s) {
    switch(s) {
    case Status::Ready:return "ready";case Status::Disabled:return "disabled";case Status::IdentityMismatch:return "exact identity mismatch";
    case Status::UnsupportedDraw:return "unsupported draw";case Status::MissingGeometry:return "missing geometry proof";
    case Status::MissingLighting:return "missing linear lighting proof";case Status::MissingOutput:return "missing output binding proof";
    case Status::MissingBindings:return "missing material bindings";case Status::MissingHeight:return "missing height map or units";
    case Status::MissingDisplacementProof:return "missing true displacement proof";case Status::InvalidConstants:return "invalid constants";
    case Status::InvalidMesh:return "invalid packed mesh";case Status::InvalidPipeline:return "invalid dedicated pipeline";
    }return "unknown";
}
Status BuildGeometry(const geometry::AffineBridge& affine,const geometry::RowMatrix4& model_projection,const geometry::RowMatrix4& projection,
                     uint32_t width,uint32_t height,uint32_t guest_width,uint32_t guest_height,TessellationPolicy policy,float edge,GeometryConstants& result) {
    result={};
    if(!affine.ready||!width||!height||!guest_width||!guest_height||width>16384||height>16384||guest_width>16384||guest_height>16384||
       static_cast<uint32_t>(policy)>1||!Range(edge,1,4096)||!FiniteArray(model_projection.values)||!FiniteArray(projection.values))return Status::InvalidConstants;
    // Zero/singular raster transforms cannot establish clip space; no sequential matrix substitution.
    if(!Invertible(projection)||!Invertible(model_projection))return Status::InvalidConstants;
    // Revalidate the bridge itself; do not trust a caller-set ready flag on singular/nonaffine values.
    geometry::AffineBridge checked;
    if(geometry::BuildAffineBridge(affine.model_to_view,geometry::RowMatrix4::Identity(),checked)!=geometry::Status::Ready||
       !FiniteArray(affine.normal_to_view.values))return Status::InvalidConstants;
    for(size_t i=0;i<16;++i)if(std::abs(double(checked.normal_to_view.values[i])-affine.normal_to_view.values[i])>
       2e-5*std::max(1.0,std::abs(double(checked.normal_to_view.values[i]))))return Status::InvalidConstants;
    result.model_to_view=affine.model_to_view;result.normal_to_view=checked.normal_to_view;
    result.model_to_clip=model_projection;result.view_to_clip=projection;
    result.raster={float(width),float(height),edge,0};result.surface={float(guest_width),float(guest_height),0,0};
    result.options[0]=static_cast<uint32_t>(policy);return Status::Ready;
}
Status BuildMaterial(const controls::Parameters& p,NormalConvention convention,bool height,bool emission,bool cutout,float cutoff,
                     MaterialConstants& result) {
    result={};auto checked=p;controls::Sanitize(checked);
    if(!(checked==p)||static_cast<uint32_t>(convention)>1||!Range(cutoff,0,1))return Status::InvalidConstants;
    if(p.height_mode!=controls::HeightMode::None&&!height)return Status::MissingHeight;
    result.base_color={p.base_color[0],p.base_color[1],p.base_color[2],1};
    result.emission_color={p.emission_color[0],p.emission_color[1],p.emission_color[2],0};
    result.surface={p.roughness_factor,p.metallic_factor,p.normal_strength,p.ao_strength};
    result.overrides={p.roughness,p.metallic,cutoff,p.emission_strength};
    result.height={p.parallax_height,p.displacement_height,p.displacement_midpoint,float(p.tessellation)};
    result.options={convention==NormalConvention::DirectX?1u:0u,p.roughness_override?1u:0u,p.metallic_override?1u:0u,static_cast<uint32_t>(p.height_mode)};
    result.maps={1,1,height?1u:0u,emission?1u:0u};result.coverage={cutout?1u:0u,0,0,0};return Status::Ready;
}
Status BuildScene(std::span<const Light> lights,Vec3 irradiance,float exposure,OutputPolicy policy,SceneConstants& result) {
    result={};
    if(lights.size()>kLightCount||!Finite(irradiance)||!Range(exposure,0,64)||static_cast<uint32_t>(policy)>1)return Status::InvalidConstants;
    for(float c:{irradiance.x,irradiance.y,irradiance.z})if(!Range(c,0,1e6f))return Status::InvalidConstants;
    for(const auto& l:lights)if(!ValidLight(l))return Status::InvalidConstants;
    SceneConstants packed;packed.irradiance_exposure={irradiance.x,irradiance.y,irradiance.z,exposure};
    packed.info={static_cast<uint32_t>(lights.size()),static_cast<uint32_t>(policy),0,0};
    for(size_t i=0;i<lights.size();++i){const auto& l=lights[i];auto& d=packed.lights[i];Vec3 direction{};Unit(l.direction,direction);
        d.position_range={l.position.x,l.position.y,l.position.z,l.range};
        d.direction_type={direction.x,direction.y,direction.z,float(l.type)};
        d.radiance={l.linear_radiance.x,l.linear_radiance.y,l.linear_radiance.z,0};
        d.attenuation_falloff={l.attenuation0,l.attenuation1,l.attenuation2,l.falloff};d.cone={l.cos_inner,l.cos_outer,0,0};}
    result=packed;return Status::Ready;
}
Status BuildBakedScene(float exposure,OutputPolicy policy,SceneConstants& result) {
    result={};
    if(!Range(exposure,0,64)||static_cast<uint32_t>(policy)>1)return Status::InvalidConstants;
    result.irradiance_exposure={0,0,0,exposure};result.info={0,static_cast<uint32_t>(policy),1,0};return Status::Ready;
}
Status SetFog(uint32_t mode,float start,float end,float density,std::array<float,3> color,SceneConstants& result) {
    std::array<float,4> fog{},rgb{};
    if(mode){fog={start,end,density,float(mode)};rgb={color[0],color[1],color[2],0};}
    if(mode>3||!ValidFog(fog,rgb))return Status::InvalidConstants;
    result.fog=fog;result.fog_color=rgb;return Status::Ready;
}
Status ValidateConstants(const GeometryConstants& g,const MaterialConstants& m,const SceneConstants& s) {
    geometry::AffineBridge a;a.model_to_view=g.model_to_view;a.normal_to_view=g.normal_to_view;a.ready=true;GeometryConstants checked;
    if(g.options[1]>1||g.options[2]>1||g.options[3]>2||!Range(g.raster[0],1,16384)||!Range(g.raster[1],1,16384)||
       !Range(g.surface[0],1,16384)||!Range(g.surface[1],1,16384)||g.surface[2]!=0||g.surface[3]!=0||
       !(g.depth[3]==0||g.depth[3]==1)||!std::isfinite(g.depth[0])||!std::isfinite(g.depth[1])||!std::isfinite(g.depth[2])||
       std::floor(g.raster[0])!=g.raster[0]||std::floor(g.raster[1])!=g.raster[1]||g.raster[3]!=0||
       std::floor(g.surface[0])!=g.surface[0]||std::floor(g.surface[1])!=g.surface[1]||
       BuildGeometry(a,g.model_to_clip,g.view_to_clip,uint32_t(g.raster[0]),uint32_t(g.raster[1]),uint32_t(g.surface[0]),uint32_t(g.surface[1]),
                     TessellationPolicy(g.options[0]),g.raster[2],checked)!=Status::Ready)
        return Status::InvalidConstants;
    controls::Parameters p;
    p.base_color={m.base_color[0],m.base_color[1],m.base_color[2]};p.emission_color={m.emission_color[0],m.emission_color[1],m.emission_color[2]};
    p.roughness_factor=m.surface[0];p.metallic_factor=m.surface[1];p.normal_strength=m.surface[2];p.ao_strength=m.surface[3];
    p.roughness=m.overrides[0];p.metallic=m.overrides[1];p.emission_strength=m.overrides[3];
    p.parallax_height=m.height[0];p.displacement_height=m.height[1];p.displacement_midpoint=m.height[2];
    if(!Range(m.height[3],1,16)||std::floor(m.height[3])!=m.height[3])return Status::InvalidConstants;p.tessellation=uint32_t(m.height[3]);
    p.roughness_override=m.options[1]!=0;p.metallic_override=m.options[2]!=0;p.height_mode=controls::HeightMode(m.options[3]);
    MaterialConstants mc;
    if(m.base_color[3]!=1||m.emission_color[3]!=0||m.options[0]>1||m.options[1]>1||m.options[2]>1||
       m.maps[0]!=1||m.maps[1]!=1||m.maps[2]>1||m.maps[3]>1||m.coverage[0]>1||m.coverage[1]||m.coverage[2]||m.coverage[3]||
       BuildMaterial(p,m.options[0]?NormalConvention::DirectX:NormalConvention::OpenGl,m.maps[2]!=0,m.maps[3]!=0,m.coverage[0]!=0,m.overrides[2],mc)!=Status::Ready)
        return Status::InvalidConstants;
    if(!FiniteArray(s.irradiance_exposure)||s.info[0]>kLightCount||s.info[1]>1||s.info[2]>1||s.info[3]||!ValidFog(s.fog,s.fog_color))
        return Status::InvalidConstants;
    // The baked mode has no light or uniform irradiance input of its own.
    if(s.info[2]&&(s.info[0]||s.irradiance_exposure[0]!=0||s.irradiance_exposure[1]!=0||s.irradiance_exposure[2]!=0))
        return Status::InvalidConstants;
    std::array<Light,kLightCount> lights{};
    for(size_t i=0;i<s.info[0];++i){const auto& q=s.lights[i];
        if(!FiniteArray(q.position_range)||!FiniteArray(q.direction_type)||!FiniteArray(q.radiance)||!FiniteArray(q.attenuation_falloff)||
           !FiniteArray(q.cone)||!Range(q.direction_type[3],0,2)||std::floor(q.direction_type[3])!=q.direction_type[3]||
           q.radiance[3]!=0||q.cone[2]!=0||q.cone[3]!=0)return Status::InvalidConstants;
        lights[i]={LightType(uint32_t(q.direction_type[3])),{q.position_range[0],q.position_range[1],q.position_range[2]},
            {q.direction_type[0],q.direction_type[1],q.direction_type[2]},{q.radiance[0],q.radiance[1],q.radiance[2]},
            q.position_range[3],q.attenuation_falloff[0],q.attenuation_falloff[1],q.attenuation_falloff[2],q.attenuation_falloff[3],q.cone[0],q.cone[1]};}
    SceneConstants sc;
    return BuildScene({lights.data(),s.info[0]},{s.irradiance_exposure[0],s.irradiance_exposure[1],s.irradiance_exposure[2]},
                      s.irradiance_exposure[3],OutputPolicy(s.info[1]),sc);
}
namespace {
uint64_t MeshHash(const void* data,size_t n,uint64_t h) {
    constexpr uint64_t m=0x9E3779B97F4A7C15ull;const auto* p=static_cast<const uint8_t*>(data);
    for(;n>=8;p+=8,n-=8){uint64_t v;std::memcpy(&v,p,8);h=(h^v)*m;h^=h>>32;}
    uint64_t v=0;std::memcpy(&v,p,n);h=(h^v^n)*m;return h^(h>>29);
}
Status ValidateMeshNow(std::span<const Vertex>,std::span<const uint32_t>,NormalSource,std::string*);
}
// The same bytes validate the same way: the backend builds a mesh once and replans it every frame, and the device
// revalidates every draw. Successes are remembered per thread by a hash of the exact vertex and index bytes.
Status ValidateMesh(std::span<const Vertex> vertices,std::span<const uint32_t> indices,NormalSource source,std::string* why) {
    thread_local std::unordered_set<uint64_t> valid;
    uint64_t key=MeshHash(vertices.data(),vertices.size_bytes(),uint64_t(source)+1);
    key=MeshHash(indices.data(),indices.size_bytes(),key^vertices.size());
    if(valid.count(key))return Status::Ready;
    const auto status=ValidateMeshNow(vertices,indices,source,why);
    if(status==Status::Ready){if(valid.size()>=16384)valid.clear();valid.insert(key);}
    return status;
}
namespace {
Status ValidateMeshNow(std::span<const Vertex> vertices,std::span<const uint32_t> indices,NormalSource source,std::string* why) {
    const auto fail=[&](std::string text){if(why)*why=std::move(text);return Status::InvalidMesh;};
    if(vertices.empty()||vertices.size()>kMaxMeshVertices||indices.size()>kMaxMeshIndices||uint32_t(source)>1)return fail("size/source");
    const size_t n=indices.empty()?vertices.size():indices.size();if(n<3||n%3)return fail("index count");
    for(size_t i=0;i<vertices.size();++i){const auto& v=vertices[i];Vec3 normal;
        if(!Finite(v.model_position)||!Unit(v.model_normal,normal)||!Finite(v.uv.x)||!Finite(v.uv.y)||!Finite(v.uv1.x)||!Finite(v.uv1.y)||
        !FiniteArray(v.color)||!std::all_of(v.color.begin(),v.color.end(),[](float c){return c>=0&&c<=1;}))
            return fail("vertex "+std::to_string(i)+": position/normal/UV/colour");}
    // A triangle without a tangent frame (collinear UVs, a near-zero-area sliver) still rasterizes in the original;
    // the shaders leave its normal unperturbed and skip parallax there, so it is allowed. Collapsed UVs only mean one
    // texel. The mesh needs at least one triangle with area, and authored normals must still agree. Diagnosis counts
    // every failing triangle.
    std::string first;size_t failed=0,with_area=0;
    for(size_t i=0;i<n;i+=3){const Vertex* p[3];for(unsigned k=0;k<3;++k){const size_t index=indices.empty()?i+k:indices[i+k];
            if(index>=vertices.size())return fail("index out of range");p[k]=&vertices[index];}
        const char* rule=nullptr;
        Vec3 normals[3];for(unsigned k=0;k<3;++k)Unit(p[k]->model_normal,normals[k]);
        bool frameless=false;
        if(source==NormalSource::Face) {
            // The shaded normal IS this triangle's plane; vertex normals across a crease need not agree.
            frameless=!Unit(Cross(Sub(p[1]->model_position,p[0]->model_position),Sub(p[2]->model_position,p[0]->model_position)),normals[0]);
        }else if(Dot(normals[0],normals[1])<=1e-4f||Dot(normals[0],normals[2])<=1e-4f||Dot(normals[1],normals[2])<=1e-4f)
            rule="vertex normals disagree";
        geometry::CotangentFrame frame;
        if(!rule&&!frameless)switch(geometry::BuildCotangentFrame(Sub(p[1]->model_position,p[0]->model_position),Sub(p[2]->model_position,p[0]->model_position),
            {p[1]->uv.x-p[0]->uv.x,p[1]->uv.y-p[0]->uv.y},{p[2]->uv.x-p[0]->uv.x,p[2]->uv.y-p[0]->uv.y},normals[0],frame)) {
            case geometry::Status::Ready:++with_area;break;
            case geometry::Status::DegenerateUv:++with_area;frameless=true;break;
            case geometry::Status::DegeneratePosition:frameless=true;break;
            case geometry::Status::DegenerateNormal:if(source==NormalSource::Face)frameless=true;else rule="normal in the triangle plane";break;
            default:rule="non-finite frame";break;
        }
        if(!rule)continue;
        if(!why)return Status::InvalidMesh;
        if(!failed++)first="triangle "+std::to_string(i/3)+": "+rule;
    }
    if(failed)return fail(first+" ("+std::to_string(failed)+" of "+std::to_string(n/3)+" triangles fail)");
    if(!with_area)return fail("no triangle has area (collinear or coincident positions throughout)");
    return Status::Ready;
}
}  // namespace
Status Eligible(const Evidence& e,controls::HeightMode mode) {
    if(!e.enabled)return Status::Disabled;
    if(!e.approved_assignment||!e.texture||!e.vertex_shader||!e.pixel_shader||e.texture!=e.expected_texture||
       e.vertex_shader!=e.expected_vertex_shader||e.pixel_shader!=e.expected_pixel_shader)return Status::IdentityMismatch;
    if(!e.fixed_transform||!e.triangle_list||!e.opaque||e.pretransformed||e.vertex_blended||e.programmable||e.screen_space||uint32_t(mode)>2)
        return Status::UnsupportedDraw;
    if(!e.packed_mesh_validated||!e.original_normal_uv_proven||!e.affine_bridge_proven||!e.view_projection_proven)return Status::MissingGeometry;
    if(!e.linear_lights_proven)return Status::MissingLighting;
    if(!e.output_policy_proven)return Status::MissingOutput;if(!e.all_five_bindings)return Status::MissingBindings;
    if(mode!=controls::HeightMode::None&&(!e.height_map||!e.height_units_proven))return Status::MissingHeight;
    if(mode==controls::HeightMode::Displacement&&(!e.tessellation_supported||!e.shared_edges_and_uv_continuity_proven||
       !e.displacement_bounds_and_culling_proven||!e.displacement_depth_and_multipass_proven||!e.displacement_budget_proven))
        return Status::MissingDisplacementProof;
    return Status::Ready;
}
Status ValidateTessellationBudget(uint32_t patches,uint32_t maximum) {
    if(!patches||maximum<1||maximum>16||uint64_t(patches)*maximum*maximum>kMaxTessellatedTriangles)return Status::MissingDisplacementProof;
    return Status::Ready;
}
D3D12_ROOT_SIGNATURE_DESC RootLayout::Describe() {
    ranges={};parameters={};ranges[0]={D3D12_DESCRIPTOR_RANGE_TYPE_SRV,kMapCount,0,0,D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND};
    ranges[1]={D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER,1,0,0,D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND};
    for(uint32_t i=0;i<3;++i){parameters[i].ParameterType=D3D12_ROOT_PARAMETER_TYPE_CBV;parameters[i].Descriptor.ShaderRegister=i;
        parameters[i].ShaderVisibility=D3D12_SHADER_VISIBILITY_ALL;} // b2 fog is evaluated per vertex.
    for(uint32_t i=0;i<2;++i){parameters[3+i].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameters[3+i].DescriptorTable={1,&ranges[i]};parameters[3+i].ShaderVisibility=D3D12_SHADER_VISIBILITY_ALL;}
    return {uint32_t(parameters.size()),parameters.data(),0,nullptr,D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT};
}
std::span<const D3D12_INPUT_ELEMENT_DESC> InputLayout() {
    static const D3D12_INPUT_ELEMENT_DESC layout[]={
        {"POSITION",0,DXGI_FORMAT_R32G32B32_FLOAT,0,0,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
        {"NORMAL",0,DXGI_FORMAT_R32G32B32_FLOAT,0,12,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
        {"TEXCOORD",0,DXGI_FORMAT_R32G32_FLOAT,0,24,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
        {"COLOR",0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,32,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
        {"TEXCOORD",1,DXGI_FORMAT_R32G32_FLOAT,0,48,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0}};
    return layout;
}
D3D_PRIMITIVE_TOPOLOGY DrawTopology(controls::HeightMode mode) {
    if(uint32_t(mode)>2)return D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
    return mode==controls::HeightMode::Displacement?D3D_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST:D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
}
bool KeepsLegacyBlend(const D3D12_BLEND_DESC& d) {
    const auto& b=d.RenderTarget[0];
    if(d.AlphaToCoverageEnable||d.IndependentBlendEnable||b.LogicOpEnable)return false;
    if(!b.BlendEnable)return true;
    return b.BlendOp==D3D12_BLEND_OP_ADD&&((b.SrcBlend==D3D12_BLEND_SRC_ALPHA&&b.DestBlend==D3D12_BLEND_INV_SRC_ALPHA)||
                                           (b.SrcBlend==D3D12_BLEND_ONE&&b.DestBlend==D3D12_BLEND_ZERO));
}
Status DescribePipeline(const D3D12_GRAPHICS_PIPELINE_STATE_DESC& legacy,ID3D12RootSignature* root,const ShaderSet& s,
                        controls::HeightMode mode,D3D12_GRAPHICS_PIPELINE_STATE_DESC& result) {
    result={};const bool tess=mode==controls::HeightMode::Displacement;
    if(!root||!Bytecode(s.vs)||!Bytecode(s.ps)||uint32_t(mode)>2||legacy.NumRenderTargets!=1||
       legacy.RTVFormats[0]==DXGI_FORMAT_UNKNOWN||!legacy.SampleDesc.Count||legacy.PrimitiveTopologyType!=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE||
       !KeepsLegacyBlend(legacy.BlendState)||
       !Zero(legacy.GS)||!Zero(legacy.HS)||!Zero(legacy.DS)||legacy.StreamOutput.NumEntries||legacy.StreamOutput.NumStrides||
       (tess?(!Bytecode(s.hs)||!Bytecode(s.ds)):(!Zero(s.hs)||!Zero(s.ds))))return Status::InvalidPipeline;
    result=legacy;result.pRootSignature=root;result.VS=s.vs;result.PS=s.ps;result.HS=s.hs;result.DS=s.ds;
    auto layout=InputLayout();result.InputLayout={layout.data(),uint32_t(layout.size())};
    result.PrimitiveTopologyType=tess?D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH:D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    // Cached legacy PSOs cannot describe these different shaders/root/layout.
    result.CachedPSO={};return Status::Ready;
}
Vec3 EncodeOutput(Vec3 color,float exposure,OutputPolicy policy) {
    if(!Finite(color)||!Range(exposure,0,64)||uint32_t(policy)>1)return {};
    auto channel=[&](float v){v=std::max(0.0f,v)*exposure;if(!Finite(v))return 0.0f;
        if(policy==OutputPolicy::LinearHdr)return v;v=v/(1+v);return v<=0.0031308f?12.92f*v:1.055f*std::pow(v,1/2.4f)-0.055f;};
    return {channel(color.x),channel(color.y),channel(color.z)};
}
Vec3 LightRadiance(const Light& l,Vec3 position,Vec3& direction) {
    direction={};if(!ValidLight(l)||!Finite(position))return {};float attenuation=1;
    if(l.type==LightType::Directional){Vec3 d;Unit(l.direction,d);direction=Scale(d,-1);}
    else {Vec3 delta=Sub(l.position,position);const double d2=double(delta.x)*delta.x+double(delta.y)*delta.y+double(delta.z)*delta.z;
        const double distance=std::sqrt(d2);if(!(distance>0)||distance>l.range||!std::isfinite(distance)||!Unit(delta,direction))return {};
        const double denominator=l.attenuation0+l.attenuation1*distance+l.attenuation2*d2;
        attenuation=float(1/std::max(denominator,1e-6));
        if(l.type==LightType::Spot){Vec3 d;Unit(l.direction,d);float rho=Dot(Scale(direction,-1),d);
            float cone=rho>=l.cos_inner?1:rho<=l.cos_outer?0:(rho-l.cos_outer)/(l.cos_inner-l.cos_outer);
            attenuation*=cone>0?std::pow(std::clamp(cone,0.0f,1.0f),l.falloff):0;}}
    return Scale(l.linear_radiance,attenuation);
}
Status DisplaceViewPosition(Vec3 position,Vec3 normal,float sample,float distance,float midpoint,Vec3& result) {
    result={};Vec3 n;
    if(!Finite(position)||!Unit(normal,n)||!Range(sample,0,1)||!Range(distance,0,0.25f)||!Range(midpoint,0,1))return Status::InvalidConstants;
    const float height=(sample-midpoint)*distance;
    result={position.x+n.x*height,position.y+n.y*height,position.z+n.z*height};
    if(!Finite(result)){result={};return Status::InvalidConstants;}return Status::Ready;
}
std::string VertexHlsl(bool tess) {return CommonHlsl()+std::string(tess?shader::kPatchVertex:shader::kVertex);}
std::string PixelHlsl() {return CommonHlsl()+std::string(shader::kPixel);}
std::string HullHlsl() {return CommonHlsl()+std::string(shader::kHull);}
std::string DomainHlsl() {return CommonHlsl()+std::string(shader::kDomain);}

} // namespace gpu::pbr::pipeline
