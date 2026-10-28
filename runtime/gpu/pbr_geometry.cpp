#include "gpu/pbr_geometry.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace gpu::pbr::geometry {
namespace {
constexpr double kRelativeEpsilon = 1e-6;
struct D3 { double x, y, z; };
D3 D(Vec3 v) { return {v.x, v.y, v.z}; }
D3 Add(D3 a, D3 b) { return {a.x+b.x, a.y+b.y, a.z+b.z}; }
D3 Scale(D3 a, double s) { return {a.x*s, a.y*s, a.z*s}; }
double Dot(D3 a, D3 b) { return a.x*b.x + a.y*b.y + a.z*b.z; }
D3 Cross(D3 a, D3 b) { return {a.y*b.z-a.z*b.y, a.z*b.x-a.x*b.z, a.x*b.y-a.y*b.x}; }
double MaxAbs(D3 a) { return std::max({std::abs(a.x),std::abs(a.y),std::abs(a.z)}); }
bool Finite(D3 a) { return std::isfinite(a.x) && std::isfinite(a.y) && std::isfinite(a.z); }
bool Finite(Vec2 a) { return std::isfinite(a.x) && std::isfinite(a.y); }
bool Float(double v) { return std::isfinite(v) && std::abs(v)<=std::numeric_limits<float>::max(); }
bool Store(D3 v, Vec3& result) {
    if (!Float(v.x) || !Float(v.y) || !Float(v.z)) return false;
    result={float(v.x),float(v.y),float(v.z)};
    return true;
}
bool Unit(D3 v, D3& result) {
    const double scale=MaxAbs(v);
    if (!Finite(v) || !(scale>0)) return false;
    v=Scale(v,1/scale);
    const double length=std::sqrt(Dot(v,v));
    if (!(length>0) || !std::isfinite(length)) return false;
    result=Scale(v,1/length);
    return true;
}
bool MatrixFinite(const RowMatrix4& m) {
    return std::all_of(m.values.begin(),m.values.end(),[](float v){return std::isfinite(v);});
}
bool Affine(const RowMatrix4& m) {
    return m.values[3]==0 && m.values[7]==0 && m.values[11]==0 && m.values[15]==1;
}
D3 Direction(D3 v, const RowMatrix4& m, double scale=1) {
    const auto& a=m.values;
    return {v.x*(a[0]/scale)+v.y*(a[4]/scale)+v.z*(a[8]/scale),
            v.x*(a[1]/scale)+v.y*(a[5]/scale)+v.z*(a[9]/scale),
            v.x*(a[2]/scale)+v.y*(a[6]/scale)+v.z*(a[10]/scale)};
}
Status PositionDerivatives(Vec3 dx, Vec3 dy, D3& a, D3& b, D3& cross) {
    a=D(dx);b=D(dy);
    if (!Finite(a)||!Finite(b)) return Status::NonFinite;
    const double scale=std::max(MaxAbs(a),MaxAbs(b));
    if (!(scale>0)) return Status::DegeneratePosition;
    a=Scale(a,1/scale);b=Scale(b,1/scale);cross=Cross(a,b);
    const double lengths=Dot(a,a)*Dot(b,b);
    if (!(lengths>0) || Dot(cross,cross)<=kRelativeEpsilon*kRelativeEpsilon*lengths)
        return Status::DegeneratePosition;
    return Status::Ready;
}
}  // namespace

Status BuildAffineBridge(const RowMatrix4& model, const RowMatrix4& view, AffineBridge& result) {
    result={};
    if (!MatrixFinite(model)||!MatrixFinite(view)) return Status::NonFinite;
    if (!Affine(model)||!Affine(view)) return Status::NonAffine;
    AffineBridge candidate;
    for (unsigned r=0;r<4;++r) for (unsigned c=0;c<4;++c) {
        double value=0;
        for (unsigned k=0;k<4;++k) value+=double(model.values[r*4+k])*view.values[k*4+c];
        if (!Float(value)) return Status::NonFinite;
        candidate.model_to_view.values[r*4+c]=float(value);
    }
    const auto& a=candidate.model_to_view.values;
    double cofactors[3][3];
    for (unsigned r=0;r<3;++r) for (unsigned c=0;c<3;++c) {
        const unsigned r1=(r+1)%3,r2=(r+2)%3,c1=(c+1)%3,c2=(c+2)%3;
        cofactors[r][c]=double(a[r1*4+c1])*a[r2*4+c2]-double(a[r1*4+c2])*a[r2*4+c1];
    }
    const double determinant=double(a[0])*cofactors[0][0]+double(a[1])*cofactors[0][1]+double(a[2])*cofactors[0][2];
    const D3 row0{a[0],a[1],a[2]},row1{a[4],a[5],a[6]},row2{a[8],a[9],a[10]};
    const double row_lengths=std::sqrt(Dot(row0,row0))*std::sqrt(Dot(row1,row1))*std::sqrt(Dot(row2,row2));
    if (!(row_lengths>0) || std::abs(determinant)<=kRelativeEpsilon*row_lengths)
        return Status::SingularTransform;
    for (unsigned r=0;r<3;++r) for (unsigned c=0;c<3;++c) {
        const double normal=cofactors[r][c]/determinant;
        if (!Float(normal)) return Status::NonFinite;
        candidate.normal_to_view.values[r*4+c]=float(normal);
    }
    candidate.normal_to_view.values[15]=1;
    candidate.mirrored=determinant<0;
    candidate.ready=true;
    result=candidate;
    return Status::Ready;
}

Status ViewPosition(const AffineBridge& bridge, Vec3 model_position, Vec3& result) {
    result={};
    if (!bridge.ready) return Status::NotReady;
    if (!Finite(D(model_position))||!MatrixFinite(bridge.model_to_view)) return Status::NonFinite;
    if (!Affine(bridge.model_to_view)) return Status::NonAffine;
    const auto& a=bridge.model_to_view.values;
    const auto value=Add(Direction(D(model_position),bridge.model_to_view),{a[12],a[13],a[14]});
    return Store(value,result)?Status::Ready:Status::NonFinite;
}

Status ViewNormal(const AffineBridge& bridge, Vec3 model_normal, Vec3& result) {
    result={};
    if (!bridge.ready) return Status::NotReady;
    if (!Finite(D(model_normal))||!MatrixFinite(bridge.normal_to_view)) return Status::NonFinite;
    if (!Affine(bridge.normal_to_view)) return Status::NonAffine;
    D3 normal;
    if (!Unit(D(model_normal),normal)) return Status::DegenerateNormal;
    double scale=0;
    for (unsigned r=0;r<3;++r) for (unsigned c=0;c<3;++c)
        scale=std::max(scale,std::abs(double(bridge.normal_to_view.values[r*4+c])));
    if (!(scale>0)||!Unit(Direction(normal,bridge.normal_to_view,scale),normal)) return Status::DegenerateNormal;
    Store(normal,result);
    return Status::Ready;
}

Status FaceNormal(Vec3 position_dx, Vec3 position_dy, bool flip_orientation, Vec3& result) {
    result={};
    D3 a,b,cross,normal;
    const auto status=PositionDerivatives(position_dx,position_dy,a,b,cross);
    if (status!=Status::Ready) return status;
    if (!Unit(cross,normal)) return Status::DegeneratePosition;
    Store(Scale(normal,flip_orientation?-1:1),result);
    return Status::Ready;
}

Status BuildCotangentFrame(Vec3 position_dx, Vec3 position_dy, Vec2 uv_dx, Vec2 uv_dy,
                          Vec3 geometric_normal, CotangentFrame& result) {
    result={};
    if (!Finite(uv_dx)||!Finite(uv_dy)||!Finite(D(geometric_normal))) return Status::NonFinite;
    D3 a,b,area,normal;
    const auto status=PositionDerivatives(position_dx,position_dy,a,b,area);
    if (status!=Status::Ready) return status;
    if (!Unit(D(geometric_normal),normal)) return Status::DegenerateNormal;
    const double facing=Dot(area,normal);
    if (std::abs(facing)<=kRelativeEpsilon*std::sqrt(Dot(area,area))) return Status::DegenerateNormal;
    const double uv_scale=std::max({std::abs(double(uv_dx.x)),std::abs(double(uv_dx.y)),
                                    std::abs(double(uv_dy.x)),std::abs(double(uv_dy.y))});
    if (!(uv_scale>0)) return Status::DegenerateUv;
    const double ux=uv_dx.x/uv_scale,vx=uv_dx.y/uv_scale,uy=uv_dy.x/uv_scale,vy=uv_dy.y/uv_scale;
    const double determinant=ux*vy-vx*uy,lengths=(ux*ux+vx*vx)*(uy*uy+vy*vy);
    if (!(lengths>0)||std::abs(determinant)<=kRelativeEpsilon*std::sqrt(lengths)) return Status::DegenerateUv;
    // Cotangents are gradients of U/V. Dividing by the signed projected area reduces to this
    // orientation sign after common normalization; it keeps increasing-U/V directions when N flips.
    const D3 pdy_perp=Cross(b,normal),pdx_perp=Cross(normal,a);
    D3 tangent=Add(Scale(pdy_perp,ux),Scale(pdx_perp,uy));
    D3 bitangent=Add(Scale(pdy_perp,vx),Scale(pdx_perp,vy));
    const double max_length=std::max(Dot(tangent,tangent),Dot(bitangent,bitangent));
    if (!(max_length>0)||!std::isfinite(max_length)) return Status::DegenerateUv;
    const double common=(facing<0?-1:1)/std::sqrt(max_length);
    tangent=Scale(tangent,common);bitangent=Scale(bitangent,common);
    const double hand=Dot(Cross(tangent,bitangent),normal);
    const double basis_lengths=Dot(tangent,tangent)*Dot(bitangent,bitangent);
    if (!(basis_lengths>0)||std::abs(hand)<=kRelativeEpsilon*std::sqrt(basis_lengths)) return Status::DegenerateUv;
    CotangentFrame candidate;
    if (!Store(tangent,candidate.tangent)||!Store(bitangent,candidate.bitangent)||!Store(normal,candidate.normal))
        return Status::NonFinite;
    candidate.uv_orientation=determinant<0?-1:1;
    candidate.handedness=hand<0?-1:1;
    candidate.ready=true;
    result=candidate;
    return Status::Ready;
}

std::string_view GeometryHlsl() {
    return R"HLSL(
// Independent opt-in helpers. row_major parameters accept untransposed CPU RowMatrix4 values.
static const float PBR_GEO_EPS = 1e-6;
float PbrGeoMaxAbs(float3 a) { return max(abs(a.x),max(abs(a.y),abs(a.z))); }
bool PbrGeoUnit(float3 value,out float3 result) {
    result=0;
    if (!all(isfinite(value))) return false;
    float scale=PbrGeoMaxAbs(value);
    if (!(scale>0)) return false;
    float3 v=value/scale;
    float length2=dot(v,v);
    if (!(length2>0)||!isfinite(length2)) return false;
    result=v*rsqrt(length2);
    return all(isfinite(result));
}
bool PbrGeoAffine(row_major float4x4 m) {
    return all(isfinite(m[0]))&&all(isfinite(m[1]))&&all(isfinite(m[2]))&&all(isfinite(m[3]))&&
           m[0][3]==0&&m[1][3]==0&&m[2][3]==0&&m[3][3]==1;
}
bool PbrGeoPosition(float3 model_position,row_major float4x4 model_to_view,out float3 result) {
    result=0;
    if (!all(isfinite(model_position))||!PbrGeoAffine(model_to_view)) return false;
    float4 value=mul(float4(model_position,1),model_to_view);
    if (!all(isfinite(value))||value.w!=1) return false;
    result=value.xyz;
    return true;
}
bool PbrGeoNormal(float3 model_normal,row_major float4x4 normal_to_view,out float3 result) {
    result=0;
    float3 n;
    if (!PbrGeoAffine(normal_to_view)||!PbrGeoUnit(model_normal,n)) return false;
    float scale=max(PbrGeoMaxAbs(normal_to_view[0].xyz),max(PbrGeoMaxAbs(normal_to_view[1].xyz),PbrGeoMaxAbs(normal_to_view[2].xyz)));
    if (!(scale>0)) return false;
    // Only the linear rows participate. Do not divide the dummy homogeneous 1 by a tiny scale.
    float3 transformed=n.x*(normal_to_view[0].xyz/scale)+n.y*(normal_to_view[1].xyz/scale)+n.z*(normal_to_view[2].xyz/scale);
    return PbrGeoUnit(transformed,result);
}
bool PbrGeoPositionDerivatives(float3 dx,float3 dy,out float3 a,out float3 b,out float3 area) {
    a=0;b=0;area=0;
    if (!all(isfinite(dx))||!all(isfinite(dy))) return false;
    float scale=max(PbrGeoMaxAbs(dx),PbrGeoMaxAbs(dy));
    if (!(scale>0)) return false;
    float3 x=dx/scale,y=dy/scale,c=cross(x,y);
    float lengths=dot(x,x)*dot(y,y);
    if (!(lengths>0)||dot(c,c)<=PBR_GEO_EPS*PBR_GEO_EPS*lengths) return false;
    a=x;b=y;area=c;
    return true;
}
bool PbrGeoFaceNormal(float3 dx,float3 dy,bool flip_orientation,out float3 result) {
    result=0;
    float3 a,b,area,n;
    if (!PbrGeoPositionDerivatives(dx,dy,a,b,area)||!PbrGeoUnit(area,n)) return false;
    result=flip_orientation?-n:n;
    return true;
}
struct PbrGeoFrame {
    float3 tangent,bitangent,normal;
    float uv_orientation,handedness;
};
bool PbrGeoCotangentFrame(float3 dx,float3 dy,float2 uv_dx,float2 uv_dy,float3 geometric_normal,out PbrGeoFrame result) {
    result=(PbrGeoFrame)0;
    if (!all(isfinite(uv_dx))||!all(isfinite(uv_dy))) return false;
    float3 a,b,area,n;
    if (!PbrGeoPositionDerivatives(dx,dy,a,b,area)||!PbrGeoUnit(geometric_normal,n)) return false;
    float facing=dot(area,n);
    if (abs(facing)<=PBR_GEO_EPS*sqrt(dot(area,area))) return false;
    float uv_scale=max(max(abs(uv_dx.x),abs(uv_dx.y)),max(abs(uv_dy.x),abs(uv_dy.y)));
    if (!(uv_scale>0)) return false;
    float2 ux=uv_dx/uv_scale,uy=uv_dy/uv_scale;
    float determinant=ux.x*uy.y-ux.y*uy.x,lengths=dot(ux,ux)*dot(uy,uy);
    if (!(lengths>0)||abs(determinant)<=PBR_GEO_EPS*sqrt(lengths)) return false;
    float3 pdy_perp=cross(b,n),pdx_perp=cross(n,a);
    float3 tangent=pdy_perp*ux.x+pdx_perp*uy.x,bitangent=pdy_perp*ux.y+pdx_perp*uy.y;
    float max_length=max(dot(tangent,tangent),dot(bitangent,bitangent));
    if (!(max_length>0)||!isfinite(max_length)) return false;
    float common=(facing<0?-1:1)*rsqrt(max_length);
    tangent*=common;bitangent*=common;
    float hand=dot(cross(tangent,bitangent),n),basis_lengths=dot(tangent,tangent)*dot(bitangent,bitangent);
    if (!(basis_lengths>0)||abs(hand)<=PBR_GEO_EPS*sqrt(basis_lengths)||!all(isfinite(tangent))||!all(isfinite(bitangent))) return false;
    result.tangent=tangent;result.bitangent=bitangent;result.normal=n;
    result.uv_orientation=determinant<0?-1:1;result.handedness=hand<0?-1:1;
    return true;
}
)HLSL";
}

}  // namespace gpu::pbr::geometry
