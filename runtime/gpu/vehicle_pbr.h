#pragma once
// Reviewed car-program surface bridge. This is shader construction, not material approval or GPU proof.
// Only the two complete translated instruction bodies documented in pbr_geometry_bridge_20261028.md are
// recognized. The original instructions, raster position, combiners, alpha, fog and velocity outputs stay intact.
#include "gpu/car_paint.h"
#include "gpu/shaders.h"

#include <array>
#include <cmath>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace gpu::vehicle_pbr {

enum class Profile { Unsupported, DefaultShiny, CarPaint };
constexpr uint16_t kRegisters = (1u << 0) | (1u << 2) | (1u << 3) | (1u << 9);

namespace detail {
constexpr std::string_view kStart =
    "    {\n        precise float4 mr = mac_mul(((i.v3).wwww), ((c[116]).xxxx));";
constexpr std::string_view kEnd = "\n    VOut o;";
inline uint64_t Digest(std::string_view text, uint64_t seed) {
    for (unsigned char c : text) seed = (seed ^ c) * 1099511628211ull;
    return seed;
}
inline std::string_view Body(std::string_view source) {
    const size_t main = source.find("VOut main(");
    if (main == source.npos) return {};
    const size_t first = source.find(kStart, main), last = source.find(kEnd, first);
    if (first == source.npos || last == source.npos) return {};
    return source.substr(first, last - first);
}
inline bool Replace(std::string& source, std::string_view from, std::string_view to) {
    const size_t at = source.find(from);
    if (at == source.npos || source.find(from, at + from.size()) != source.npos) return false;
    source.replace(at, from.size(), to);
    return true;
}
inline bool ReplaceInMain(std::string& source,std::string_view from,std::string_view to) {
    const size_t main=source.find("VOut main("),at=source.find(from,main);
    if(main==source.npos||at==source.npos||source.find(from,at+from.size())!=source.npos)return false;
    source.replace(at,from.size(),to);return true;
}
inline bool Outputs(std::string& source) {
    // TEXCOORD8/9 belong to DLSS. Do not alias them or silently reuse an occupied semantic.
    if (source.find("TEXCOORD10") != source.npos || source.find("TEXCOORD11") != source.npos ||
        source.find("vehicle_position") != source.npos) return false;
    const size_t begin = source.find("struct VOut {"), end = source.find("};", begin);
    if (begin == source.npos || end == source.npos) return false;
    source.insert(end, "    float3 vehicle_position : TEXCOORD10;\n    float3 vehicle_normal : TEXCOORD11;\n");
    return true;
}
} // namespace detail

// Both independent full-body hashes and length must agree; a short opcode prefix/filename never authorizes a
// program. The backend must additionally match the approved material's complete stable VS/PS draw keys.
inline Profile Recognize(std::string_view generated) {
    const auto body = detail::Body(generated);
    const uint64_t a = detail::Digest(body, 1469598103934665603ull);
    const uint64_t b = detail::Digest(body, 7809847782465536322ull);
    if (body.size() == 4054 && a == 0xcfa69ce70c03c965ull && b == 0xe9bbbe43664294f8ull)
        return Profile::DefaultShiny;
    if (body.size() == 4823 && a == 0xaa6d62626fd9e7d2ull && b == 0xdbd076a6dd3cdd81ull)
        return Profile::CarPaint;
    return Profile::Unsupported;
}
inline Profile Recognize(const std::vector<uint32_t>& program, uint16_t regs, uint64_t decode) {
    // These declarations are the actual float4-converted or packed-normal forms in the captured draws.
    if (regs != kRegisters || (decode != 0 && decode != (4ull << 8))) return Profile::Unsupported;
    return Recognize(gpu::VertexProgramHlsl(program, regs, decode));
}
inline bool SupportsStage(Profile profile, uint32_t stage, uint32_t uv_set) {
    // The exact programs move authored v9 to oT0 (default_shiny) or oT1 (carpaint). Other outputs are generated
    // reflection/ramp coordinates, not a surface UV chart. uv_set names authored v9, i.e. set zero.
    return uv_set == 0 && ((profile == Profile::DefaultShiny && stage == 0) ||
                           (profile == Profile::CarPaint && stage == 1));
}

// A normal transform also serves as a position-space rotation only when it is proper rigid. This check must use
// the draw's FINAL uploaded c134..136, after extensions; it cannot be inferred from the recognized program.
inline bool RigidLightTransform(const std::array<std::array<float,4>,3>& rows) {
    for (const auto& row : rows) for (float x : row) if (!std::isfinite(x)) return false;
    for (size_t i = 0; i < 3; ++i) for (size_t j = 0; j < 3; ++j) {
        float d = 0; for (size_t k = 0; k < 3; ++k) d += rows[i][k] * rows[j][k];
        if (std::fabs(d - (i == j ? 1.f : 0.f)) > 0.001f) return false;
    }
    const float det = rows[0][0]*(rows[1][1]*rows[2][2]-rows[1][2]*rows[2][1]) -
                      rows[0][1]*(rows[1][0]*rows[2][2]-rows[1][2]*rows[2][0]) +
                      rows[0][2]*(rows[1][0]*rows[2][1]-rows[1][1]*rows[2][0]);
    return std::fabs(det - 1.f) <= 0.002f;
}
inline bool ConstantsSupported(std::span<const float> constants) {
    if (constants.size() != 192 * 4) return false;
    std::array<std::array<float,4>,3> rows{}, clip{};
    for (size_t i = 0; i < 3; ++i) for (size_t j = 0; j < 4; ++j) {
        rows[i][j] = constants[(134+i)*4+j];
        clip[i][j] = constants[(i == 2 ? 103 : 100+i)*4+j];
        if (!std::isfinite(clip[i][j])) return false;
    }
    std::array<float,3> camera{};
    return RigidLightTransform(rows) && car_paint::CameraFromClipRows(clip[0],clip[1],clip[2],camera);
}

// CPU mirror of the pixel frame, for UV-orientation/degenerate checks without a GPU.
inline std::array<float,3> NormalFromDerivatives(std::array<float,3> px,std::array<float,3> py,
        std::array<float,2> ux,std::array<float,2> uy,std::array<float,3> n,std::array<float,3> map) {
    const auto dot=[](const auto& a,const auto& b){return a[0]*b[0]+a[1]*b[1]+a[2]*b[2];};
    const auto cross=[](const auto& a,const auto& b){return std::array<float,3>{
        a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]};};
    const float det=ux[0]*uy[1]-ux[1]*uy[0];
    std::array<float,3> t{},b{};
    for(size_t k=0;k<3;++k){t[k]=px[k]*uy[1]-py[k]*ux[1];b[k]=py[k]*ux[0]-px[k]*uy[0];}
    const auto plane=cross(px,py);const float area=dot(plane,plane);
    const float tt=dot(t,t),bb=dot(b,b),nn=dot(n,n);
    if(!std::isfinite(det)||!std::isfinite(tt)||!std::isfinite(bb)||!std::isfinite(nn)||
        !std::isfinite(area)||area<1e-20f||std::fabs(det)<1e-10f||tt<1e-16f||bb<1e-16f||nn<1e-12f)return n;
    const float sign=det<0?-1.f:1.f;
    for(size_t k=0;k<3;++k)t[k]*=sign;
    const float projection=dot(n,t);
    for(size_t k=0;k<3;++k)t[k]-=n[k]*projection;
    const float length2=dot(t,t);if(length2<1e-16f)return n;
    for(float& x:t)x/=std::sqrt(length2);
    auto bitangent=cross(n,t);
    const float handed=dot(bitangent,b)*sign<0?-1.f:1.f;
    std::array<float,3> out{};
    for(size_t k=0;k<3;++k)out[k]=t[k]*map[0]+bitangent[k]*handed*map[1]+n[k]*map[2];
    const float length=std::sqrt(dot(out,out));
    if(!std::isfinite(length)||length<1e-12f)return n;
    for(float& x:out)x/=length;return out;
}

// Input is car_paint::VertexHlsl(current VertexProgramHlsl(...)), before ReversedDepth/Velocity transforms.
// Empty means unsupported source, occupied outputs or a changed translation; caller keeps its original variant.
inline std::optional<std::string> VertexHlsl(std::string source, Profile profile, uint32_t stage, uint32_t uv_set) {
    if (profile == Profile::Unsupported || Recognize(source) != profile || !SupportsStage(profile,stage,uv_set) ||
        source.find("float4 cn : TEXCOORD6;") == source.npos ||
        source.find("float4 cv : TEXCOORD7;") == source.npos || !detail::Outputs(source)) return {};
    const size_t body = source.find(detail::kStart, source.find("VOut main("));
    source.insert(body, "    float3 vehicle_morphed = 0, vehicle_lit_normal = 0;\n");
    // Snapshot immediately after their actual instruction blocks. R0 is swizzled XYW, and R11 is overwritten
    // later by fresnel/lighting. Never recover these from raw v0 or the final temporary register values.
    const std::string morph = "        R0.xyw = mr.xyw;\n    }\n";
    const std::string normal = "        precise float4 mr = dot(((i.v2).xyzz).xyz, ((c[136]).xyzz).xyz).xxxx;\n        R11.z = mr.z;\n    }\n";
    if (!detail::ReplaceInMain(source,morph,morph+"    vehicle_morphed = R0.xyw;\n") ||
        !detail::ReplaceInMain(source,normal,normal+"    vehicle_lit_normal = R11.xyz;\n")) return {};
    const std::string final = R"(    // exact-program vehicle PBR bridge; original o.pos, oD*, oT* and fog are untouched
    o.vehicle_position = float3(dot(vehicle_morphed,c[134].xyz),dot(vehicle_morphed,c[135].xyz),dot(vehicle_morphed,c[136].xyz));
    o.vehicle_normal = vehicle_lit_normal;
    o.cn.xyz = vehicle_lit_normal;
    {
        float3 yw = cross(c[101].xyz,c[103].xyz), wx = cross(c[103].xyz,c[100].xyz), xy = cross(c[100].xyz,c[101].xyz);
        float det = dot(c[100].xyz,yw);
        float3 cam = -(c[100].w*yw+c[101].w*wx+c[103].w*xy)/det;
        float3 view = cam-vehicle_morphed;
        o.cv.xyz = float3(dot(view,c[134].xyz),dot(view,c[135].xyz),dot(view,c[136].xyz));
    }
    return o;
})";
    if (!detail::Replace(source,"    return o;\n}",final)) return {};
    return source;
}

// Derivative cotangent frame follows the actual selected UVs, including rotations/mirrors. A collapsed UV or
// position triangle keeps the original normal (no guessed arbitrary axis, no discard/coverage change).
inline constexpr const char* kPixelHelpers = R"(
float3 VehicleMappedNormal(float3 p,float3 n,float2 uv,float3 map) {
    float3 px=ddx(p),py=ddy(p); float2 ux=ddx(uv),uy=ddy(uv);
    float det=ux.x*uy.y-ux.y*uy.x;
    float3 plane=cross(px,py); float area=dot(plane,plane);
    float3 tx=px*uy.y-py*ux.y, bx=py*ux.x-px*uy.x;
    if(!all(isfinite(float4(det,dot(tx,tx),dot(bx,bx),dot(n,n)))) || abs(det)<1e-10 ||
       !isfinite(area) || area<1e-20 || dot(tx,tx)<1e-16 || dot(bx,bx)<1e-16 || dot(n,n)<1e-12) return n;
    float orient=det<0?-1:1;
    float3 T=tx*orient; T-=n*dot(n,T);
    if(dot(T,T)<1e-16)return n;
    T=normalize(T);
    float3 B=cross(n,T); B*=dot(B,bx*orient)<0?-1:1;
    return normalize(T*map.x+B*map.y+n*map.z);
}
float3 VehicleLinear(float3 encoded) {
    return encoded<=0.04045?encoded/12.92:pow((encoded+0.055)/1.055,2.4);
}
)";

// Input is the enhanced car-paint pixel variant WITHOUT the quarantined pk.pbr path. Map bindings t4/t5 are
// approved normal/ORM, each with its own actual format/mips; stage's sampler/addressing is used, not smp0.
// pbr = normal strength, AO strength, roughness factor/negative override, metallic factor/negative override.
// pbr2.yzw = tint. There is deliberately no parallax or geometry-displacement claim in this compatibility path.
inline std::optional<std::string> PixelHlsl(std::string source,uint32_t stage,bool bc5,bool directx) {
    if (stage > 1 || source.find("pbr_nrm") != source.npos ||
        source.find("float4 cn : TEXCOORD6;") == source.npos || !detail::Outputs(source)) return {};
    const std::string index=std::to_string(stage);
    const std::string maps = "    float2 vehicle_uv = tc"+index+".xy;\n"+
        "    float2 vehicle_dx=ddx(vehicle_uv),vehicle_dy=ddy(vehicle_uv);\n"+
        "    float4 vehicle_map=vehicle_normal_map.SampleGrad(smp"+index+",vehicle_uv,vehicle_dx,vehicle_dy);\n"+
        "    float3 vehicle_ts=vehicle_map.rgb*2-1;\n"+
        (bc5 ? "    vehicle_ts.z=sqrt(saturate(1-dot(vehicle_ts.xy,vehicle_ts.xy)));\n" : "")+
        (directx ? "" : "    vehicle_ts.y=-vehicle_ts.y;\n")+
        "    vehicle_ts.xy*=pbr.x; vehicle_ts=normalize(float3(vehicle_ts.xy,max(vehicle_ts.z,1e-3)));\n"+
        "    n=VehicleMappedNormal(i.vehicle_position,normalize(i.vehicle_normal),vehicle_uv,vehicle_ts);\n"+
        "    float3 vehicle_orm=vehicle_orm_map.SampleGrad(smp"+index+",vehicle_uv,vehicle_dx,vehicle_dy).rgb;\n"+
        "    float vehicle_ao=saturate(1+(vehicle_orm.r-1)*pbr.y);\n"+
        "    float rough=clamp(pbr.z<0?-pbr.z-1:vehicle_orm.g*pbr.z,0.02,1);\n"+
        "    float vehicle_metal=saturate(pbr.w<0?-pbr.w-1:vehicle_orm.b*pbr.w);\n"+
        "    float3 vehicle_f0=lerp(float3(0.04,0.04,0.04),VehicleLinear(saturate(outc.rgb)),vehicle_metal);\n"+
        "    float3 diffuse=c[99].xyz*vehicle_ao,highlight=0;\n";
    if (!detail::Replace(source,
        "    float3 diffuse = c[99].xyz, highlight = 0;\n    float rough = clamp(carpaint.z, 0.02, 1), pbr_ao = 1;\n",maps) ||
        !detail::Replace(source,"        float F = 0.04 + 0.96 * pow(1 - vh, 5);",
                                "        float3 F = vehicle_f0 + (1-vehicle_f0)*pow(1-vh,5);") ||
        !detail::Replace(source,"    float3 relit = outc.rgb * lerp(1, ratio, saturate(carpaint.x)) * pbr_ao;",
                                "    float3 relit = outc.rgb * pbr2.yzw * lerp(1, ratio, saturate(carpaint.x)) * (1-vehicle_metal);")) return {};
    const size_t entry=source.find("float4 main(VOut i, bool front");
    if(entry==source.npos)return {};
    source.insert(entry,std::string("Texture2D<float4> vehicle_normal_map : register(t4);\nTexture2D<float4> vehicle_orm_map : register(t5);\n")+kPixelHelpers);
    return source;
}

} // namespace gpu::vehicle_pbr
