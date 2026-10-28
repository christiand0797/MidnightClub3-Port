#include "gpu/pbr_geometry.h"

#include <cassert>
#include <cmath>
#include <iostream>
#include <limits>
#include <string>
#include <utility>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3dcompiler.h>
#include <d3d11shader.h>
#include <wrl/client.h>
#endif

using gpu::pbr::Vec3;
using namespace gpu::pbr::geometry;
namespace {
constexpr float kTolerance=0.00002f;
void Near(float a,float b,float tolerance=kTolerance) { assert(std::isfinite(a)&&std::isfinite(b)&&std::abs(a-b)<=tolerance); }
void Near(Vec3 a,Vec3 b,float tolerance=kTolerance) { Near(a.x,b.x,tolerance);Near(a.y,b.y,tolerance);Near(a.z,b.z,tolerance); }
float Dot(Vec3 a,Vec3 b) { return a.x*b.x+a.y*b.y+a.z*b.z; }
Vec3 Cross(Vec3 a,Vec3 b) { return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x}; }
Vec3 Scale(Vec3 a,float s) { return {a.x*s,a.y*s,a.z*s}; }
Vec3 Direction(Vec3 p,const RowMatrix4& m) {
    const auto& a=m.values;
    return {p.x*a[0]+p.y*a[4]+p.z*a[8],p.x*a[1]+p.y*a[5]+p.z*a[9],p.x*a[2]+p.y*a[6]+p.z*a[10]};
}
RowMatrix4 Scaling(float x,float y,float z) { auto m=RowMatrix4::Identity();m.values[0]=x;m.values[5]=y;m.values[10]=z;return m; }
void Empty(const CotangentFrame& frame) {
    assert(!frame.ready&&frame.uv_orientation==0&&frame.handedness==0);
    Near(frame.tangent,{});Near(frame.bitangent,{});Near(frame.normal,{});
}
void AffineChecks() {
    AffineBridge bridge;Vec3 result{99,99,99};
    assert(ViewPosition(bridge,{1,2,3},result)==Status::NotReady);Near(result,{});
    assert(ViewNormal(bridge,{0,0,1},result)==Status::NotReady);Near(result,{});
    assert(BuildAffineBridge(RowMatrix4::Identity(),RowMatrix4::Identity(),bridge)==Status::Ready);
    assert(bridge.ready&&!bridge.mirrored);
    assert(ViewPosition(bridge,{1,2,3},result)==Status::Ready);Near(result,{1,2,3});
    assert(ViewNormal(bridge,{0,0,5},result)==Status::Ready);Near(result,{0,0,1});

    // Noncommuting transforms with explicit expectations expose row/column and world/view order errors.
    auto model=RowMatrix4::Identity();model.values[12]=10;model.values[13]=-4;model.values[14]=3;
    RowMatrix4 view{{0,1,0,0, -1,0,0,0, 0,0,1,0, 1,2,-8,1}};
    assert(BuildAffineBridge(model,view,bridge)==Status::Ready);
    assert(ViewPosition(bridge,{2,3,4},result)==Status::Ready);Near(result,{2,14,-1});
    assert(ViewNormal(bridge,{1,0,0},result)==Status::Ready);Near(result,{0,1,0});

    // Inverse transpose, not the position transform: the normal stays perpendicular to transformed tangents.
    model=Scaling(2,4,1);
    assert(BuildAffineBridge(model,RowMatrix4::Identity(),bridge)==Status::Ready);
    assert(ViewNormal(bridge,{1,1,0},result)==Status::Ready);
    const float fifth=1/std::sqrt(5.0f);Near(result,{2*fifth,fifth,0});
    Near(Dot(result,Direction({1,-1,0},bridge.model_to_view)),0);
    model.values[1]=0.7f;model.values[6]=0.2f;  // Shear plus nonuniform scale.
    assert(BuildAffineBridge(model,view,bridge)==Status::Ready);
    assert(ViewNormal(bridge,{1,1,1},result)==Status::Ready);
    Near(Dot(result,Direction({1,-1,0},bridge.model_to_view)),0);
    Near(Dot(result,Direction({1,0,-1},bridge.model_to_view)),0);
    Near(Dot(result,result),1);

    // Mirroring is exposed, not silently repaired by flipping an authored normal or raster winding.
    assert(BuildAffineBridge(Scaling(-2,4,1),RowMatrix4::Identity(),bridge)==Status::Ready);
    assert(bridge.mirrored&&ViewNormal(bridge,{1,1,0},result)==Status::Ready);Near(result,{-2*fifth,fifth,0});
    assert(ViewNormal(bridge,{0,0,1},result)==Status::Ready);Near(result,{0,0,1});
    assert(BuildAffineBridge(Scaling(-2,4,1),Scaling(1,-1,1),bridge)==Status::Ready&&!bridge.mirrored);

    // Units do not define degeneracy: small and large well-conditioned transforms are both supported.
    for(float scale:{1e-10f,1e10f}) {
        assert(BuildAffineBridge(Scaling(scale,scale,scale),RowMatrix4::Identity(),bridge)==Status::Ready);
        assert(ViewNormal(bridge,{0,0,10},result)==Status::Ready);Near(result,{0,0,1});
        assert(ViewPosition(bridge,{1,2,3},result)==Status::Ready);
        Near(result.x/scale,1);Near(result.y/scale,2);Near(result.z/scale,3);
    }
    // Relative angular singularity catches near-parallel rows, without an arbitrary metre threshold.
    model=RowMatrix4::Identity();model.values[4]=1;model.values[5]=1e-8f;
    assert(BuildAffineBridge(model,RowMatrix4::Identity(),bridge)==Status::SingularTransform&&!bridge.ready);
    assert(BuildAffineBridge(Scaling(1,0,1),RowMatrix4::Identity(),bridge)==Status::SingularTransform&&!bridge.ready);
    assert(BuildAffineBridge(Scaling(1e-39f,1,1),RowMatrix4::Identity(),bridge)==Status::NonFinite&&!bridge.ready);

    // No perspective/homogeneous normalization or guessed camera extraction is accepted here.
    for(unsigned index:{3u,7u,11u,15u}) {
        model=RowMatrix4::Identity();model.values[index]=index==15?2.0f:1e-9f;
        assert(BuildAffineBridge(model,RowMatrix4::Identity(),bridge)==Status::NonAffine&&!bridge.ready);
        assert(BuildAffineBridge(RowMatrix4::Identity(),model,bridge)==Status::NonAffine&&!bridge.ready);
    }
    const float inf=std::numeric_limits<float>::infinity(),nan=std::numeric_limits<float>::quiet_NaN();
    for(float invalid:{inf,-inf,nan}) for(unsigned index:{0u,5u,12u,15u}) {
        model=RowMatrix4::Identity();model.values[index]=invalid;
        assert(BuildAffineBridge(model,RowMatrix4::Identity(),bridge)==Status::NonFinite&&!bridge.ready);
        assert(BuildAffineBridge(RowMatrix4::Identity(),model,bridge)==Status::NonFinite&&!bridge.ready);
    }
    assert(BuildAffineBridge(Scaling(std::numeric_limits<float>::max(),2,1),Scaling(2,1,1),bridge)==Status::NonFinite);
    assert(BuildAffineBridge(Scaling(2,2,2),RowMatrix4::Identity(),bridge)==Status::Ready);
    result={99,99,99};assert(ViewPosition(bridge,{std::numeric_limits<float>::max(),0,0},result)==Status::NonFinite);Near(result,{});
    for(float invalid:{inf,-inf,nan}) {
        assert(ViewPosition(bridge,{invalid,0,0},result)==Status::NonFinite);Near(result,{});
        assert(ViewNormal(bridge,{0,invalid,1},result)==Status::NonFinite);Near(result,{});
    }
    assert(ViewNormal(bridge,{},result)==Status::DegenerateNormal);Near(result,{});
}

void DerivativeChecks() {
    Vec3 normal;
    assert(FaceNormal({2,0,0},{0,5,0},false,normal)==Status::Ready);Near(normal,{0,0,1});
    assert(FaceNormal({2,0,0},{0,5,0},true,normal)==Status::Ready);Near(normal,{0,0,-1});
    for(float scale:{1e-20f,1.0f,1e20f}) {
        assert(FaceNormal({2*scale,0,0},{0,5*scale,0},false,normal)==Status::Ready);Near(normal,{0,0,1});
    }
    for(auto dy:{Vec3{2,0,0},Vec3{1,1e-8f,0},Vec3{}}) {
        normal={99,99,99};assert(FaceNormal({1,0,0},dy,false,normal)==Status::DegeneratePosition);Near(normal,{});
    }
    const float inf=std::numeric_limits<float>::infinity(),nan=std::numeric_limits<float>::quiet_NaN();
    for(float invalid:{inf,-inf,nan}) {
        assert(FaceNormal({invalid,0,0},{0,1,0},false,normal)==Status::NonFinite);Near(normal,{});
    }

    CotangentFrame frame;
    auto build=[&](Vec3 dx,Vec3 dy,Vec2 ux,Vec2 uy,Vec3 n={0,0,1}) {
        return BuildCotangentFrame(dx,dy,ux,uy,n,frame);
    };
    assert(build({1,0,0},{0,1,0},{1,0},{0,1})==Status::Ready&&frame.ready);
    Near(frame.tangent,{1,0,0});Near(frame.bitangent,{0,1,0});Near(frame.normal,{0,0,1});
    assert(frame.uv_orientation==1&&frame.handedness==1);
    assert(build({1,0,0},{0,1,0},{-1,0},{0,1})==Status::Ready);
    Near(frame.tangent,{-1,0,0});Near(frame.bitangent,{0,1,0});assert(frame.uv_orientation==-1&&frame.handedness==-1);
    assert(build({1,0,0},{0,1,0},{1,0},{0,-1})==Status::Ready);
    Near(frame.tangent,{1,0,0});Near(frame.bitangent,{0,-1,0});assert(frame.uv_orientation==-1&&frame.handedness==-1);
    // Geometry mirroring and UV mirroring are distinct; either reverses handedness and both cancel.
    assert(build({-1,0,0},{0,1,0},{1,0},{0,1})==Status::Ready);
    Near(frame.tangent,{-1,0,0});Near(frame.bitangent,{0,1,0});assert(frame.uv_orientation==1&&frame.handedness==-1);
    assert(build({-1,0,0},{0,1,0},{-1,0},{0,1})==Status::Ready);
    Near(frame.tangent,{1,0,0});Near(frame.bitangent,{0,1,0});assert(frame.uv_orientation==-1&&frame.handedness==1);
    // Supplied normal reversal never silently reverses the increasing-U/V gradient directions.
    assert(build({1,0,0},{0,1,0},{1,0},{0,1},{0,0,-2})==Status::Ready);
    Near(frame.tangent,{1,0,0});Near(frame.bitangent,{0,1,0});Near(frame.normal,{0,0,-1});assert(frame.handedness==-1);
    // Skew UVs produce a genuinely nonorthogonal cotangent frame, preserving a common normalization.
    assert(build({1,0,0},{0,1,0},{1,0},{0.5f,1},{0,0,9})==Status::Ready);
    const float scale=1/std::sqrt(1.25f);
    Near(frame.tangent,{scale,0.5f*scale,0});Near(frame.bitangent,{0,scale,0});Near(Dot(frame.tangent,frame.bitangent),0.4f);
    Near(Dot(frame.tangent,frame.normal),0);Near(Dot(frame.bitangent,frame.normal),0);
    // Global derivative units cancel without overflowing/underflowing intermediate squared lengths.
    const CotangentFrame expected=frame;
    for(float p_scale:{1e-20f,1.0f,1e20f}) for(float uv_scale:{1e-20f,1.0f,1e20f}) {
        assert(build({p_scale,0,0},{0,p_scale,0},{uv_scale,0},{0.5f*uv_scale,uv_scale})==Status::Ready);
        Near(frame.tangent,expected.tangent);Near(frame.bitangent,expected.bitangent);Near(frame.normal,expected.normal);
    }
    for(auto pair:{std::pair{Vec2{},Vec2{}},std::pair{Vec2{1,0},Vec2{2,0}},
                   std::pair{Vec2{1,0},Vec2{1,1e-8f}},std::pair{Vec2{},Vec2{0,1}}}) {
        assert(build({1,0,0},{0,1,0},pair.first,pair.second)==Status::DegenerateUv);Empty(frame);
    }
    assert(build({1,0,0},{2,0,0},{1,0},{0,1})==Status::DegeneratePosition);Empty(frame);
    assert(build({1,0,0},{0,1,0},{1,0},{0,1},{})==Status::DegenerateNormal);Empty(frame);
    assert(build({1,0,0},{0,1,0},{1,0},{0,1},{1,0,0})==Status::DegenerateNormal);Empty(frame);
    for(float invalid:{inf,-inf,nan}) {
        assert(build({invalid,0,0},{0,1,0},{1,0},{0,1})==Status::NonFinite);Empty(frame);
        assert(build({1,0,0},{0,1,0},{invalid,0},{0,1})==Status::NonFinite);Empty(frame);
        assert(build({1,0,0},{0,1,0},{1,0},{0,1},{0,invalid,1})==Status::NonFinite);Empty(frame);
    }
}

void CompileHlsl() {
#ifdef _WIN32
    using Microsoft::WRL::ComPtr;
    const std::string helpers(GeometryHlsl());
    const std::string vertex=helpers+R"HLSL(
cbuffer Geometry : register(b0) {
    row_major float4x4 ModelToView;
    row_major float4x4 NormalToView;
};
struct VOut { float4 position:SV_Position; float3 view_position:TEXCOORD0; float3 normal:TEXCOORD1; };
VOut main(float3 position:POSITION,float3 normal:NORMAL) {
    VOut result=(VOut)0;
    float3 p,n;
    bool valid=PbrGeoPosition(position,ModelToView,p)&&PbrGeoNormal(normal,NormalToView,n);
    result.position=float4(valid?p:position,1);result.view_position=p;result.normal=n;
    return result;
})HLSL";
    const std::string pixel=helpers+R"HLSL(
float4 main(float3 view_position:TEXCOORD0,float2 uv:TEXCOORD1,bool front:SV_IsFrontFace):SV_Target {
    float3 n;PbrGeoFrame frame;
    if (!PbrGeoFaceNormal(ddx(view_position),ddy(view_position),!front,n) ||
        !PbrGeoCotangentFrame(ddx(view_position),ddy(view_position),ddx(uv),ddy(uv),n,frame)) return 0;
    return float4(frame.tangent+frame.bitangent+frame.normal,frame.handedness);
})HLSL";
    for(auto [source,profile]:{std::pair{vertex,"vs_5_0"},std::pair{pixel,"ps_5_0"}}) {
        ComPtr<ID3DBlob> blob,error;
        const HRESULT status=D3DCompile(source.data(),source.size(),"pbr_geometry_synthetic",nullptr,nullptr,"main",profile,
            D3DCOMPILE_ENABLE_STRICTNESS|D3DCOMPILE_WARNINGS_ARE_ERRORS|D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&blob,&error);
        if(FAILED(status)&&error) std::cerr.write(static_cast<const char*>(error->GetBufferPointer()),error->GetBufferSize());
        assert(SUCCEEDED(status)&&blob&&blob->GetBufferSize()>0);
        if(std::string(profile)=="vs_5_0") {
            ComPtr<ID3D11ShaderReflection> reflection;
            assert(SUCCEEDED(D3DReflect(blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&reflection))));
            auto* buffer=reflection->GetConstantBufferByName("Geometry");
            D3D11_SHADER_BUFFER_DESC description{};
            assert(buffer&&SUCCEEDED(buffer->GetDesc(&description))&&description.Size==128);
            for(auto [name,offset]:{std::pair{"ModelToView",0u},std::pair{"NormalToView",64u}}) {
                auto* variable=buffer->GetVariableByName(name);
                D3D11_SHADER_VARIABLE_DESC value{};D3D11_SHADER_TYPE_DESC type{};
                assert(variable&&SUCCEEDED(variable->GetDesc(&value))&&value.StartOffset==offset&&value.Size==64);
                assert(SUCCEEDED(variable->GetType()->GetDesc(&type))&&type.Class==D3D_SVC_MATRIX_ROWS&&type.Rows==4&&type.Columns==4);
            }
        }
    }
    std::cout<<"D3DCompile synthetic VS/PS and reflected row-major128byte constants passed; no GPU execution performed\n";
#else
    assert(!GeometryHlsl().empty());
#endif
}
}  // namespace

int main() {
    AffineChecks();DerivativeChecks();CompileHlsl();
    std::cout<<"PBR geometry transform/cotangent fixtures passed\n";
}
