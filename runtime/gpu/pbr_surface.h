#pragma once
#include "gpu/pbr_material.h"
#include "gpu/texture.h"
#include <span>

namespace gpu::pbr {
struct ColorProof {
    uint32_t width=0,height=0;
    std::string alpha_sha256;
    bool constant_alpha=false;
    uint8_t alpha=0;
    bool ready() const { return width&&height&&alpha_sha256.size()==64; }
};
// Exact alpha of the actual level-zero upload, AFTER extension decoding. Never infers metalness.
// Accepts tightly bounded BGRA8 or BC1/2/3 bytes; malformed/unsupported inputs return no proof.
ColorProof ProveUploadedColor(Host,uint32_t width,uint32_t height,uint32_t pitch,
                             std::span<const uint8_t> actual_upload) noexcept;
} // namespace gpu::pbr
