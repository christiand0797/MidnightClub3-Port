#include "gpu/pbr_fixed_mesh.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <new>

namespace gpu::pbr::fixed_mesh {
namespace {
using Matrix = geometry::RowMatrix4;
struct Format { DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN; uint8_t decode = 0, bytes = 0; };
// These pairs are the raw HostFormat/Convert/VsInputs contract, not arbitrary DXGI reinterpretations.
Format LegacyFormat(uint32_t type) {
    switch(type) {
    case 0x12:return {DXGI_FORMAT_R32_FLOAT,kDecodeNone,4};
    case 0x22:return {DXGI_FORMAT_R32G32_FLOAT,kDecodeNone,8};
    case 0x32:return {DXGI_FORMAT_R32G32B32_FLOAT,kDecodeNone,12};
    case 0x42:return {DXGI_FORMAT_R32G32B32A32_FLOAT,kDecodeNone,16};
    case 0x72:return {DXGI_FORMAT_R32G32B32_FLOAT,kDecodeFloat2H,12};
    case 0x40:return {DXGI_FORMAT_B8G8R8A8_UNORM,kDecodeNone,4};
    case 0x11:return {DXGI_FORMAT_R16_SNORM,kDecodeNone,2};
    case 0x21:return {DXGI_FORMAT_R16G16_SNORM,kDecodeNone,4};
    case 0x31:return {DXGI_FORMAT_R16G16B16A16_SNORM,kDecodeW1,8};
    case 0x41:return {DXGI_FORMAT_R16G16B16A16_SNORM,kDecodeNone,8};
    case 0x14:return {DXGI_FORMAT_R8_UNORM,kDecodeNone,1};
    case 0x24:return {DXGI_FORMAT_R8G8_UNORM,kDecodeNone,2};
    case 0x34:return {DXGI_FORMAT_R8G8B8A8_UNORM,kDecodeW1,4};
    case 0x44:return {DXGI_FORMAT_R8G8B8A8_UNORM,kDecodeNone,4};
    case 0x15:return {DXGI_FORMAT_R16_SINT,kDecodeInt,2};
    case 0x25:return {DXGI_FORMAT_R16G16_SINT,kDecodeInt,4};
    case 0x35:return {DXGI_FORMAT_R16G16B16A16_SINT,kDecodeIntW1,8};
    case 0x45:return {DXGI_FORMAT_R16G16B16A16_SINT,kDecodeInt,8};
    case 0x16:return {DXGI_FORMAT_R32_UINT,kDecodePacked,4};
    default:return {};
    }
}
template<class T> T Read(const uint8_t* source) { T value;std::memcpy(&value,source,sizeof(value));return value; }
Vec3 Sub(Vec3 a,Vec3 b) { return {a.x-b.x,a.y-b.y,a.z-b.z}; }
Vec3 Cross(Vec3 a,Vec3 b) { return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x}; }
bool Unit(Vec3 v,Vec3& out) {
    const double length=std::sqrt(double(v.x)*v.x+double(v.y)*v.y+double(v.z)*v.z);
    if(!(length>0)||!std::isfinite(length))return false;
    out={float(v.x/length),float(v.y/length),float(v.z/length)};return true;
}
bool Finite(const std::array<float,4>& value) {
    return std::all_of(value.begin(),value.end(),[](float f){return std::isfinite(f);});
}
bool Finite(const Matrix& value) {
    return std::all_of(value.values.begin(),value.values.end(),[](float f){return std::isfinite(f);});
}
bool Affine(const Matrix& value) {
    return Finite(value)&&value.values[3]==0&&value.values[7]==0&&value.values[11]==0&&value.values[15]==1;
}
// Deliberately float32, same expression and multiplication order as FillTransformConstants::MatMul.
Matrix Multiply(const Matrix& x,const Matrix& y) {
    Matrix out;
    for(unsigned r=0;r<4;++r)for(unsigned c=0;c<4;++c)
        out.values[r*4+c]=x.values[r*4]*y.values[c]+x.values[r*4+1]*y.values[4+c]+
                         x.values[r*4+2]*y.values[8+c]+x.values[r*4+3]*y.values[12+c];
    return out;
}
std::array<float,4> Transform(Vec3 p,const Matrix& m) {
    std::array<float,4> result;
    for(unsigned c=0;c<4;++c)result[c]=p.x*m.values[c]+p.y*m.values[4+c]+p.z*m.values[8+c]+m.values[12+c];
    return result;
}
bool RigidView(const Matrix& m) {
    if(!Affine(m))return false;
    for(unsigned r=0;r<3;++r)for(unsigned c=r;c<3;++c) {
        double dot=0;for(unsigned k=0;k<3;++k)dot+=double(m.values[r*4+k])*m.values[c*4+k];
        if(std::abs(dot-(r==c?1.0:0.0))>2e-5)return false;
    }
    return true;
}
Status Constants(const State& state,const RasterTarget& target,pipeline::GeometryConstants& out,bool& units) {
    const Matrix world{state.transforms[256]},view{state.transforms[2]},projection{state.transforms[3]};
    if(!Affine(world)||!Affine(view)||!Finite(projection))return Status::InvalidTransform;
    const auto& v=state.viewport;
    if(!target.guest_width||!target.guest_height||!v.w||!v.h||
       uint64_t(v.x)+v.w>target.guest_width||uint64_t(v.y)+v.h>target.guest_height||
       !std::isfinite(v.min_z)||!std::isfinite(v.max_z)||v.min_z<0||v.max_z>1||v.min_z>=v.max_z)
        return Status::InvalidTransform;
    const float gw=float(target.guest_width),gh=float(target.guest_height);
    const float sx=float(v.w)/gw,sy=float(v.h)/gh;
    const float ox=(2.0f*v.x+v.w)/gw-1,oy=1-(2.0f*v.y+v.h)/gh;
    const Matrix viewport{{sx,0,0,0, 0,sy,0,0, 0,0,v.max_z-v.min_z,0, ox,oy,v.min_z,1}};
    const Matrix model_view=Multiply(world,view);
    const Matrix exact_model_clip=Multiply(Multiply(model_view,projection),viewport);
    const Matrix view_clip=Multiply(projection,viewport);
    geometry::AffineBridge bridge;
    // Reuse the geometric inverse transpose, applied to the same float32 model-view used by the original path.
    if(geometry::BuildAffineBridge(model_view,Matrix::Identity(),bridge)!=geometry::Status::Ready||
       pipeline::BuildGeometry(bridge,exact_model_clip,view_clip,target.width,target.height,
           target.guest_width,target.guest_height,target.tessellation_policy,target.target_edge_pixels,out)!=pipeline::Status::Ready)
        return Status::InvalidTransform;
    units=RigidView(view);return Status::Ready;
}
struct Attribute {
    const VertexBuffer* buffer = nullptr;
    uint16_t offset = 0;
    uint32_t type = 0;
    uint8_t bytes = 0;
    bool converted = false;
};
Status Bind(const State& state,const VertexShader& layout,const Assembled& draw,unsigned reg,bool converted,Attribute& result) {
    if(!((draw.regs>>reg)&1))return Status::MissingInput;
    const auto& declared=layout.inputs[reg];
    if(declared.stream<0||declared.stream>=16||!declared.type||declared.type==0x02)return Status::MissingInput;
    const auto format=LegacyFormat(declared.type);if(!format.bytes)return Status::UnsupportedFormat;
    const auto& element=draw.elements[reg];
    const unsigned decode=unsigned(draw.decode>>(reg*4))&15;
    if(element.buffer>=draw.buffer_count||element.buffer>=draw.buffers.size())return Status::InvalidBuffer;
    const auto& buffer=draw.buffers[element.buffer];
    if(!buffer.src||!buffer.stride)return Status::InvalidBuffer;
    if(converted) {
        const uint32_t before=std::popcount(uint16_t(draw.regs&((1u<<reg)-1)));
        if(element.buffer||element.format!=DXGI_FORMAT_R32G32B32A32_FLOAT||decode||element.offset!=before*16||
           buffer.stride!=uint32_t(std::popcount(draw.regs))*16||draw.buffer_count!=1)
            return Status::UnsupportedFormat;
        if(buffer.src!=reinterpret_cast<const uint8_t*>(draw.vertices.data())||draw.vertices.size()*sizeof(float)!=buffer.bytes)
            return Status::InvalidBuffer;
    }else {
        const uint32_t stride=state.streams[size_t(declared.stream)].stride?
            state.streams[size_t(declared.stream)].stride:layout.stream_stride[size_t(declared.stream)];
        if(element.format!=format.format||decode!=format.decode)return Status::UnsupportedFormat;
        if(element.offset!=declared.offset||buffer.stride!=stride)return Status::InvalidBuffer;
    }
    const uint32_t bytes=converted?16:format.bytes;
    if(uint64_t(draw.vertex_count-1)*buffer.stride+element.offset+bytes>buffer.bytes)return Status::InvalidBuffer;
    result={&buffer,element.offset,declared.type,uint8_t(bytes),converted};return Status::Ready;
}
std::array<float,4> Decode(const Attribute& attribute,uint32_t index) {
    const uint8_t* p=attribute.buffer->src+size_t(index)*attribute.buffer->stride+attribute.offset;
    std::array<float,4> value{0,0,0,1};
    if(attribute.converted){std::memcpy(value.data(),p,16);return value;}
    const uint32_t type=attribute.type,n=std::min((type>>4)&15,4u);
    if(type==0x72){value={Read<float>(p),Read<float>(p+4),0,Read<float>(p+8)};return value;}
    switch(type&15) {
    case 0:value={p[2]/255.0f,p[1]/255.0f,p[0]/255.0f,p[3]/255.0f};break;
    case 1:for(unsigned i=0;i<n;++i)value[i]=std::max(Read<int16_t>(p+i*2)/32767.0f,-1.0f);break;
    case 2:for(unsigned i=0;i<n;++i)value[i]=Read<float>(p+i*4);break;
    case 4:for(unsigned i=0;i<n;++i)value[i]=p[i]/255.0f;break;
    case 5:for(unsigned i=0;i<n;++i)value[i]=float(Read<int16_t>(p+i*2));break;
    case 6: {
        const uint32_t packed=Read<uint32_t>(p);
        const int32_t x=int32_t(packed<<21)>>21,y=int32_t(packed<<10)>>21,z=int32_t(packed)>>22;
        value={std::max(x/1023.0f,-1.0f),std::max(y/1023.0f,-1.0f),std::max(z/511.0f,-1.0f),1};break;
    }
    default:break; // Bind rejects every unsupported legacy type before this point.
    }
    return value;
}
// The draw-state half of the proof, which Refresh repeats for a cached mesh.
Status Gate(const State& state,const Assembled& draw,const Assignment& assignment,const FixedProof& proof) {
    if(proof.kind!=DrawKind::ObjectSpaceFixedFunction||proof.authored_normals==proof.face_normals||!proof.assigned_uv)
        return Status::UnprovenDraw;
    for(float f:proof.diffuse_factor)if(!(f>=0&&f<=1))return Status::UnprovenDraw;
    if(!controls::ValidAssignment(assignment))return Status::UnprovenDraw;
    if(draw.pretransformed||draw.topology!=Topology::Triangles||(state.rs[kRsVertexBlend]&7)||
       ((state.vs_handle&1)&&(!state.vs||!state.vs->program.empty()))||
       (!(state.vs_handle&1)&&(state.vs_handle&0xE)!=0x2))return Status::UnsupportedDraw;
    const uint32_t coordinates=state.tss[assignment.stage][kTssTexCoordIndex];
    if(coordinates!=assignment.uv_set||state.tss[assignment.stage][kTssTextureTransformFlags])return Status::UnsupportedDraw;
    if(proof.layer_uv>=4||proof.layer_uv<-1)return Status::UnprovenDraw;
    if(proof.layer_uv>=0&&(state.tss[1][kTssTexCoordIndex]!=uint32_t(proof.layer_uv)||state.tss[1][kTssTextureTransformFlags]))
        return Status::UnsupportedDraw;
    return Status::Ready;
}
// The actual float32 shader operations and perspective/far-edge coordinates must stay finite.
bool Projectable(Vec3 position,const pipeline::GeometryConstants& g,const RasterTarget& target) {
    const auto vp=Transform(position,g.model_to_view);
    const auto cp=Transform(position,g.model_to_clip);
    if(!Finite(vp)||vp[3]!=1||!Finite(cp)||cp[3]==0)return false;
    for(unsigned c=0;c<3;++c)if(!std::isfinite(cp[c]/cp[3]))return false;
    return std::isfinite((cp[0]/cp[3]+1)*0.5f*target.guest_width)&&std::isfinite((-cp[1]/cp[3]+1)*0.5f*target.guest_height);
}
Status BuildImpl(const State& state,const Assembled& draw,const Assignment& assignment,const FixedProof& proof,
                 const RasterTarget& target,std::span<const uint8_t> index_bytes,Mesh& out,Budget budget,std::string* detail) {
    if(const auto gate=Gate(state,draw,assignment,proof);gate!=Status::Ready)return gate;
    if(!budget.vertices||!budget.indices||budget.vertices>pipeline::kMaxMeshVertices||budget.indices>pipeline::kMaxMeshIndices||
       !draw.vertex_count||draw.vertex_count>budget.vertices||draw.index_count>budget.indices||draw.draw_count>budget.indices)
        return Status::BudgetExceeded;
    if(!draw.buffer_count||draw.buffer_count>draw.buffers.size())return Status::InvalidBuffer;
    const uint16_t required=uint16_t((1u<<0)|(proof.authored_normals?1u<<2:0)|(proof.vertex_diffuse?1u<<3:0)|
                                     (1u<<(9+assignment.uv_set))|(proof.layer_uv>=0?1u<<(9+proof.layer_uv):0u));
    if((draw.regs&required)!=required)return Status::MissingInput;
    // A face-normal claim on a stream that HAS a normal would silently discard the authored one.
    if(proof.face_normals&&((draw.regs>>2)&1))return Status::UnprovenDraw;
    const uint32_t count=draw.index_count?draw.index_count:draw.draw_count;
    if(count<3||(!draw.strip&&count%3))return Status::InvalidMesh;
    // A strip of n indices expands to at most n-2 triangles; fans arrive here already rebuilt as lists.
    if(draw.strip&&uint64_t(count-2)*3>budget.indices)return Status::BudgetExceeded;
    if(draw.index_count) {
        if(index_bytes.size()!=uint64_t(draw.index_count)*(draw.index16?2:4))return Status::InvalidIndex;
    }else if(!index_bytes.empty()||draw.draw_count>draw.vertex_count||draw.base_vertex)return Status::InvalidIndex;
    VertexShader fvf;
    if(!(state.vs_handle&1))FvfInputs(state.vs_handle,fvf);
    const auto& layout=(state.vs_handle&1)?*state.vs:fvf;
    Attribute position,normal,uv,diffuse,layer;const bool converted=!draw.vertices.empty();
    for(auto [reg,attribute]:{std::pair{0u,&position},std::pair{2u,&normal},std::pair{3u,&diffuse},std::pair{9u+assignment.uv_set,&uv}}) {
        if((reg==2&&!proof.authored_normals)||(reg==3&&!proof.vertex_diffuse))continue;
        const auto status=Bind(state,layout,draw,reg,converted,*attribute);if(status!=Status::Ready)return status;
    }
    if(proof.layer_uv>=0)
        if(const auto status=Bind(state,layout,draw,9u+uint32_t(proof.layer_uv),converted,layer);status!=Status::Ready)return status;
    auto status=Constants(state,target,out.geometry,out.height_units_proven);if(status!=Status::Ready)return status;
    out.vertices.resize(draw.vertex_count);
    std::vector<uint32_t> source(count);
    for(uint32_t i=0;i<draw.vertex_count;++i) {
        const auto p=Decode(position,i),t=Decode(uv,i);
        const auto n=proof.authored_normals?Decode(normal,i):std::array<float,4>{0,0,1,0};
        auto d=proof.vertex_diffuse?Decode(diffuse,i):std::array<float,4>{1,1,1,1};
        for(unsigned c=0;c<3;++c)d[c]*=proof.diffuse_factor[c];
        d[3]=(proof.alpha_from_diffuse?d[3]:1.0f)*proof.diffuse_factor[3];
        const auto t1=proof.layer_uv>=0?Decode(layer,i):std::array<float,4>{0,0,0,0};
        if(!Finite(p)||!Finite(n)||!Finite(t)||!Finite(d)||!Finite(t1))return Status::InvalidMesh;
        out.vertices[i]={{p[0],p[1],p[2]},{n[0],n[1],n[2]},{t[0],t[1]},d,{t1[0],t1[1]}};
        if(!Projectable(out.vertices[i].model_position,out.geometry,target))return Status::InvalidTransform;
    }
    for(uint32_t i=0;i<count;++i) {
        const uint32_t raw=draw.index_count?(draw.index16?Read<uint16_t>(index_bytes.data()+size_t(i)*2):
            Read<uint32_t>(index_bytes.data()+size_t(i)*4)):i;
        const int64_t index=int64_t(raw)+(draw.index_count?int64_t(draw.base_vertex):0);
        if(index<0||uint64_t(index)>=draw.vertex_count)return Status::InvalidIndex;
        source[i]=uint32_t(index);
    }
    if(!draw.strip)out.indices=std::move(source);
    else {
        // The rasterizer's strip rule: triangle i is (i, i+1, i+2), with the first two swapped for odd i so every
        // triangle keeps the strip's winding. Index-degenerate triangles (strip joins) draw nothing and are dropped;
        // the parity of the triangles after them is unchanged.
        out.indices.reserve(size_t(count-2)*3);
        for(uint32_t i=0;i+2<count;++i) {
            const uint32_t a=source[i+(i&1)],b=source[i+1-(i&1)],c=source[i+2];
            if(a==b||b==c||a==c)continue;
            out.indices.insert(out.indices.end(),{a,b,c});
        }
    }
    // A triangle with coincident or exactly collinear corners covers no pixel in the original either.
    std::vector<Vec3> faces;faces.reserve(out.indices.size()/3);
    size_t kept=0;
    for(size_t i=0;i+2<out.indices.size();i+=3) {
        const uint32_t a=out.indices[i],b=out.indices[i+1],c=out.indices[i+2];
        const Vec3 pa=out.vertices[a].model_position;
        const Vec3 area=Cross(Sub(out.vertices[b].model_position,pa),Sub(out.vertices[c].model_position,pa));
        if(area.x==0&&area.y==0&&area.z==0)continue;
        out.indices[kept]=a;out.indices[kept+1]=b;out.indices[kept+2]=c;kept+=3;faces.push_back(area);
    }
    out.indices.resize(kept);
    if(out.indices.empty())return Status::InvalidMesh;
    if(proof.face_normals) {
        // Packed normals only orient/interpolate: area-weighted per vertex, or the vertex's own face where a
        // crease cancels. The pixel shader shades with the rasterized triangle's plane.
        std::vector<Vec3> sum(out.vertices.size()),own(out.vertices.size(),Vec3{0,0,1});
        for(size_t f=0;f<faces.size();++f)for(unsigned k=0;k<3;++k) {
            Vec3& v=sum[out.indices[f*3+k]];v={v.x+faces[f].x,v.y+faces[f].y,v.z+faces[f].z};
            Unit(faces[f],own[out.indices[f*3+k]]);
        }
        for(size_t i=0;i<out.vertices.size();++i)
            if(!Unit(sum[i],out.vertices[i].model_normal))out.vertices[i].model_normal=own[i];
        out.normals=pipeline::NormalSource::Face;out.geometry.options[1]=uint32_t(pipeline::NormalSource::Face);
    }
    out.vertex_diffuse_alpha_one=proof.vertex_diffuse&&
        std::all_of(out.indices.begin(),out.indices.end(),[&](uint32_t i){return out.vertices[i].color[3]==1;});
    if(pipeline::ValidateMesh(out.vertices,out.indices,out.normals,detail)!=pipeline::Status::Ready)return Status::InvalidMesh;
    out.ready=true;return Status::Ready;
}
} // namespace

const char* StatusText(Status status) {
    switch(status) {
    case Status::Ready:return "fixed mesh ready";case Status::UnprovenDraw:return "ordinary fixed geometry not proved";
    case Status::UnsupportedDraw:return "unsupported fixed draw state";case Status::MissingInput:return "authored input missing";
    case Status::UnsupportedFormat:return "unsupported vertex format/decode contract";case Status::InvalidBuffer:return "vertex stream bounds mismatch";
    case Status::InvalidIndex:return "index bounds mismatch";case Status::InvalidTransform:return "invalid fixed transform";
    case Status::InvalidMesh:return "invalid triangle geometry/normal/UV";case Status::BudgetExceeded:return "fixed mesh budget exceeded";
    case Status::AllocationFailure:return "fixed mesh allocation failed";
    }return "unknown fixed mesh status";
}
Status Refresh(const State& state,const Assembled& draw,const Assignment& assignment,const FixedProof& proof,
               const RasterTarget& target,Mesh& mesh) {
    const auto fail=[&](Status status){mesh={};return status;};
    if(!mesh.ready||mesh.vertices.empty())return fail(Status::InvalidMesh);
    if(const auto gate=Gate(state,draw,assignment,proof);gate!=Status::Ready)return fail(gate);
    if(Constants(state,target,mesh.geometry,mesh.height_units_proven)!=Status::Ready)return fail(Status::InvalidTransform);
    mesh.geometry.options[1]=uint32_t(mesh.normals);
    for(const auto& v:mesh.vertices)if(!Projectable(v.model_position,mesh.geometry,target))return fail(Status::InvalidTransform);
    return Status::Ready;
}
Status Build(const State& state,const Assembled& draw,const Assignment& assignment,const FixedProof& proof,
             const RasterTarget& target,std::span<const uint8_t> index_bytes,Mesh& result,Budget budget,std::string* detail) {
    try {
        Mesh candidate;const auto status=BuildImpl(state,draw,assignment,proof,target,index_bytes,candidate,budget,detail);
        if(status!=Status::Ready){result={};return status;}
        result=std::move(candidate);return Status::Ready;
    }catch(const std::bad_alloc&){result={};return Status::AllocationFailure;}
}

} // namespace gpu::pbr::fixed_mesh
