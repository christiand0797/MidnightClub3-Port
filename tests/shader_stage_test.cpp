#include "gpu/shaders.h"
#include <cassert>
#include <string>

int main() {
    gpu::State state{};
    gpu::TexInfo textures[4]{};
    textures[0].kind = gpu::kTex2D;
    for (auto& stage : state.tss) {
        stage[gpu::kTssColorOp] = 2;  // SELECTARG1
        stage[gpu::kTssColorArg1] = 2;  // TEXTURE
        stage[gpu::kTssAlphaOp] = 2;
        stage[gpu::kTssAlphaArg1] = 2;
    }
    auto key = gpu::MakePixelKey(state, textures);
    auto shader = gpu::PixelHlsl(key);
    assert(key.ff[0][0] == 2);
    assert(key.ff[1][0] == 0 && key.ff[2][0] == 0);
    assert(shader.find("float4 t0 = tex0.Sample") != std::string::npos);
    assert(shader.find("float4 t1 =") == std::string::npos);
    // A stage using diffuse/current without a texture remains valid.
    state.tss[1][gpu::kTssColorArg1] = 0;
    key = gpu::MakePixelKey(state, textures);
    assert(key.ff[1][0] == 2 && key.ff[2][0] == 0);
    // A later bound texture cannot re-enable a cascade terminated by an absent earlier texture.
    textures[2].kind = gpu::kTex2D;
    state.tss[1][gpu::kTssColorArg1] = 2;
    key = gpu::MakePixelKey(state, textures);
    assert(key.ff[1][0] == 0 && key.ff[2][0] == 0);
    // A brightened source-alpha draw (car neon) keeps its hue: past alpha 1 the colour grows only until its
    // brightest channel reaches 1, so a coloured glow is not clipped to white.
    shader = gpu::PixelHlsl(key);
    assert(shader.find("float peak = max(max(outc.r, outc.g), outc.b);") != std::string::npos);
    assert(shader.find("min(sa / na, peak > 0 ? 1 / peak : 1)") != std::string::npos);

    // Fixed-function draws without a vertex normal (e.g. FVF 0x142 = XYZ | DIFFUSE | TEX1, regs = 0x209, used by car
    // neon tubes and underbody glow) must stay unlit even when D3DRS_LIGHTING is left enabled by a prior car pass;
    // otherwise the missing normal vattr[2] = (0,0,0) zeroes out the vertex diffuse colour and replaces it with
    // white ambient.
    state.rs[gpu::kRsLighting] = 1;
    state.rs[gpu::kRsColorVertex] = 1;
    state.rs[gpu::kRsDiffuseMaterialSource] = 1;
    const auto fk = gpu::MakeFixedKey(state);
    assert((fk.light & 1) != 0);
    const std::string unnormaled_vs = gpu::FixedVertexHlsl((1u << 0) | (1u << 3) | (1u << 9), false, 0, fk);
    assert(unnormaled_vs.find("oD0 = i.v3;") != std::string::npos);
    assert(unnormaled_vs.find("amb.rgb") == std::string::npos);
    const std::string normaled_vs = gpu::FixedVertexHlsl((1u << 0) | (1u << 2) | (1u << 3) | (1u << 9), false, 0, fk);
    assert(normaled_vs.find("amb.rgb") != std::string::npos);
}
