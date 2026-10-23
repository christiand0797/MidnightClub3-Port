#pragma once
// Texture packs: every texture the title uploads can be dumped to textures/dump/<hash>.png, and any
// textures/replace/<hash>.png is used in its place at whatever size it is (an AI upscale, a remade texture). The hash
// is the texture's own bytes as the console holds them, with its format, size and palette, so it names the same
// texture on every run and every machine.
#include <cstdint>
#include <memory>
#include <vector>

#include "gpu/texture.h"

namespace gpu::pack {

// what a replacement holds: BGRA8, full mip chain, level 0 first
struct Image {
    uint32_t width = 0, height = 0;
    std::vector<std::vector<uint8_t>> levels;
};

bool DumpEnabled();     // settings: graphics.texture_dump
bool ReplaceEnabled();  // settings: graphics.texture_pack, and a replace folder with files in it

// the texture's name: its guest bytes (every level), format, size and the palette a P8 one is drawn with
uint64_t ContentHash(const TextureDesc& d, const uint8_t* guest, uint32_t bytes, uint64_t palette);

// the replacement for this hash; the caller owns decoded pixels until they have been copied to its upload buffer
// and can then release them. Null without one; only the file index and used hashes are retained by the pack.
std::unique_ptr<Image> Find(uint64_t hash);

// level 0 as the host decoded it (BGRA8, or BC1/BC2/BC3 blocks), written once per hash
void Dump(uint64_t hash, const TextureDesc& d, Host host, const std::vector<uint8_t>& px, uint32_t pitch);

// how many replacements the folder has, and how many were used / dumped this run (for the menu)
struct Stats {
    uint32_t available = 0, used = 0, dumped = 0;
};
Stats GetStats();

}  // namespace gpu::pack
