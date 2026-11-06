#include "gpu/draw_setup.h"

#include <array>
#include <cmath>
#include <cstring>

#include "gpu/fast_hash.h"
#include "gpu/texture.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"

namespace gpu {

float Bits(uint32_t v) {
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}

void Color(uint32_t argb, float* o) {
    o[0] = ((argb >> 16) & 255) / 255.0f;
    o[1] = ((argb >> 8) & 255) / 255.0f;
    o[2] = (argb & 255) / 255.0f;
    o[3] = (argb >> 24) / 255.0f;
}

uint32_t SimpleState(const State& s, uint32_t method) {
    return s.simple[method >> 2];
}

ShaderChoice ClassifyShaders(const State& s, const Assembled& a, uint32_t count) {
    ShaderChoice sh;
    // a shader made from a declaration alone (no program) is the fixed function pipeline on those inputs
    sh.prog = (s.vs_handle & 1) && s.vs && !s.vs->program.empty();
    // up to a quad with no depth test: it may be a pass over the whole target, see QuadHlsl; points never are
    bool points = a.topology == Topology::Points;
    sh.quad = count <= kQuadVertices && !s.rs[kRsZEnable] && !points;
    sh.fixed_transform = !sh.prog && !a.pretransformed;
    sh.sprite = points && sh.prog && s.rs[kRsPointSpriteEnable];
    if (!sh.prog) sh.fk = MakeFixedKey(s);
    return sh;
}

// the program's hash is cached in the guest shader object
void EnsureProgramHash(const State& s) {
    if (!s.vs->hash) s.vs->hash = Hash(s.vs->program.data(), s.vs->program.size() * 4) | 1;
}

// vertex shader cache key salts
constexpr uint64_t kDeclarationSalt = 0x10000;    // fixed function seed, above the 16 bit register mask
constexpr uint64_t kPretransformedSalt = 0x5A5A;  // screen space fixed function vertices, apart from untransformed
constexpr uint64_t kQuadSalt = 0xC3C30000;        // the QuadHlsl variant, apart from the plain shader
constexpr uint64_t kSpriteSalt = 0x3C3C0000;      // the SpriteSizeHlsl variant

uint64_t VertexKey(const State& s, const Assembled& a, const ShaderChoice& sh) {
    uint64_t seed = sh.prog ? s.vs->hash ^ a.regs : Hash(&s.vs_handle, 4, a.regs | kDeclarationSalt);
    uint64_t vkey = Hash(&a.decode, 8, seed);
    if (!sh.prog && a.pretransformed) vkey ^= kPretransformedSalt;
    if (sh.quad) vkey ^= kQuadSalt;
    if (sh.sprite) vkey ^= kSpriteSalt;
    if (!sh.prog) vkey = Hash(&sh.fk, sizeof(sh.fk), vkey);
    return vkey;
}

bool OffsetsPolygons(const State& s, Topology topology) {
    return topology == Topology::Triangles && SimpleState(s, kMSolidOffsetEnable);
}

// alpha test off: compare always, in CmpIndex terms like the enabled test
constexpr float kAlphaTestAlways = 7.0f;

// D3DRS_FOGTABLEMODE values
constexpr uint32_t kFogTableNone = 0;
constexpr float kFogTableLinear = 3.0f;

// table mode none under a vertex program: oFog is the blend factor, passed on as linear fog from 1 to 0
void FillFogConstants(const State& s, bool vertex_program, DrawConstants& c) {
    if (!s.rs[kRsFogEnable]) return;
    if (vertex_program && s.rs[kRsFogTableMode] == kFogTableNone) {
        c.fog[0] = 1.0f;
        c.fog[1] = 0.0f;
        c.fog[2] = 0.0f;
        c.fog[3] = kFogTableLinear;
        return;
    }
    c.fog[0] = Bits(s.rs[kRsFogStart]);
    c.fog[1] = Bits(s.rs[kRsFogEnd]);
    c.fog[2] = Bits(s.rs[kRsFogDensity]);
    c.fog[3] = float(s.rs[kRsFogTableMode]);
}

void FillSurfaceConstants(const State& s, GuestExtent size, DrawConstants& c) {
    c.surf[0] = float(size.w);
    c.surf[1] = float(size.h);
    c.surf[2] = 1.0f / s.zscale;
    Color(s.rs[kRsFogColor], c.fog_color);
    c.misc[0] = float(SimpleState(s, kMAlphaRef) & 0xFF);
    c.misc[1] = SimpleState(s, kMAlphaTestEnable) ? float(CmpIndex(SimpleState(s, kMAlphaFunc))) : kAlphaTestAlways;
    c.misc[2] = float(CmpIndex(s.rs[kRsShadowFunc]));
    c.misc[3] = s.rs[kRsSpecularEnable] ? 1.0f : 0.0f;
}

void FillCombinerConstants(const State& s, DrawConstants& c) {
    for (int i = 0; i < 8; i++) {
        Color(SimpleState(s, kMPsConstant0 + i * 4), c.pc[i * 2]);
        Color(SimpleState(s, kMPsConstant1 + i * 4), c.pc[i * 2 + 1]);
    }
    Color(SimpleState(s, kMPsFinalConstant0), c.pc[16]);
    Color(SimpleState(s, kMPsFinalConstant1), c.pc[17]);
    Color(s.rs[kRsTextureFactor], c.tfactor);
    std::memcpy(c.vattr, s.vattr.data(), sizeof(c.vattr));  // inline vertex attributes
}

void MatMul(const float* x, const float* y, float* o) {
    for (int r = 0; r < 4; r++) {
        for (int col = 0; col < 4; col++) {
            o[r * 4 + col] = x[r * 4] * y[col] + x[r * 4 + 1] * y[4 + col] + x[r * 4 + 2] * y[8 + col] +
                             x[r * 4 + 3] * y[12 + col];
        }
    }
}

// HLSL mul(row vector, matrix) with column major packing wants the transpose
void Transpose(const float* m, float* o) {
    for (int r = 0; r < 4; r++) {
        for (int col = 0; col < 4; col++) o[col * 4 + r] = m[r * 4 + col];
    }
}

static void InverseTranspose3x3(const float* wv, float* n) {
    float det = wv[0] * (wv[5] * wv[10] - wv[6] * wv[9]) - wv[1] * (wv[4] * wv[10] - wv[6] * wv[8]) +
                wv[2] * (wv[4] * wv[9] - wv[5] * wv[8]);
    float id = det != 0 ? 1.0f / det : 0.0f;
    for (int r = 0; r < 3; r++) {
        for (int col = 0; col < 3; col++) {
            int r1 = (r + 1) % 3;
            int r2 = (r + 2) % 3;
            int c1 = (col + 1) % 3;
            int c2 = (col + 2) % 3;
            n[r * 4 + col] = (wv[r1 * 4 + c1] * wv[r2 * 4 + c2] - wv[r1 * 4 + c2] * wv[r2 * 4 + c1]) * id;
        }
    }
}

void FillTransformConstants(const State& s, GuestExtent size, DrawConstants& c) {
    uint32_t gw = size.w;
    uint32_t gh = size.h;
    // world, view, projection, then the viewport inside the target
    float wv[16];
    float wvp[16];
    MatMul(s.transforms[kTsWorld].data(), s.transforms[kTsView].data(), wv);
    MatMul(wv, s.transforms[kTsProjection].data(), wvp);
    const auto& v = s.viewport;
    float sx = float(v.w) / gw;
    float sy = float(v.h) / gh;
    float ox = (2.0f * v.x + v.w) / gw - 1;
    float oy = 1 - (2.0f * v.y + v.h) / gh;
    float vpm[16] = {sx, 0, 0, 0, 0, sy, 0, 0, 0, 0, v.max_z - v.min_z, 0, ox, oy, v.min_z, 1};
    float full[16];
    MatMul(wvp, vpm, full);
    Transpose(full, c.wvp);
    Transpose(wv, c.wv);
    // normals take the inverse transpose of the upper 3x3
    float n[16] = {};
    InverseTranspose3x3(wv, n);
    Transpose(n, c.wvn);
    for (int t = 0; t < 4; t++) Transpose(s.transforms[kTsTexture0 + size_t(t)].data(), c.tm[t]);
    // vertex blending: the same for each of the four world matrices
    if (s.rs[kRsVertexBlend] & 7) {
        for (size_t k = 0; k < 4; k++) {
            float bwv[16], bwvp[16], bfull[16], bn[16] = {};
            MatMul(s.transforms[kTsWorld + k].data(), s.transforms[kTsView].data(), bwv);
            MatMul(bwv, s.transforms[kTsProjection].data(), bwvp);
            MatMul(bwvp, vpm, bfull);
            Transpose(bfull, c.bwvp[k]);
            Transpose(bwv, c.bwv[k]);
            InverseTranspose3x3(bwv, bn);
            Transpose(bn, c.bwvn[k]);
        }
    }
}

// view, projection and viewport as FillTransformConstants makes them, without a world: the camera's clip = world * out
void CameraMatrix(const State& s, GuestExtent size, float out[16]) {
    float vp[16];
    MatMul(s.transforms[kTsView].data(), s.transforms[kTsProjection].data(), vp);
    const auto& v = s.viewport;
    float sx = float(v.w) / size.w;
    float sy = float(v.h) / size.h;
    float ox = (2.0f * v.x + v.w) / size.w - 1;
    float oy = 1 - (2.0f * v.y + v.h) / size.h;
    float vpm[16] = {sx, 0, 0, 0, 0, sy, 0, 0, 0, 0, v.max_z - v.min_z, 0, ox, oy, v.min_z, 1};
    MatMul(vp, vpm, out);
}

// a general 4x4 inverse (row major), false for a singular matrix
bool Invert4x4(const float m[16], float out[16]) {
    float inv[16];
    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
    float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    if (!std::isfinite(det) || std::fabs(det) < 1e-20f) return false;
    for (int i = 0; i < 16; i++) out[i] = inv[i] / det;
    return true;
}

// D3DLIGHT8 as floats: Type, Diffuse[4], Specular[4], Ambient[4], Position[3], Direction[3], Range, Falloff,
// Attenuation0 to 2, Theta, Phi
constexpr int kLightFloats = 26;
constexpr int kLightDiffuse = 1;
constexpr int kLightSpecular = 5;
constexpr int kLightAmbient = 9;
constexpr int kLightPosition = 13;
constexpr int kLightDirection = 16;
constexpr int kLightRange = 19;
constexpr int kLightFalloff = 20;
constexpr int kLightAttenuation0 = 21;
constexpr int kLightAttenuation1 = 22;
constexpr int kLightAttenuation2 = 23;
constexpr int kLightTheta = 24;
constexpr int kLightPhi = 25;

static void LightToView(const std::array<uint32_t, kLightFloats>& l, const float* vm, float (&o)[7][4]) {
    float f[kLightFloats];
    std::memcpy(f, l.data(), sizeof(f));
    for (int j = 0; j < 4; j++) {
        o[0][j] = f[kLightDiffuse + j];
        o[1][j] = f[kLightSpecular + j];
        o[2][j] = f[kLightAmbient + j];
    }
    const float* pos = f + kLightPosition;
    const float* dir = f + kLightDirection;
    for (int j = 0; j < 3; j++) {
        o[3][j] = pos[0] * vm[j] + pos[1] * vm[4 + j] + pos[2] * vm[8 + j] + vm[12 + j];
        o[4][j] = dir[0] * vm[j] + dir[1] * vm[4 + j] + dir[2] * vm[8 + j];
    }
    o[3][3] = f[kLightRange];
    o[4][3] = f[kLightFalloff];
    o[5][0] = f[kLightAttenuation0];
    o[5][1] = f[kLightAttenuation1];
    o[5][2] = f[kLightAttenuation2];
    o[6][0] = std::cos(f[kLightTheta] * 0.5f);
    o[6][1] = std::cos(f[kLightPhi] * 0.5f);
}

void FillLightConstants(const State& s, const FixedKey& fk, DrawConstants& c) {
    std::memcpy(c.mat, s.material.data(), sizeof(float) * 16);
    c.mat[4][0] = s.material[16];
    Color(s.rs[kRsAmbient], c.amb);
    // lights to view space
    const float* vm = s.transforms[kTsView].data();
    for (int i = 0; i < 8; i++) {
        if (!((fk.types >> (i * 2)) & 3)) continue;
        LightToView(s.lights[size_t(i)], vm, c.lt[i]);
    }
}

// the projection's near and far clip w as D3DDevice_SetTransform finds them
static WRange ProjectionWRange(const float* m) {
    float m33 = m[10];
    float m34 = m[11];
    float m43 = m[14];
    float m44 = m[15];
    if (m33 == m34 || m33 == 0) return {0, 1};
    return {m44 - m43 / m33 * m34, (m44 - m43) / (m33 - m34) * m34 + m44};
}

// NV097_SET_CLIP_MIN and MAX in buffer units, sent by D3DDevice_SetDepthClipPlanes
constexpr uint32_t kMClipMin = 0x394;
constexpr uint32_t kMClipMax = 0x398;
constexpr float kFallbackNear = 1.0f / 65536;  // of the far w, when no range is known

// Fixed function clip w is the host's view depth over the projection's range; a program's is in buffer units up to
// zscale, its near end the clip minimum the title sent
WRange DrawWRange(const State& s, bool program) {
    WRange r{0, s.zscale};
    if (!program) {
        r = ProjectionWRange(s.transforms[kTsProjection].data());
    } else if (SimpleState(s, kMClipMax)) {
        r.near_w = Bits(SimpleState(s, kMClipMin));
    }
    if (r.near_w <= 0 || r.near_w >= r.far_w) r.near_w = r.far_w * kFallbackNear;
    return r;
}

void FillDepthConstants(const State& s, bool program, Topology topology, DrawConstants& c) {
    bool offset = OffsetsPolygons(s, topology);
    float units = offset ? Bits(SimpleState(s, kMDepthOffset)) : 0;
    if (s.rs[kRsZEnable] != kZUseW) {
        c.zparams[2] = units / s.zscale;
        return;
    }
    WRange r = DrawWRange(s, program);
    c.zparams[0] = r.near_w / (r.far_w - r.near_w);
    c.zparams[1] = r.far_w;
    c.zparams[2] = program ? units : units * r.far_w / s.zscale;
}

// a P8 texture is decoded with the stage's palette: 256 colours, fewer by the size code in the top two bits
void ReadStagePalette(const State& s, int stage, const TextureDesc& d, StagePalette& palette) {
    constexpr uint32_t kFmtP8 = 0x0B;          // D3DFMT_P8
    constexpr uint32_t kPaletteCommon = 0;     // D3DPalette: Common, its top two bits the size code
    constexpr uint32_t kPaletteData = 4;       // D3DPalette: Data, physical address of the colours
    constexpr uint32_t kPaletteSizeShift = 30;
    if (d.format != kFmtP8 || !s.palettes[size_t(stage)]) return;
    uint32_t header = s.palettes[size_t(stage)];
    palette.va = mem::VirtualFromPhysical(kernel::R32(header + kPaletteData));
    uint32_t size_code = kernel::R32(header + kPaletteCommon) >> kPaletteSizeShift;
    size_t bytes = (std::size(palette.colours) >> size_code) * sizeof(uint32_t);
    std::memcpy(palette.colours, kernel::HostPtr(palette.va), bytes);
    palette.hash = Hash(palette.colours, sizeof(palette.colours));
}

// the XDK's screen space offset puts the console's positions half a pixel right and down of the host's, unless a
// program writes them without c-37; render target reads move back that far (TexUv)
float RenderTargetShift(const State& s, bool program, float target_scale) {
    bool offset = !program || ReadsViewportOffset(s.vs->program);
    return offset ? -0.5f * target_scale : 0.0f;
}

}  // namespace gpu
