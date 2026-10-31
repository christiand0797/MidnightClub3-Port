// Actual raw/converted assembler + fixed bridge, with only guest memory/logging mocked. No game/device/window.
#include "gpu/pbr_fixed_mesh.h"
#include "kernel/kernel.h"

#include <cassert>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>

// Report a CPU regression without invoking the debug CRT's modal assertion/error-report path.
#define CHECK(condition) do { if(!(condition)) { std::cerr<<"Check failed: "<<#condition<<" at line "<<__LINE__<<'\n';std::exit(1); } } while(false)
namespace fixture { std::vector<uint8_t> memory(1u<<20); }
namespace kernel { uint8_t* g_base=fixture::memory.data(); }
namespace mem { uint32_t VirtualFromPhysical(uint32_t address){return address;} }
namespace logx { void Write(Level,const char*,...){} }
// Include to compare the bridge against the production Convert/HostFormat contract, not a second test decoder.
#include "../runtime/gpu/vertex.cpp"

namespace {
namespace fm=gpu::pbr::fixed_mesh;
namespace pp=gpu::pbr::pipeline;
namespace geo=gpu::pbr::geometry;
using Matrix=geo::RowMatrix4;
constexpr fm::FixedProof proof{fm::DrawKind::ObjectSpaceFixedFunction,true,true};
constexpr fm::RasterTarget target{3840,2160,640,480};
constexpr gpu::pbr::Assignment assignment{0x11,0x22,0x33,2,1,gpu::pbr::SurfaceClass::World,false};
template<class T> void Write(uint8_t* p,T value){std::memcpy(p,&value,sizeof(value));}
void Encode(uint32_t type,uint8_t* p,const std::array<float,4>& value) {
    const unsigned n=std::min((type>>4)&15,4u);
    if(type==0x72){Write(p,value[0]);Write(p+4,value[1]);Write(p+8,value[3]);return;}
    switch(type&15) {
    case 0:p[0]=uint8_t(value[2]*255);p[1]=uint8_t(value[1]*255);p[2]=uint8_t(value[0]*255);p[3]=uint8_t(value[3]*255);break;
    case 1:for(unsigned i=0;i<n;++i)Write(p+i*2,int16_t(value[i]*32767));break;
    case 2:for(unsigned i=0;i<n;++i)Write(p+i*4,value[i]);break;
    case 4:for(unsigned i=0;i<n;++i)p[i]=uint8_t(value[i]*255);break;
    case 5:for(unsigned i=0;i<n;++i)Write(p+i*2,int16_t(value[i]));break;
    case 6:Write(p,(uint32_t(int32_t(value[0]*1023))&2047)|((uint32_t(int32_t(value[1]*1023))&2047)<<11)|
                       ((uint32_t(int32_t(value[2]*511))&1023)<<22));break;
    default:CHECK(false);
    }
}
struct Fixture {
    gpu::State state;
    gpu::Assembled draw;
    uint32_t normal_type=0x16,uv_type=0x22;
    std::array<uint32_t,3> data{0x10000,0x20000,0x30000},stride{};
    Fixture(uint32_t normal=0x16,uint32_t uv=0x22):normal_type(normal),uv_type(uv) {
        std::fill(fixture::memory.begin(),fixture::memory.end(),0);
        state.vs_handle=1;state.vs=std::make_shared<gpu::VertexShader>();
        for(auto& m:state.transforms)m=Matrix::Identity().values;
        state.viewport={16,24,600,432,.125f,.875f};
        state.tss[assignment.stage][gpu::kTssTexCoordIndex]=assignment.uv_set;
        const std::array<uint32_t,3> types{0x32,normal_type,uv_type};
        const std::array<unsigned,3> regs{0,2,10};
        for(unsigned stream=0;stream<3;++stream) {
            const uint32_t header=0x1000+stream*0x100;
            stride[stream]=(gpu::TypeBytes(types[stream])+3)&~3u;
            Write(fixture::memory.data()+header+4,data[stream]);
            state.streams[stream]={header,stride[stream]};
            state.vs->inputs[regs[stream]]={int(stream),0,types[stream]};
            state.vs->stream_stride[stream]=stride[stream];
        }
        Set(0,{0,0,0,1},{0,0,1,1},{0,0,0,1});
        Set(1,{1,0,0,1},{0,0,1,1},{1,0,0,1});
        Set(2,{0,1,0,1},{0,0,1,1},{0,1,0,1});
        Assemble();
    }
    void Set(unsigned i,std::array<float,4> p,std::array<float,4> n,std::array<float,4> uv) {
        Encode(0x32,fixture::memory.data()+data[0]+i*stride[0],p);
        Encode(normal_type,fixture::memory.data()+data[1]+i*stride[1],n);
        Encode(uv_type,fixture::memory.data()+data[2]+i*stride[2],uv);
    }
    void Assemble(){CHECK(gpu::Assemble(state,5,0,3,nullptr,draw));}
    std::span<const uint8_t> Indices() const {
        return {static_cast<const uint8_t*>(draw.index_src),size_t(draw.index_count)*(draw.index16?2:4)};
    }
    fm::Status Build(fm::Mesh& mesh,fm::FixedProof p=proof,fm::Budget budget={})const {
        return fm::Build(state,draw,assignment,p,target,Indices(),mesh,budget);
    }
};
void Empty(const fm::Mesh& mesh) {
    CHECK(!mesh.ready&&mesh.vertices.empty()&&mesh.indices.empty()&&!mesh.height_units_proven);
    CHECK(!mesh.shared_edges_and_uv_continuity_proven&&!mesh.displacement_bounds_and_culling_proven&&
           !mesh.displacement_depth_and_multipass_proven&&!mesh.displacement_budget_proven);
}
void Reject(const Fixture& f,fm::Status expected) {
    fm::Mesh result;result.ready=true;result.vertices.resize(1);result.indices.push_back(0);result.height_units_proven=true;
    const auto actual=f.Build(result);
    if(actual!=expected)std::cerr<<"Expected "<<fm::StatusText(expected)<<" got "<<fm::StatusText(actual)<<'\n';
    CHECK(actual==expected);Empty(result);
}
// Frozen mathematical statement of the original FillTransformConstants multiplication order, float32 throughout.
__declspec(noinline) Matrix LegacyMultiply(const Matrix& a,const Matrix& b) {
    Matrix c;
    for(unsigned r=0;r<4;++r)for(unsigned k=0;k<4;++k)c.values[r*4+k]=
        a.values[r*4]*b.values[k]+a.values[r*4+1]*b.values[4+k]+a.values[r*4+2]*b.values[8+k]+a.values[r*4+3]*b.values[12+k];
    return c;
}
void ExactConstants() {
    Fixture f;
    Matrix world{{1.25f,0,0,0, 0,2.125f,0,0, 0,0,.75f,0, 123.125f,-99.875f,5.125f,1}};
    Matrix view{{.6f,.8f,0,0, -.8f,.6f,0,0, 0,0,1,0, -33.25f,77.125f,-1.25f,1}};
    Matrix projection{{1.13f,0,0,0, 0,1.77f,0,0, 0,0,1.0001f,1, 0,0,-.10001f,0}};
    f.state.transforms[256]=world.values;f.state.transforms[2]=view.values;f.state.transforms[3]=projection.values;
    const auto& v=f.state.viewport;
    Matrix viewport{{float(v.w)/640,0,0,0, 0,float(v.h)/480,0,0, 0,0,v.max_z-v.min_z,0,
        (2.0f*v.x+v.w)/640-1,1-(2.0f*v.y+v.h)/480,v.min_z,1}};
    const auto wv=LegacyMultiply(world,view);
    const auto clip=LegacyMultiply(LegacyMultiply(wv,projection),viewport);
    fm::Mesh mesh;const auto status=f.Build(mesh);
    if(status!=fm::Status::Ready)std::cerr<<"Constant fixture: "<<fm::StatusText(status)<<'\n';
    CHECK(status==fm::Status::Ready&&mesh.ready&&mesh.height_units_proven);
    if(mesh.geometry.model_to_clip.values!=clip.values)for(size_t i=0;i<16;++i)
        if(mesh.geometry.model_to_clip.values[i]!=clip.values[i])std::cerr<<"Clip "<<i<<": "<<std::hexfloat<<mesh.geometry.model_to_clip.values[i]<<" != "<<clip.values[i]<<'\n';
    CHECK(mesh.geometry.model_to_clip.values==clip.values);
    CHECK(mesh.geometry.model_to_view.values==wv.values);
    CHECK(mesh.geometry.view_to_clip.values==LegacyMultiply(projection,viewport).values);
    // The test has non-associative inputs: a reordered implementation must not pass this exact-raster gate.
    CHECK(clip.values!=LegacyMultiply(world,LegacyMultiply(view,LegacyMultiply(projection,viewport))).values);
    geo::AffineBridge normals;CHECK(geo::BuildAffineBridge(wv,Matrix::Identity(),normals)==geo::Status::Ready);
    CHECK(mesh.geometry.normal_to_view.values==normals.normal_to_view.values);
    CHECK(mesh.geometry.surface[0]==640&&mesh.geometry.raster[0]==3840); // Far-edge policy uses guest size.
    f.state.transforms[2][0]*=2;f.state.transforms[2][1]*=2;
    CHECK(f.Build(mesh)==fm::Status::Ready&&!mesh.height_units_proven); // A scaled camera is not unit evidence.
}
void Fvf() {
    Fixture f;
    f.state.vs_handle=0x2|0x10|(2u<<8);f.state.vs.reset();
    f.state.streams[0].stride=40;
    for(unsigned i=0;i<3;++i) {
        float data[10]{float(i==1),float(i==2),0, 0,0,1, .25f,.25f, float(i==1),float(i==2)};
        std::memcpy(fixture::memory.data()+f.data[0]+i*40,data,sizeof(data));
    }
    f.Assemble();fm::Mesh mesh;CHECK(f.Build(mesh)==fm::Status::Ready);
    CHECK(mesh.vertices[1].uv.x==1&&mesh.vertices[2].uv.y==1); // Chosen UV1, not the degenerate UV0.
    f.state.vs_handle=(f.state.vs_handle&~0xEu)|0x4;Reject(f,fm::Status::UnsupportedDraw);
    f.state.vs_handle=(f.state.vs_handle&~0xEu)|0x6;Reject(f,fm::Status::UnsupportedDraw);
    f.state.vs_handle=0x2|(2u<<8);Reject(f,fm::Status::MissingInput); // No inline authored-normal substitute.
}
void Formats() {
    // These all exercise actual Assemble in both raw and converted test binaries, comparing production Convert.
    for(uint32_t normal:{0x32u,0x42u,0x16u,0x31u,0x41u,0x35u,0x45u,0x34u,0x44u,0x40u})
        for(uint32_t uv:{0x22u,0x32u,0x42u,0x72u,0x21u,0x41u,0x25u,0x45u,0x24u,0x44u,0x40u}) {
            Fixture f(normal,uv);fm::Mesh mesh;CHECK(f.Build(mesh)==fm::Status::Ready);
            CHECK(mesh.vertices.size()==3&&mesh.indices==std::vector<uint32_t>({0,1,2}));
            for(unsigned i=0;i<3;++i) {
                float n[4],t[4];gpu::Convert(normal,fixture::memory.data()+f.data[1]+i*f.stride[1],n);
                gpu::Convert(uv,fixture::memory.data()+f.data[2]+i*f.stride[2],t);
                CHECK(mesh.vertices[i].model_normal.x==n[0]&&mesh.vertices[i].model_normal.y==n[1]&&mesh.vertices[i].model_normal.z==n[2]);
                CHECK(mesh.vertices[i].uv.x==t[0]&&mesh.vertices[i].uv.y==t[1]);
            }
            const auto saved=mesh.vertices[1];std::fill(fixture::memory.begin(),fixture::memory.end(),0);
            CHECK(std::memcmp(&mesh.vertices[1],&saved,sizeof(saved))==0); // No guest/raw pointers retained.
        }
    // SNORM minimum and 11:11:10 sign extension agree with Convert, including the saturating negative endpoint.
    for(uint32_t normal:{0x16u,0x31u}) {
        Fixture f(normal);for(unsigned i=0;i<3;++i)f.Set(i,{float(i==1),float(i==2),0,1},{0,0,-1,1},{float(i==1),float(i==2),0,1});
        if(normal==0x31)for(unsigned i=0;i<3;++i)Write(fixture::memory.data()+f.data[1]+i*f.stride[1]+4,int16_t(-32768));
        else for(unsigned i=0;i<3;++i)Write(fixture::memory.data()+f.data[1]+i*f.stride[1],uint32_t(512u<<22));
        f.Assemble();fm::Mesh mesh;CHECK(f.Build(mesh)==fm::Status::Ready&&mesh.vertices[0].model_normal.z==-1);
    }
}
void Guards() {
    {Fixture f;fm::Mesh mesh;CHECK(f.Build(mesh,{})==fm::Status::UnprovenDraw);Empty(mesh);}
    {Fixture f;f.draw.pretransformed=true;Reject(f,fm::Status::UnsupportedDraw);}
    {Fixture f;f.state.vs->program.push_back(1);Reject(f,fm::Status::UnsupportedDraw);}
    {Fixture f;f.state.rs[gpu::kRsVertexBlend]=1;Reject(f,fm::Status::UnsupportedDraw);}
    {Fixture f;f.draw.topology=gpu::Topology::Lines;Reject(f,fm::Status::UnsupportedDraw);}
    {Fixture f;f.state.tss[2][gpu::kTssTexCoordIndex]=1u|(1u<<16);Reject(f,fm::Status::UnsupportedDraw);}
    {Fixture f;f.state.tss[2][gpu::kTssTexCoordIndex]=0;Reject(f,fm::Status::UnsupportedDraw);}
    {Fixture f;f.state.tss[2][gpu::kTssTextureTransformFlags]=0x102;Reject(f,fm::Status::UnsupportedDraw);}
    {Fixture f;f.draw.regs&=~4u;f.state.vattr[2]={0,0,1,1};Reject(f,fm::Status::MissingInput);}
    {Fixture f;f.draw.elements[2].format=255;Reject(f,fm::Status::UnsupportedFormat);}
    {Fixture f;f.draw.decode|=15ull<<(2*4);Reject(f,fm::Status::UnsupportedFormat);}
    {Fixture f;f.draw.buffers[f.draw.elements[2].buffer].bytes=1;Reject(f,fm::Status::InvalidBuffer);}
    {Fixture f;f.draw.elements[2].buffer=16;Reject(f,fm::Status::InvalidBuffer);}
    {Fixture f;f.state.transforms[256][3]=.125f;Reject(f,fm::Status::InvalidTransform);}
    {Fixture f;f.state.transforms[2][0]=0;Reject(f,fm::Status::InvalidTransform);}
    {Fixture f;f.state.transforms[3][0]=std::numeric_limits<float>::infinity();Reject(f,fm::Status::InvalidTransform);}
    {Fixture f;f.state.viewport.max_z=f.state.viewport.min_z;Reject(f,fm::Status::InvalidTransform);}
    {Fixture f;f.state.viewport.x=640;Reject(f,fm::Status::InvalidTransform);}
    {Fixture f;f.Set(2,{0,1,0,1},{0,0,0,1},{0,1,0,1});f.Assemble();Reject(f,fm::Status::InvalidMesh);}
    {Fixture f;f.Set(2,{0,1,0,1},{0,0,1,1},{1,0,0,1});f.Assemble();fm::Mesh m;CHECK(f.Build(m)==fm::Status::Ready);} // collapsed UV
    {Fixture f;f.Set(2,{2,0,0,1},{0,0,1,1},{0,1,0,1});f.Assemble();Reject(f,fm::Status::InvalidMesh);}
    {Fixture f;f.Set(2,{0,std::numeric_limits<float>::quiet_NaN(),0,1},{0,0,1,1},{0,1,0,1});f.Assemble();Reject(f,fm::Status::InvalidMesh);}
    {Fixture f;f.state.transforms[256][0]=1e10f;f.Set(1,{1e30f,0,0,1},{0,0,1,1},{1,0,0,1});f.Assemble();Reject(f,fm::Status::InvalidTransform);}
    {Fixture f;fm::Mesh mesh;CHECK(f.Build(mesh,proof,{2,3})==fm::Status::BudgetExceeded);Empty(mesh);}
    {Fixture f;fm::Mesh mesh;CHECK(f.Build(mesh,proof,{3,2})==fm::Status::BudgetExceeded);Empty(mesh);}
    {Fixture f;const std::array<uint32_t,4> indices{0,1,2,0};f.draw.index_src=indices.data();f.draw.index16=false;
     f.draw.index_count=uint32_t(indices.size());Reject(f,fm::Status::InvalidMesh);}
    {Fixture f;f.draw.draw_count=0;f.draw.index_count=0;Reject(f,fm::Status::InvalidMesh);}
}
void Indices() {
    for(bool shorts:{false,true}) {
        Fixture f;std::array<uint32_t,3> wide{101,102,103};std::array<uint16_t,3> narrow{101,102,103};
        std::span<const uint8_t> bytes{shorts?reinterpret_cast<const uint8_t*>(narrow.data()):reinterpret_cast<const uint8_t*>(wide.data()),shorts?sizeof(narrow):sizeof(wide)};
        f.draw.index_count=3;f.draw.index16=shorts;f.draw.draw_count=0;f.draw.base_vertex=-101;
        fm::Mesh mesh;CHECK(fm::Build(f.state,f.draw,assignment,proof,target,bytes,mesh)==fm::Status::Ready);
        CHECK(mesh.indices==std::vector<uint32_t>({0,1,2}));
        CHECK(fm::Build(f.state,f.draw,assignment,proof,target,bytes.first(bytes.size()-1),mesh)==fm::Status::InvalidIndex);Empty(mesh);
        f.draw.base_vertex=-102;CHECK(fm::Build(f.state,f.draw,assignment,proof,target,bytes,mesh)==fm::Status::InvalidIndex);Empty(mesh);
        f.draw.base_vertex=std::numeric_limits<int32_t>::max();
        CHECK(fm::Build(f.state,f.draw,assignment,proof,target,bytes,mesh)==fm::Status::InvalidIndex);Empty(mesh);
    }
}
// Strips expand with the rasterizer's parity rule; index-degenerate joins drop without shifting that parity.
void Strips() {
    {
        Fixture f;f.Set(3,{1,1,0,1},{0,0,1,1},{1,1,0,1});
        CHECK(gpu::Assemble(f.state,6,0,4,nullptr,f.draw));
        fm::Mesh mesh;CHECK(f.Build(mesh)==fm::Status::Ready);
        if(f.draw.strip) {
            CHECK(mesh.indices==std::vector<uint32_t>({0,1,2, 2,1,3}));
            CHECK(f.Build(mesh,proof,{4,5})==fm::Status::BudgetExceeded);Empty(mesh);
        }else CHECK(mesh.indices.size()==6); // The converted path already rebuilt the strip as a list.
    }
    {
        Fixture f;f.Set(3,{1,1,0,1},{0,0,1,1},{1,1,0,1});
        CHECK(gpu::Assemble(f.state,6,0,4,nullptr,f.draw));
        // Join 0,1,2 to 2,1,3 through two degenerate triangles: the fourth strip triangle is odd and swaps.
        const std::array<uint32_t,6> joined{0,1,2,2,1,3};
        f.draw.strip=true;
        f.draw.index_src=joined.data();f.draw.index16=false;f.draw.index_count=6;f.draw.draw_count=0;f.draw.base_vertex=0;
        fm::Mesh mesh;CHECK(f.Build(mesh)==fm::Status::Ready);
        CHECK(mesh.indices==std::vector<uint32_t>({0,1,2, 1,2,3}));
        const std::array<uint32_t,4> flat{0,0,1,1};
        f.draw.index_src=flat.data();f.draw.index_count=4;Reject(f,fm::Status::InvalidMesh);
        f.draw.index_count=2;Reject(f,fm::Status::InvalidMesh);
    }
}
// The world shape: XYZ+DIFFUSE+TEX2 with no normal. Face normals and the actual diffuse are packed.
void FaceNormalsAndDiffuse() {
    constexpr fm::FixedProof face{fm::DrawKind::ObjectSpaceFixedFunction,false,true,true,true};
    const auto world=[](Fixture& f,const std::array<std::array<float,3>,4>& p,const std::array<uint32_t,4>& diffuse) {
        f.state.vs_handle=0x2|0x40|(2u<<8);f.state.vs.reset();f.state.streams[0].stride=32;
        for(unsigned i=0;i<4;++i) {
            uint8_t* v=fixture::memory.data()+f.data[0]+i*32;
            for(unsigned k=0;k<3;++k)Write(v+k*4,p[i][k]);
            Write(v+12,diffuse[i]);Write(v+16,.25f);Write(v+20,.25f);Write(v+24,p[i][0]);Write(v+28,p[i][1]+p[i][2]);
        }
    };
    const std::array<std::array<float,3>,4> square{{{0,0,0},{1,0,0},{0,1,0},{1,1,0}}};
    {
        Fixture f;world(f,square,{0xFF804020,0xFFFFFFFF,0xFF000000,0xFF102030});f.Assemble();
        fm::Mesh mesh;const auto status=f.Build(mesh,face);
        if(status!=fm::Status::Ready)std::cerr<<"Face fixture: "<<fm::StatusText(status)<<'\n';
        CHECK(status==fm::Status::Ready&&mesh.normals==pp::NormalSource::Face&&mesh.geometry.options[1]==1);
        CHECK(mesh.vertex_diffuse_alpha_one);
        for(unsigned i=0;i<3;++i) {
            const auto& n=mesh.vertices[i].model_normal;CHECK(n.x==0&&n.y==0&&n.z==1);
            float d[4];gpu::Convert(0x40,fixture::memory.data()+f.data[0]+i*32+12,d);
            for(unsigned k=0;k<4;++k)CHECK(mesh.vertices[i].color[k]==d[k]);
        }
        CHECK(mesh.vertices[0].color[0]==float(0x80)/255&&mesh.vertices[0].color[2]==float(0x20)/255);
        // The authored proof needs a register-2 normal this stream does not have; both claims are no proof.
        Reject(f,fm::Status::MissingInput);
        fm::FixedProof both=face;both.authored_normals=true;CHECK(f.Build(mesh,both)==fm::Status::UnprovenDraw);Empty(mesh);
        fm::FixedProof neither=face;neither.face_normals=false;CHECK(f.Build(mesh,neither)==fm::Status::UnprovenDraw);Empty(mesh);
        // A stage-1 TFACTOR tint multiplies the packed RGB only; out-of-range factors are no proof.
        fm::FixedProof tinted=face;tinted.diffuse_factor={.5f,1,0,1};
        CHECK(f.Build(mesh,tinted)==fm::Status::Ready);
        CHECK(mesh.vertices[0].color[0]==float(0x80)/255*.5f&&mesh.vertices[0].color[1]==float(0x40)/255&&
              mesh.vertices[0].color[2]==0&&mesh.vertices[0].color[3]==1);
        // Alpha: the factor alpha, times the vertex alpha only where the cascade reads it.
        tinted.diffuse_factor={1,1,1,0};CHECK(f.Build(mesh,tinted)==fm::Status::Ready&&mesh.vertices[0].color[3]==0);
        tinted.diffuse_factor={1,1,1,1};
        tinted.diffuse_factor={1.5f,1,1,1};CHECK(f.Build(mesh,tinted)==fm::Status::UnprovenDraw);Empty(mesh);
        world(f,square,{0x80804020,0xFFFFFFFF,0xFF000000,0xFF102030});f.Assemble();
        CHECK(f.Build(mesh,face)==fm::Status::Ready&&!mesh.vertex_diffuse_alpha_one);
    }
    {
        // A crease: vertex normals average across it, but neither face needs the other's normal to agree.
        Fixture f;world(f,{{{0,0,0},{1,0,0},{0,1,0},{1,0,-1}}},{~0u,~0u,~0u,~0u});
        const std::array<uint32_t,6> crease{0,1,2, 0,3,1};
        CHECK(gpu::Assemble(f.state,5,0,6,nullptr,f.draw));
        f.draw.index_src=crease.data();f.draw.index16=false;f.draw.index_count=6;f.draw.draw_count=0;f.draw.base_vertex=0;
        fm::Mesh mesh;const auto status=f.Build(mesh,face);
        if(status!=fm::Status::Ready)std::cerr<<"Crease fixture: "<<fm::StatusText(status)<<'\n';
        CHECK(status==fm::Status::Ready&&mesh.indices.size()==6);
        const auto& shared=mesh.vertices[0].model_normal;
        CHECK(std::abs(shared.x*shared.x+shared.y*shared.y+shared.z*shared.z-1)<1e-5f&&shared.y<0&&shared.z>0);
        // A zero-area triangle covers no pixel in the original: it is dropped, not a reason to reject the draw.
        const std::array<uint32_t,6> sliver{0,1,2, 0,0,1};
        f.draw.index_src=sliver.data();CHECK(f.Build(mesh,face)==fm::Status::Ready&&mesh.indices==std::vector<uint32_t>({0,1,2}));
    }
    {
        // A face claim on a stream that HAS an authored normal would discard it.
        Fixture f;fm::Mesh mesh;fm::FixedProof claim=proof;claim.authored_normals=false;claim.face_normals=true;
        CHECK(f.Build(mesh,claim)==fm::Status::UnprovenDraw);Empty(mesh);
        CHECK(f.Build(mesh)==fm::Status::Ready&&mesh.normals==pp::NormalSource::Authored&&mesh.geometry.options[1]==0);
        CHECK(mesh.vertices[0].color==(std::array<float,4>{1,1,1,1})); // No diffuse requested: white.
    }
}
// A cached mesh refreshed for a later draw: new transform constants, the same vertices, and every state check again.
void Refreshed() {
    Fixture f;fm::Mesh mesh;CHECK(f.Build(mesh)==fm::Status::Ready);
    const auto vertices=mesh.vertices;const auto indices=mesh.indices;
    f.state.transforms[256][12]=5;  // the object moved
    fm::Mesh rebuilt;CHECK(f.Build(rebuilt)==fm::Status::Ready);
    CHECK(fm::Refresh(f.state,f.draw,assignment,proof,target,mesh)==fm::Status::Ready);
    CHECK(mesh.geometry.model_to_clip.values==rebuilt.geometry.model_to_clip.values);
    CHECK(mesh.geometry.model_to_view.values==rebuilt.geometry.model_to_view.values);
    CHECK(mesh.vertices.size()==vertices.size()&&mesh.indices==indices);
    for(size_t i=0;i<vertices.size();++i)CHECK(std::memcmp(&mesh.vertices[i],&vertices[i],sizeof(vertices[i]))==0);
    fm::Mesh copy=mesh;f.state.rs[gpu::kRsVertexBlend]=1;
    CHECK(fm::Refresh(f.state,f.draw,assignment,proof,target,copy)==fm::Status::UnsupportedDraw);Empty(copy);
    f.state.rs[gpu::kRsVertexBlend]=0;copy=mesh;f.state.transforms[256][0]=std::numeric_limits<float>::infinity();
    CHECK(fm::Refresh(f.state,f.draw,assignment,proof,target,copy)==fm::Status::InvalidTransform);Empty(copy);
    fm::Mesh never;CHECK(fm::Refresh(f.state,f.draw,assignment,proof,target,never)==fm::Status::InvalidMesh);
}
} // namespace

int main() {
    static_assert(sizeof(pp::Vertex)==48);
    Formats();ExactConstants();Fvf();Guards();Indices();Strips();FaceNormalsAndDiffuse();Refreshed();
    std::cout<<"Fixed mesh raw/converted source decoding, exact clip order, bounds and explicit proof gates passed\n";
}
