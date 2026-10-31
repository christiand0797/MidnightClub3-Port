// Enhanced car paint shader generation: detection, the camera solve, and that the generated variants compile.
#include "gpu/car_paint.h"
#include "gpu/shaders.h"

#include <cassert>
#include <cmath>
#include <d3dcompiler.h>
#include <iostream>
#include <wrl/client.h>

namespace {
bool Compiles(const std::string& source, const char* profile) {
    Microsoft::WRL::ComPtr<ID3DBlob> code, errors;
    const HRESULT hr = D3DCompile(source.data(), source.size(), "car_paint", nullptr, nullptr, "main", profile,
                                  D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
    if (FAILED(hr) && errors) std::cerr << static_cast<const char*>(errors->GetBufferPointer()) << "\n";
    return SUCCEEDED(hr);
}
}  // namespace

int main() {
    using namespace gpu::car_paint;
    // Camera at (3, -2, 7) looking down -z with a perspective projection: the solve recovers it from x, y, w rows.
    // view = translate(-camera); clip rows: x = f * (px - 3), y = f * (py + 2), w = -(pz - 7)
    const float f = 1.7f;
    std::array<float, 4> x{f, 0, 0, -3 * f}, y{0, f, 0, 2 * f}, w{0, 0, -1, 7};
    std::array<float, 3> camera{};
    assert(CameraFromClipRows(x, y, w, camera));
    assert(std::fabs(camera[0] - 3) < 1e-5f && std::fabs(camera[1] + 2) < 1e-5f && std::fabs(camera[2] - 7) < 1e-5f);
    // An orthographic/degenerate set has no camera point.
    std::array<float, 4> flat{0, 0, 0, 1};
    assert(!CameraFromClipRows(x, y, flat, camera));

    // Detection needs the whole lighting model; a program missing the normal matrix is left alone.
    const std::string lit = "c[134] c[135] c[136] c[108] c[109] c[110] c[96] c[107] c[100] c[101] c[103] i.v0 i.v2";
    assert(LightingProgram(lit));
    assert(!LightingProgram("c[108] c[109] c[110] c[96] c[107] c[100] c[101] c[103] i.v0 i.v2"));

    // A program reading v0 and v2 (here an empty one: the translator's epilogue only) gets the extra outputs and
    // still compiles; the matching pixel shader reads them and compiles with the shared constants.
    constexpr uint16_t kRegsV0V2 = (1 << 0) | (1 << 2);
    const std::string vs = VertexHlsl(gpu::VertexProgramHlsl({}, kRegsV0V2));
    assert(vs.find("float4 cn : TEXCOORD6;") != std::string::npos && vs.find("o.cv = float4(") != std::string::npos);
    assert(Compiles(vs, "vs_5_0"));

    gpu::State state{};
    gpu::TexInfo textures[4]{};
    textures[0].kind = gpu::kTex2D;
    state.tss[0][gpu::kTssColorOp] = 4;  // MODULATE texture, diffuse
    state.tss[0][gpu::kTssColorArg1] = 2;
    state.tss[0][gpu::kTssColorArg2] = 0;
    state.tss[0][gpu::kTssAlphaOp] = 2;
    state.tss[0][gpu::kTssAlphaArg1] = 2;
    auto key = gpu::MakePixelKey(state, textures);
    const std::string plain = gpu::PixelHlsl(key);
    const std::string ps = PixelHlsl(plain);
    assert(plain.find("i.cn") == std::string::npos);  // the ordinary shader is untouched
    assert(ps.find("float4 cn : TEXCOORD6;") != std::string::npos && ps.find("c[108 + l]") != std::string::npos);
    assert(ps.find("carpaint.y") != std::string::npos);
    assert(Compiles(ps, "ps_5_0"));
    assert(ps.find("coatF") == std::string::npos);  // no environment stage: no clearcoat reflection term

    // Stage 0 and 2 live targets (carpaint.fx's paint ramp + environment map): per-pixel UV shift and Fresnel clearcoat.
    for (int stage = 1; stage < 3; stage++) {
        textures[stage].kind = gpu::kTex2D;
        state.tss[stage][gpu::kTssColorOp] = 4;  // MODULATE texture, current
        state.tss[stage][gpu::kTssColorArg1] = 2;
        state.tss[stage][gpu::kTssColorArg2] = 1;
        state.tss[stage][gpu::kTssAlphaOp] = 2;
        state.tss[stage][gpu::kTssAlphaArg1] = 1;
    }
    textures[0].flags = gpu::kTexTarget;
    textures[2].flags = gpu::kTexTarget;
    key = gpu::MakePixelKey(state, textures);
    const std::string reflective = PixelHlsl(gpu::PixelHlsl(key), (1u << 0) | (1u << 2));
    assert(reflective.find("tc0.xy += duv;") != std::string::npos);
    assert(reflective.find("tc1.xy += duv;") == std::string::npos);
    assert(reflective.find("tc2.xy += duv;") != std::string::npos);
    assert(reflective.find("lerp(relit, t2.rgb") != std::string::npos);
    assert(Compiles(reflective, "ps_5_0"));
    // Stage 0 and 1 live targets (default_shiny.fx): stage 1 is the environment map.
    const std::string shiny = PixelHlsl(gpu::PixelHlsl(key), 1u << 1);
    assert(shiny.find("lerp(relit, t1.rgb") != std::string::npos);
    assert(Compiles(shiny, "ps_5_0"));
    // Asked for a reflection the shader cannot take (stage 2 not sampled): left out, still compiles.
    assert(PixelHlsl(plain, 1u << 2).find("coatF") == std::string::npos);
    // Stage 0 companion PBR maps (t4 normal, t5 ORM+height): world and car-paint variants both compile.
    key.pbr = 1;
    const std::string pbr_world = gpu::PixelHlsl(key);
    assert(pbr_world.find("Texture2D pbr_nrm : register(t4);") != std::string::npos);
    assert(pbr_world.find("Texture2D pbr_orm : register(t5);") != std::string::npos);
    assert(Compiles(pbr_world, "ps_5_0"));
    key.car_paint = 1;
    const std::string pbr_car = PixelHlsl(gpu::PixelHlsl(key), 1u << 1);
    assert(pbr_car.find("pbr_nrm.Sample(smp0, tc0.xy)") != std::string::npos);
    assert(Compiles(pbr_car, "ps_5_0"));
    std::cout << "car paint: detection, camera solve and D3DCompile of both variants passed\n";
}
