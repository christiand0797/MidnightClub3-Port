#pragma once
#include <string_view>

namespace gpu::pbr {
// Real shared HLSL, compiled by CPU tests. A future material PS calls these functions with validated
// linear-space geometry/light inputs; this is not injected into the legacy final-colour post pass.
inline constexpr std::string_view kBrdfHlsl = R"HLSL(
static const float MC3_PBR_PI = 3.14159265358979323846;
float3 Mc3PbrSafeNormalize(float3 v) {
    float length2 = dot(v,v);
    return isfinite(length2) && length2 > 1e-12 ? v * rsqrt(length2) : 0.0.xxx;
}
float Mc3PbrSrgbToLinear(float x) {
    x = saturate(x);
    return x <= 0.04045 ? x / 12.92 : pow((x + 0.055) / 1.055, 2.4);
}
float3 Mc3PbrFresnel(float3 f0, float c) {
    float a = 1.0 - saturate(c); float a2 = a * a;
    return saturate(f0) + (1.0 - saturate(f0)) * (a2 * a2 * a);
}
float Mc3PbrDistribution(float nh, float roughness) {
    float r = clamp(roughness, 0.045, 1.0), alpha = r * r, a2 = alpha * alpha;
    nh = saturate(nh);
    float nh2 = nh * nh, d = nh2 * a2 + (1.0 - nh2);
    return a2 / (MC3_PBR_PI * d * d);
}
float Mc3PbrVisibility(float nv, float nl, float roughness) {
    if (nv <= 0.0 || nl <= 0.0) return 0.0;
    float r = clamp(roughness, 0.045, 1.0), alpha = r * r, a2 = alpha * alpha;
    nv = saturate(nv); nl = saturate(nl);
    float gv = nv + sqrt(a2 + (1.0 - a2) * nv * nv);
    float gl = nl + sqrt(a2 + (1.0 - a2) * nl * nl);
    return 1.0 / (gv * gl);
}
float3 Mc3PbrDirect(float3 baseColor, float metallic, float roughness,
                   float3 n, float3 v, float3 l, float3 radiance) {
    if (dot(n,n) < 1e-12 || dot(v,v) < 1e-12 || dot(l,l) < 1e-12) return 0.0;
    n = Mc3PbrSafeNormalize(n); v = Mc3PbrSafeNormalize(v); l = Mc3PbrSafeNormalize(l);
    float nv = dot(n,v), nl = dot(n,l);
    if (nv <= 0.0 || nl <= 0.0 || dot(v+l,v+l) < 1e-12) return 0.0;
    float3 h = Mc3PbrSafeNormalize(v+l);
    baseColor = saturate(baseColor); metallic = saturate(metallic);
    float3 f = Mc3PbrFresnel(lerp(0.04.xxx, baseColor, metallic), dot(v,h));
    float3 diffuse = (1.0 - metallic) * (1.0 - f) * baseColor / MC3_PBR_PI;
    float3 specular = f * Mc3PbrDistribution(dot(n,h),roughness) * Mc3PbrVisibility(nv,nl,roughness);
    return (diffuse + specular) * max(radiance, 0.0) * nl;
}
float3 Mc3PbrIndirectDiffuse(float3 baseColor, float metallic, float ao, float3 irradiance) {
    return saturate(baseColor) * (1.0 - saturate(metallic)) * saturate(ao) * max(irradiance,0.0) / MC3_PBR_PI;
}
float3 Mc3PbrTangentNormal(float3 sampled, float strength, bool greenUp, float3 t, float3 b, float3 n) {
    float3 m = saturate(sampled) * 2.0 - 1.0;
    m.xy *= clamp(strength,0.0,2.0);
    if (greenUp) m.y = -m.y;
    float3 result = t*m.x + b*m.y + n*m.z;
    return dot(result,result) > 1e-12 ? Mc3PbrSafeNormalize(result) : Mc3PbrSafeNormalize(n);
}
)HLSL";
} // namespace gpu::pbr
