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
}
