#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "gpu/state.h"

// HLSL for the title's shaders and for its fixed function state, per the XDK's definitions of both
namespace gpu {

enum TexKind : uint8_t { kTexNone, kTex2D, kTex3D, kTexCube };

struct TexInfo {
    uint8_t kind = kTexNone;
    uint8_t rect = 0;    // linear texture, coordinates in texels
    uint8_t shadow = 0;  // depth texture: 1 D24, 2 D16
    uint8_t flags = 0;   // kTexWide, kTexTarget; bits 0 and 1 are a backend extension's
};
enum : uint8_t {
    kTexWide = 4,    // an upscaled target shrunk by the pass: each tap averaged over console texels
    kTexTarget = 8,  // a render target, read where the console's pixel offset puts it (DrawConstants::rtshift)
};

struct PixelKey {
    uint32_t programmable = 0;
    // combiners, straight from D3DPIXELSHADERDEF
    uint32_t count = 0, rgb_in[8] = {}, rgb_out[8] = {}, a_in[8] = {}, a_out[8] = {}, final_abcd = 0, final_efg = 0;
    uint32_t tex_modes = 0, dot_mapping = 0, input_texture = 0, compare_mode = 0;
    // fixed function stages: colorop, arg1, arg2, arg0, alphaop, arg1, arg2, arg0, result
    uint32_t ff[4][9] = {};
    TexInfo tex[4];
    uint32_t alphakill = 0;  // bit per stage
    uint32_t flat = 0, two_sided = 0;
    uint32_t proj = 0;       // fixed function: bit per stage sampled at coordinates divided by w
    uint32_t quad = 0;       // a screen quad: QuadHlsl
    uint32_t car_paint = 0;  // enhanced car paint: gpu/car_paint.h PixelHlsl over the generated shader
    uint32_t pbr = 0;        // stage 0 companion PBR maps bound on t4 (normal) and t5 (ORM+height)
    bool operator==(const PixelKey& o) const;
};

uint64_t Hash(const void* p, size_t n, uint64_t h = 1469598103934665603ull);

PixelKey MakePixelKey(const State& s, const TexInfo tex[4]);

// vertex program microcode to HLSL, regs is the bit mask of v registers the draw supplies
// decode: VertexDecode per register, 4 bits each, for elements the input assembler hands over raw
std::string VertexProgramHlsl(const std::vector<uint32_t>& program, uint16_t regs, uint64_t decode = 0);
// the program reads c-37, the viewport offset that carries the XDK's half pixel screen space offset
bool ReadsViewportOffset(const std::vector<uint32_t>& program);
// fixed function vertex state a shader is built for
struct FixedKey {
    uint32_t light = 0;    // bit 0 lighting, 1 specular, 2 local viewer, 3 normalize, then D3DMCS 2 bits each from bit 8:
                           // diffuse, specular, ambient, emissive (material when the vertex lacks that colour)
    uint32_t types = 0;    // D3DLIGHTTYPE per light, 2 bits each, 0 off
    uint32_t tex[4] = {};  // per stage: coordinate set, texgen at bit 4, D3DTTFF count at bit 8, projected bit 11, set size at bit 12
    uint32_t blend = 0;    // D3DRS_VERTEXBLEND (the NV2A skin mode): 1 3 5 two to four matrices, the last weight implied;
                           // 2 4 6 two to four matrices, every weight given
};
FixedKey MakeFixedKey(const State& s);
std::string FixedVertexHlsl(uint16_t regs, bool pretransformed, uint64_t decode = 0, const FixedKey& f = {});
std::string PixelHlsl(const PixelKey& k);
// a screen quad's vertex or pixel shader: texture coordinates as the quad was drawn, before its far edges were pulled out
std::string QuadHlsl(std::string s);
// the geometry shader that draws a point as its console pixel
std::string PointHlsl();

// HLSL a backend extension adds; the defaults write the console path
struct ShaderHooks {
    virtual ~ShaderHooks() = default;
    // the cbuffer line for DrawConstants::ext
    virtual std::string Constants() const;
    // float4 shadow(Texture2D<float> t, SamplerComparisonState s, float2 uv, float z, int fmt, float2 px, float2 size)
    virtual std::string Shadow() const;
    // what spreadtap scales a render target's colour by
    virtual std::string ReadScale(const TexInfo&) const { return "1"; }
    // a cube sampled along dir at stage i, empty for a plain sample
    virtual std::string Cube(const TexInfo&, int, const std::string&) const { return {}; }
    // after stage i's fetch
    virtual std::string Fetched(const TexInfo&, int) const { return {}; }
    // outc to the target, after the alpha test
    virtual std::string Output() const;
};
inline const ShaderHooks* g_shader_hooks = nullptr;

// the constant buffer layout both stages share, register b1
struct DrawConstants {
    float surf[4];      // target width, height, 1 / depth scale, 0
    float fog[4];       // start, end, density, mode
    float fog_color[4];
    float misc[4];      // alpha ref, alpha func, shadow func, specular
    float pc[18][4];    // combiner c0 c1 per stage, then final c0 c1
    float tfactor[4];
    float texsize[4][4];  // width, height, then a uv shift only renderer copies from before rtshift set
    float bump[4][4];   // 00 01 10 11
    float bumpl[4][4];  // scale offset
    float wvp[16];      // fixed function
    float ext[4];       // a backend extension's, declared by ShaderHooks::Constants as one float4: it must stay 16 bytes or
                        // every constant after it is read from the wrong place
    float spread[4][4]; // kTexWide: tap step in uv, taps per axis
    // fixed function, lit in view space
    float wv[16];
    float wvn[16];       // normals: inverse transpose of wv
    float tm[4][16];     // texture transforms
    float mat[5][4];     // material diffuse, ambient, specular, emissive, power
    float amb[4];        // D3DRS_AMBIENT
    float lt[8][7][4];   // per light: diffuse, specular, ambient, position range, direction falloff, attenuation, cos theta/2 phi/2
    float rtshift[4];    // per kTexTarget stage: host pixels its reads move along x and y, negative up and left
    float vattr[16][4];  // State::vattr, read for the vertex registers a draw does not supply
    float sblend[4];     // x: 1 the positive, 2 the negative part of the output read as signed bytes (signed blends);
                         // y: output alpha scale less 1, z: output colour and alpha scale less 1 (screen effects);
                         // w: colour times alpha scale less 1, alpha raised first (D3D12Backend::ScaleTextureEffect)
    // vertex blending: world 0 to 3 (D3DTS_WORLDMATRIX(n)) by view and projection, by view, and its normal matrix
    float bwvp[4][16];
    float bwv[4][16];
    float bwvn[4][16];
    float carpaint[4];   // enhanced car paint (gpu/car_paint.h): relight strength, highlights, roughness, 1; last, so
                         // nothing before it moves
    float pbr[4];        // PBR parameters: normal_strength, ao_strength, roughness (factor or -(1+override)), metallic
    float pbr2[4];       // PBR parameters: parallax_height, base_color.r, base_color.g, base_color.b
};

}  // namespace gpu
