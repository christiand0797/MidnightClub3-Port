#pragma once
// Reflection of already compiled optimized shaders is CPU-only. It cannot prove pixel visibility.
#include <d3dcompiler.h>
#include <d3d11shader.h>
#include <wrl/client.h>
#include <cstdint>
#include <optional>

namespace gpu::photo_capture {
inline std::optional<uint32_t> CompiledTextureStages(ID3DBlob* code) {
    if (!code) return std::nullopt;
    Microsoft::WRL::ComPtr<ID3D11ShaderReflection> reflection;
    if (FAILED(D3DReflect(code->GetBufferPointer(), code->GetBufferSize(), IID_PPV_ARGS(&reflection)))) return std::nullopt;
    D3D11_SHADER_DESC shader{};
    if (FAILED(reflection->GetDesc(&shader))) return std::nullopt;
    uint32_t mask = 0;
    for (uint32_t i = 0; i < shader.BoundResources; ++i) {
        D3D11_SHADER_INPUT_BIND_DESC binding{};
        if (FAILED(reflection->GetResourceBindingDesc(i, &binding))) return std::nullopt;
        if (binding.Type != D3D_SIT_TEXTURE) continue;
        for (uint32_t slot = 0; slot < 4; ++slot)
            if (slot >= binding.BindPoint && slot - binding.BindPoint < binding.BindCount) mask |= 1u << slot;
    }
    return mask;
}
}  // namespace gpu::photo_capture
