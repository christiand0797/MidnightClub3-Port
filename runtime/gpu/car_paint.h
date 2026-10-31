#pragma once
// Enhanced car paint (opt-in, F1 > Quality "Enhanced car paint"): per-pixel lighting and clearcoat highlights for the
// car vertex programs. Those programs light per vertex (docs/notes/car_paint_lighting.md): normal N (v2) by the
// normal matrix c134-c136, ambient c99 plus three directional lights (directions c108-c110, colours c96-c98, floors
// c106), times c107 into oD0. The variant also hands the pixel shader that light-space normal and the view vector;
// the pixel shader keeps the console's combiner result (paint, vinyl, environment reflection), scales it by the
// per-pixel over the per-vertex lighting (smooth shading on low polygon bodies) and adds GGX highlights of the same
// three lights. The view vector comes from the camera position in object space, solved from the clip rows c100,
// c101 and c103 (the point where clip x, y and w all vanish).
#include <array>
#include <cmath>
#include <string>

namespace gpu::car_paint {

// the program follows the car lighting model above (its generated HLSL reads those constants and the normal)
inline bool LightingProgram(const std::string& hlsl) {
    for (const char* needle : {"c[134]", "c[135]", "c[136]", "c[108]", "c[109]", "c[110]", "c[96]", "c[107]",
                               "c[100]", "c[101]", "c[103]", "i.v0", "i.v2"})
        if (hlsl.find(needle) == std::string::npos) return false;
    return true;
}

// the camera in object space from clip rows x, y and w (row . (p, 1) = 0 for all three): Cramer's rule. The
// HLSL in VertexHlsl below is the same expression. False when the rows are degenerate (no perspective camera).
inline bool CameraFromClipRows(const std::array<float, 4>& x, const std::array<float, 4>& y,
                               const std::array<float, 4>& w, std::array<float, 3>& camera) {
    auto cross = [](const std::array<float, 4>& a, const std::array<float, 4>& b) {
        return std::array<float, 3>{a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
    };
    auto dot = [](const std::array<float, 4>& a, const std::array<float, 3>& b) {
        return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    };
    const auto yw = cross(y, w), wx = cross(w, x), xy = cross(x, y);
    const float det = dot(x, yw);
    if (!std::isfinite(det) || std::fabs(det) < 1e-12f) return false;
    for (int i = 0; i < 3; i++) camera[i] = -(x[3] * yw[i] + y[3] * wx[i] + w[3] * xy[i]) / det;
    return std::isfinite(camera[0]) && std::isfinite(camera[1]) && std::isfinite(camera[2]);
}

namespace detail {
inline bool Insert(std::string& s, const char* at, const std::string& before, const std::string& after) {
    const size_t p = s.rfind(at);
    if (p == std::string::npos) return false;
    s.replace(p, std::string(at).size(), before + at + after);
    return true;
}
constexpr const char* kOutputs = "    float4 cn : TEXCOORD6;\n    float4 cv : TEXCOORD7;\n";
}  // namespace detail

// a car lighting program's HLSL with the light-space normal (cn), view vector (cv) and reflection-map normal UV (cn.w, cv.w)
inline std::string VertexHlsl(std::string s) {
    detail::Insert(s, "    float fogamt : TEXCOORD4;\n", "", detail::kOutputs);
    detail::Insert(s, "    return o;\n}", R"(    // enhanced car paint (gpu/car_paint.h): light-space normal, view vector and reflection-map normal UV
    {
        float3 nrm = i.v2.xyz;
        float3 rx = c[100].xyz, ry = c[101].xyz, rw = c[103].xyz;
        float3 yw = cross(ry, rw), wx = cross(rw, rx), xy = cross(rx, ry);
        float det = dot(rx, yw);
        float3 cam = abs(det) > 1e-12 ? -(c[100].w * yw + c[101].w * wx + c[103].w * xy) / det : i.v0.xyz;
        float3 view = cam - i.v0.xyz;
        float2 uv_scale = c[112].z != 0 ? c[112].xy : c[113].xy;
        float2 uv_n = float2(dot(nrm, c[100].xyz), dot(nrm, c[101].xyz)) * c[116].w * uv_scale;
        o.cn = float4(dot(nrm, c[134].xyz), dot(nrm, c[135].xyz), dot(nrm, c[136].xyz), uv_n.x);
        o.cv = float4(dot(view, c[134].xyz), dot(view, c[135].xyz), dot(view, c[136].xyz), uv_n.y);
    }
)", "");
    return s;
}

// a pixel shader for such a draw: per-pixel reflection UV reprojection on target stages, then relighting and highlights.
// carpaint (DrawConstants): x relight strength 0..1, y highlight strength, z roughness, w clearcoat reflection.
// The game's diffuse is C * N.L with no 1/pi and the paint combiner doubles it (mul_x2 by v0), so a light of colour C
// carries radiance 2 pi C for the highlights. target_mask bits 0..2 mark 2D render-target stages (reflection maps).
inline std::string PixelHlsl(std::string s, uint32_t target_mask = 0) {
    const char* env_tex = ((target_mask & 4) && s.find("float4 t2 =") != std::string::npos) ? "t2.rgb"
                        : ((target_mask & 2) && s.find("float4 t1 =") != std::string::npos) ? "t1.rgb"
                        : nullptr;
    detail::Insert(s, "    float fogamt : TEXCOORD4;\n", "", detail::kOutputs);
    if (target_mask & 7) {
        std::string uv_shift = "    {\n"
                               "        float2 duv = float2(i.cn.w, i.cv.w) * (rsqrt(max(dot(i.cn.xyz, i.cn.xyz), 1e-12)) - 1) * saturate(carpaint.x);\n";
        if (target_mask & 1) uv_shift += "        tc0.xy += duv;\n";
        if (target_mask & 2) uv_shift += "        tc1.xy += duv;\n";
        if (target_mask & 4) uv_shift += "        tc2.xy += duv;\n";
        uv_shift += "    }\n";
        detail::Insert(s, "    float4 tc0 = i.tc0, tc1 = i.tc1, tc2 = i.tc2, tc3 = i.tc3;\n", "", uv_shift);
    }
    detail::Insert(s, "alphatest(outc.a);\n", R"(// enhanced car paint (gpu/car_paint.h): the game's ambient and three directional lights again, per pixel
{
    float3 n = i.cn.xyz * rsqrt(max(dot(i.cn.xyz, i.cn.xyz), 1e-12));
    float3 V = i.cv.xyz * rsqrt(max(dot(i.cv.xyz, i.cv.xyz), 1e-12));
    float3 diffuse = c[99].xyz, highlight = 0;
    float rough = clamp(carpaint.z, 0.02, 1), pbr_ao = 1;
)" + ((!(target_mask & 1) && s.find("pbr_nrm") != std::string::npos) ? R"(    {
        float3 pn = pbr_nrm.Sample(smp0, tc0.xy).rgb * 2 - 1;
        pn.xy *= pbr.x;
        pn = normalize(float3(pn.xy, max(pn.z, 1e-3)));
        float3 up = abs(n.y) < 0.99 ? float3(0, 1, 0) : float3(1, 0, 0);
        float3 T = normalize(cross(up, n)), B = cross(n, T);
        n = normalize(T * pn.x + B * pn.y + n * pn.z);
        float4 orm = pbr_orm.Sample(smp0, tc0.xy);
        pbr_ao = saturate(1 + (orm.r - 1) * pbr.y);
        float r_map = clamp(pbr.z < 0 ? (-pbr.z - 1) : orm.g * pbr.z, 0.02, 1);
        rough = clamp(lerp(rough, r_map, 0.65), 0.02, 1);
    }
)" : std::string()) + R"(    float a = rough * rough, a2 = a * a, k = a * 0.5;
    float nv = saturate(dot(n, V)) + 1e-4;
    [unroll] for (int l = 0; l < 3; l++) {
        float3 L = -c[108 + l].xyz;
        float3 C = c[96 + l].xyz;
        diffuse += max(dot(n, L), c[106][l]) * C;
        float3 Ld = L * rsqrt(max(dot(L, L), 1e-12));
        float3 H = (Ld + V) * rsqrt(max(dot(Ld + V, Ld + V), 1e-12));
        float nl = saturate(dot(n, Ld)), nh = saturate(dot(n, H)), vh = saturate(dot(V, H));
        float d = nh * nh * (a2 - 1) + 1;
        float D = a2 / (3.14159265 * d * d + 1e-7);
        float F = 0.04 + 0.96 * pow(1 - vh, 5);
        highlight += C * (D * F * nl / (4 * (nl * (1 - k) + k) * (nv * (1 - k) + k)));
    }
    float3 ratio = clamp(saturate(diffuse * c[107].xyz) / max(v0.rgb, 1.0 / 255), 0, 4);
    float3 relit = outc.rgb * lerp(1, ratio, saturate(carpaint.x)) * pbr_ao;
    float3 coat = relit;
)" + (env_tex ? "    float coatF = 0.04 + 0.96 * pow(1 - nv, 5);\n    coat = lerp(relit, " + std::string(env_tex) + ", saturate(coatF * carpaint.w));\n" : std::string()) + R"(    outc.rgb = lerp(outc.rgb, coat, fogin.a) + highlight * 6.2831853 * c[107].xyz * carpaint.y * fogin.a;
}
)", "");
    return s;
}

}  // namespace gpu::car_paint
