#include "gpu/texture.h"
#include <cassert>

// A texture laid over memory a render target once used is only that target when the NV2A could have drawn it.
// Car vinyls are P8 textures the title builds in recycled heap memory and recolours by rewriting their palette:
// sampling a stale target there (or its palette-less re-upload) loses the vinyl and every colour change.
int main() {
    constexpr uint32_t kA8R8G8B8 = 0x06, kX8R8G8B8 = 0x07, kR5G6B5 = 0x05, kLinA8R8G8B8 = 0x12;
    constexpr uint32_t kP8 = 0x0B, kDxt1 = 0x0C, kDxt5 = 0x0F;
    for (uint32_t f : {kA8R8G8B8, kX8R8G8B8, kR5G6B5, kLinA8R8G8B8}) assert(gpu::CanBeTargetFormat(f, false));
    assert(!gpu::CanBeTargetFormat(kP8, false));
    for (uint32_t f : {kDxt1, kDxt5}) assert(!gpu::CanBeTargetFormat(f, true));
}
