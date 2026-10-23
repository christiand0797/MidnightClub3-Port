#pragma once
// HD movies: a title's Bink movie drawn into a texture can be shown from video_hd/<path>.mp4 (an upscale) instead.
// The title keeps decoding the Bink as before, so its timing, audio and frame count stay the console's; the host
// decodes the same frame of the HD file (Media Foundation) and the renderer binds it in place of the texture.
#include <cstdint>
#include <functional>
#include <string>

namespace gpu::video {

bool Enabled();  // settings: graphics.hd_movies

// the title opened the movie at path (as it named it, e.g. d:\video\mc3intro.bik) into the texture whose header is
// at header_va; false when there is no HD file for it
bool Open(uint32_t header_va, const std::string& path);
void Frame(uint32_t header_va, uint32_t frame);  // the Bink frame now in the texture, 1 based
void Close(uint32_t header_va);

// the renderer, for a texture at data_va (TextureDesc::data): true when an HD movie stands in for it. fn sees the
// picture (BGRA, width, height) when it changed since serial, which is then updated.
bool Playing(uint32_t data_va);
bool Picture(uint32_t data_va, uint64_t& serial, const std::function<void(const uint8_t*, uint32_t, uint32_t)>& fn);

}  // namespace gpu::video
