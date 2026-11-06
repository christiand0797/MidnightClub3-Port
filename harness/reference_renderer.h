#pragma once
#include <vector>
#include <cstdint>

#include "gpu/state.h"

// CPU renderer for the headless harness: draws the title's calls into its own surfaces in guest memory, so a frame
// can be looked at without a GPU. Follows the same decoding of the XDK state as the SDK's shader translator.
namespace reference {

void BeginFrame();  // drops decoded textures and restarts the draw count
void Clear(const gpu::State& s, uint32_t count, const uint8_t* rects, uint32_t flags, uint32_t color, float z, uint32_t stencil);
void Copy(const gpu::Surface& src, const gpu::Surface& dst, uint32_t count, uint32_t rects_va, uint32_t points_va);
void Draw(const gpu::State& s, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices);
// 24 bit TGA of a surface's colour
bool WriteTga(const gpu::Surface& s, const char* path);
// the surface as RGBA8, top row first (the browser build shows it on a canvas)
bool ReadRgba(const gpu::Surface& s, std::vector<uint8_t>& out, uint32_t& w, uint32_t& h);

// GPU hand-off (browser build, tools/web/webgpu_backend.cpp): with a sink set, Draw runs its vertex stage as usual and
// hands the transformed triangles, the textures and the state to the sink instead of rasterizing on the CPU.
struct GpuTexture {
    uint32_t key = 0;              // texture header address, 0 for none
    uint32_t w = 0, h = 0;
    uint64_t hash = 0;             // of the decoded texels: the sink uploads only when it changes
    const uint8_t* rgba = nullptr; // w * h * 4, level 0 (face 0 of a cube)
    uint32_t kind = 0, rect = 0, address_u = 1, address_v = 1, linear = 1;
    uint32_t target = 0;           // the texture's memory is a render target the GPU drew: bind that (key = its address)
};
// per vertex, kVertexFloats floats: screen x y (target pixels), z (0..zscale), w; diffuse rgba; specular rgba;
// t0..t3 xyzw; fog amount
constexpr uint32_t kVertexFloats = 32;
struct GpuDraw {
    uint32_t rt = 0, rt_w = 0, rt_h = 0;  // colour target (guest address), 0 when it is not drawn
    uint32_t zs = 0;                      // depth target, 0 when depth and stencil are off
    float zscale = 1;
    int left = 0, top = 0, right = 0, bottom = 0;  // viewport within scissor, target pixels
    const float* verts = nullptr;
    uint32_t vertex_count = 0;
    const uint32_t* indices = nullptr;  // triangle list
    uint32_t index_count = 0;
    // raster state as the pushbuffer holds it (GL enums); color_mask in ARGB bytes, as kMColorWriteEnable
    uint32_t blend = 0, src = 0, dst = 0, op = 0, blend_color = 0, color_mask = 0;
    uint32_t ztest = 0, zwrite = 0, zfunc = 0, cull = 0, front = 0;
    uint32_t atest = 0, afunc = 0, aref = 0, alphakill = 0, programmable = 0, flat = 0;
    uint32_t ff[4][9] = {};  // fixed function stages: colorop arg1 arg2 arg0 alphaop arg1 arg2 arg0 result
    uint32_t tfactor = 0, fog_color = 0;
    GpuTexture tex[4];
};
using GpuSink = void (*)(const GpuDraw&);
void SetGpuSink(GpuSink sink);
// comparison (browser build): dual also rasterizes on the CPU after handing a draw to the sink; limit draws only the
// first n draws of each frame (0: all), on both sides
void SetGpuCompare(bool dual, uint32_t limit);
// a surface's size in pixels (false when it cannot be read); a copy's destination becomes a GPU target
bool SurfaceSize(const gpu::Surface& s, uint32_t& w, uint32_t& h);
void MarkGpuTarget(uint32_t address, uint32_t width);
// GPU targets are keyed by physical offset: the same 64 MB of RAM shows at physical and contiguous virtual addresses
inline uint32_t GpuKey(uint32_t address) { return address & 0x03FFFFFF; }

}  // namespace reference
