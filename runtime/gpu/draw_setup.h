#pragma once
// A draw's setup that does not depend on the graphics API: which shaders, their cache keys, the pipeline state from the
// console registers and every DrawConstants value. The D3D12 backend (d3d12.cpp) and the browser's WebGPU backend
// (tools/web/webgpu_backend.cpp) both use it, so the browser draws from the same decisions as the local build.
#include <cstdint>

#include "gpu/shaders.h"
#include "gpu/state.h"
#include "gpu/vertex.h"

namespace gpu {

float Bits(uint32_t v);
void Color(uint32_t argb, float* o);
uint32_t SimpleState(const State& s, uint32_t method);
// D3DCMP_NEVER 0x200 to D3DCMP_ALWAYS 0x207 as 0 to 7 (anything else always)
inline uint32_t CmpIndex(uint32_t v) { return v >= 0x200 && v <= 0x207 ? v - 0x200 : 7; }

constexpr uint32_t kQuadVertices = 6;  // a quad drawn as two triangles
constexpr uint32_t kZUseW = 2;         // D3DZB_USEW
constexpr uint32_t kCullCw = 0x900;    // D3DCULL_CW
// D3DTRANSFORMSTATETYPE indices into s.transforms
constexpr size_t kTsView = 2;
constexpr size_t kTsProjection = 3;
constexpr size_t kTsTexture0 = 16;
constexpr size_t kTsWorld = 256;

// the render target's console size in pixels, or the depth buffer's without one
struct GuestExtent {
    uint32_t w;
    uint32_t h;
};

// a w buffer's clip range in the units of a draw's clip w
struct WRange {
    float near_w;
    float far_w;
};

// what a draw's shaders are made from
struct ShaderChoice {
    bool prog = false;  // a vertex program, else the fixed function pipeline
    bool quad = false;
    bool fixed_transform = false;  // fixed function on untransformed vertices: the shader reads matrices and lights
    bool sprite = false;           // a program's points drawn as point sprites (SpriteHlsl)
    FixedKey fk;
};
ShaderChoice ClassifyShaders(const State& s, const Assembled& a, uint32_t count);
// the program's hash is cached in the guest shader object
void EnsureProgramHash(const State& s);
uint64_t VertexKey(const State& s, const Assembled& a, const ShaderChoice& sh);

bool OffsetsPolygons(const State& s, Topology topology);
void FillFogConstants(const State& s, bool vertex_program, DrawConstants& c);
void FillSurfaceConstants(const State& s, GuestExtent size, DrawConstants& c);
void FillCombinerConstants(const State& s, DrawConstants& c);
void MatMul(const float* x, const float* y, float* o);
void Transpose(const float* m, float* o);
void FillTransformConstants(const State& s, GuestExtent size, DrawConstants& c);
// view, projection and viewport as FillTransformConstants makes them, without a world: the camera's clip = world * out
void CameraMatrix(const State& s, GuestExtent size, float out[16]);
bool Invert4x4(const float m[16], float out[16]);
void FillLightConstants(const State& s, const FixedKey& fk, DrawConstants& c);
WRange DrawWRange(const State& s, bool program);
// ReversedDepthHlsl's zparams (x the w buffer's scale, 0 for a z buffer; y its far w; z the polygon offset)
void FillDepthConstants(const State& s, bool program, Topology topology, DrawConstants& c);

// a P8 texture's palette: its colours, their address (part of the texture's key) and their hash, all 0 without one
struct StagePalette {
    uint32_t colours[256] = {};
    uint32_t va = 0;
    uint64_t hash = 0;
};

struct TextureDesc;
void ReadStagePalette(const State& s, int stage, const TextureDesc& d, StagePalette& palette);
// host pixels a render target read moves by (DrawConstants::rtshift), target_scale host pixels per console pixel
float RenderTargetShift(const State& s, bool program, float target_scale);

template <class Key>
void KeyBlend(const State& s, Key& k) {
    k.blend = SimpleState(s, kMBlendEnable) != 0;
    if (k.blend) {
        k.src = SimpleState(s, kMSrcBlend);
        k.dst = SimpleState(s, kMDestBlend);
        k.op = SimpleState(s, kMBlendOp);
    }
    uint32_t cw = SimpleState(s, kMColorWriteEnable);
    // console ARGB bytes to D3D12 RGBA bits
    k.wmask = ((cw >> 16) & 1) | ((cw >> 8) & 1) << 1 | (cw & 1) << 2 | ((cw >> 24) & 1) << 3;
}

template <class Key>
void KeyRasterizer(const State& s, const Assembled& a, Key& k) {
    uint32_t cull = s.rs[kRsCullMode];
    k.cull = cull != 0 && a.topology == Topology::Triangles;
    k.ccw = cull == kCullCw;  // culling clockwise leaves counter clockwise as the front
    k.fill = s.rs[kRsFillMode];
}

}  // namespace gpu
