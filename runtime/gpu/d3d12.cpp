// The title's Direct3D 8 calls drawn with Direct3D 12
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <bit>
#include <memory>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

#include <imgui.h>
#include <backends/imgui_impl_dx12.h>
#include <backends/imgui_impl_win32.h>

#include "app/app.h"
#include "core/diag.h"
#include "core/log.h"
#include "core/host_path.h"
#include "gpu/backend.h"
#include "gpu/d3d12_diagnostics.h"
#include "gpu/ext.h"
#include "gpu/shaders.h"
#include "gpu/skinning_capture.h"
#include "gpu/capture_directory.h"
#include "gpu/capture_control.h"
#include "gpu/car_paint.h"
#include "gpu/pbr_controls.h"
#include "gpu/pbr_library_cache.h"
#include "gpu/pbr_surface.h"
#include "gpu/pbr_fixed_mesh.h"
#include "gpu/pbr_device.h"
#include "gpu/pbr_scene.h"
#include "gpu/pbr_observation.h"
#include "gpu/pbr_legacy_surface.h"
#include "gpu/draw_evidence.h"
#include "gpu/photo_capture_policy.h"
#include "gpu/photo_capture_io.h"
#include "gpu/photo_capture_shader.h"
#include "gpu/texture.h"
#include "gpu/texture_pack.h"
#include "gpu/video_hd.h"
#include "gpu/texturedumper.h"
#include "gpu/vertex.h"
#include "kernel/kernel.h"

// Forward declaration for enhanced render scale
namespace app {
float EnhancedRenderScale(uint32_t guest_height);
}

using Microsoft::WRL::ComPtr;

namespace gpu {

namespace {

constexpr int kFrames = 3;
constexpr uint64_t kStaleFrames = 8;  // frames a target may go unused and still be read back
constexpr uint64_t kUploadSize = 64ull << 20;
// a draw starting with less than this left in the frame's ring waits for the GPU and starts the ring over
constexpr uint64_t kUploadReserve = 8ull << 20;
constexpr uint32_t kSrvCount = 3 * 32768;
constexpr uint32_t kSamplerCount = 2048;
constexpr uint32_t kRtvCount = 1024;
constexpr uint32_t kDsvCount = 256;
constexpr DXGI_FORMAT kBgra = DXGI_FORMAT_B8G8R8A8_UNORM;
constexpr uint32_t kQueries = 8192;  // occlusion queries in flight, several frames of counted draws
constexpr uint32_t kNoQuery = ~0u;

// Failed command submission cannot be recovered by resetting its allocator or upload ring: the GPU may still
// own them. Keep the HRESULT in the release log and stop this render path instead of waiting on an unsignalled
// fence or reusing live resources. Device removal supplies the more specific reason when one is available.
void CheckGpu(ID3D12Device* device, HRESULT result, const char* operation) {
    if (SUCCEEDED(result)) return;
    const HRESULT removed = device->GetDeviceRemovedReason();
    LOGE("d3d12: %s failed (0x%08lX), device removal reason 0x%08lX", operation,
         static_cast<unsigned long>(result), static_cast<unsigned long>(removed));
    logx::Flush();
    throw std::runtime_error("Direct3D 12 command submission failed; see mc3.log for the operation and HRESULT.");
}

// perf diagnostic: where a frame's time goes, logged once a second with diag::Perf
using PerfClock = std::chrono::steady_clock;
double PerfMs(PerfClock::time_point t) {
    return std::chrono::duration<double, std::milli>(PerfClock::now() - t).count();
}
struct Perf {
    // milliseconds
    double draw = 0;
    double assemble = 0;
    double bind = 0;
    double sig = 0;
    double texup = 0;
    double compile = 0;
    double clear = 0;
    double copy = 0;
    double present = 0;
    double wait = 0;
    double swap = 0;
    double sync = 0;
    // counts and bytes
    uint64_t draws = 0;
    uint64_t verts = 0;
    uint64_t upload = 0;
    uint64_t overflow = 0;
    uint64_t sigs = 0;
    uint64_t sig_bytes = 0;
    uint64_t texups = 0;
    uint64_t texup_bytes = 0;
    uint64_t newtex = 0;
    uint64_t targets = 0;
    uint64_t rtups = 0;
    uint64_t shrinks = 0;
    uint64_t syncs = 0;
    uint64_t psos = 0;
    uint64_t shaders = 0;
    uint64_t raw = 0;
    uint64_t lates = 0;
};
Perf g_perf;
// the dedicated PBR path's own share, logged every ten seconds while it draws (release builds keep LOGR)
struct PbrPerf {
    double total = 0, mesh = 0, plan = 0, record = 0;  // ms
    uint64_t attempts = 0, drawn = 0, frames = 0;
};
PbrPerf g_pbr_perf;
// 64-bit content hash for the PBR mesh cache key: eight bytes a step, so hashing a draw's vertex bytes costs about
// what copying them does
uint64_t MixHash(const void* data, size_t n, uint64_t h) {
    constexpr uint64_t m = 0x9E3779B97F4A7C15ull;
    const auto* p = static_cast<const uint8_t*>(data);
    for (; n >= 8; p += 8, n -= 8) {
        uint64_t v;
        std::memcpy(&v, p, 8);
        h = (h ^ v) * m;
        h ^= h >> 32;
    }
    uint64_t v = 0;
    std::memcpy(&v, p, n);
    h = (h ^ v ^ n) * m;
    return h ^ (h >> 29);
}
struct PerfScope {
    double& acc;
    bool on = diag::Perf();
    PerfClock::time_point t = on ? PerfClock::now() : PerfClock::time_point{};
    ~PerfScope() {
        if (on) acc += PerfMs(t);
    }
};


// Diagnostic-only opt-in: selection strings never alter the draw/shader path.
std::string DrawEvidenceEnv(const char* name) {
    char value[128] = {};
    DWORD n = GetEnvironmentVariableA(name, value, sizeof(value));
    if (!n) return {};
    return n < sizeof(value) ? std::string(value, n) : std::string("invalid-too-long");
}

const char* kBlitHlsl = R"(
cbuffer B : register(b1) { float4 uvrect; float4 opts; };
Texture2D img : register(t0);
Texture2D lut : register(t1);
SamplerState ls : register(s0);
SamplerState ps : register(s1);
struct O { float4 p : SV_Position; float2 uv : TEXCOORD0; };
O vs(uint id : SV_VertexID) {
    O o;
    float2 t = float2((id << 1) & 2, id & 2);
    o.p = float4(t * float2(2, -2) + float2(-1, 1), 0, 1);
    o.uv = uvrect.xy + t * uvrect.zw;
    return o;
}
float4 psmain(O i) : SV_Target {
    float4 c = 0;
    int n = max((int)opts.z, 1);
    float2 px = fwidth(i.uv);
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++) c += img.SampleLevel(ls, i.uv + px * ((float2(x, y) + 0.5) / n - 0.5), 0);
    c /= n * n;
    c.rgb *= opts.w;
    if (opts.x > 0) c.rgb = float3(lut.Sample(ps, float2(c.r, 0.5)).r, lut.Sample(ps, float2(c.g, 0.5)).g, lut.Sample(ps, float2(c.b, 0.5)).b);
    return float4(c.rgb, opts.y > 0 ? c.a : 1);
}
)";

// Depth is reversed in a 32 bit float buffer: 1 at the near clip, 0 at the far one, where a float keeps its finest
// steps; a 24 bit buffer storing z / w lost the steps that told distant surfaces apart
constexpr DXGI_FORMAT kDepthTypeless = DXGI_FORMAT_R32G8X24_TYPELESS;
constexpr DXGI_FORMAT kDepthFormat = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
constexpr DXGI_FORMAT kDepthRead = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
constexpr float kDepthFar = 0;

const char* kDepthHlsl = R"(
Texture2D<float> img : register(t0);
SamplerState ps : register(s1);
struct O { float4 p : SV_Position; float2 uv : TEXCOORD0; };
O vs(uint id : SV_VertexID) {
    O o;
    o.uv = float2((id << 1) & 2, id & 2);
    o.p = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
float psmain(O i) : SV_Depth { return img.SampleLevel(ps, i.uv, 0); }
)";

// the present's picture chain (app::Settings::graphics): grade, FXAA, AMD CAS; each a full screen pass
const char* kPostHlsl = R"(
cbuffer P : register(b1) { float4 p0; float4 p1; float4 p2; float4 p3; };
Texture2D t0 : register(t0);
Texture2D t1 : register(t1);
SamplerState ls : register(s0);
SamplerState ps : register(s1);
struct O { float4 p : SV_Position; float2 uv : TEXCOORD0; };
O vs(uint id : SV_VertexID) {
    O o;
    float2 t = float2((id << 1) & 2, id & 2);
    o.p = float4(t * float2(2, -2) + float2(-1, 1), 0, 1);
    o.uv = t;
    return o;
}
float Luma(float3 c) { return dot(c, float3(0.299, 0.587, 0.114)); }
// p0: output texel size, source taps per axis, console gamma ramp; p1: brightness contrast saturation vibrance;
// p2.x: gamma exponent. Out: graded colour, luma in alpha for FXAA
float4 grade(O i) : SV_Target {
    int n = max((int)p0.z, 1);
    float4 c = 0;
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++) c += t0.SampleLevel(ls, i.uv + p0.xy * ((float2(x, y) + 0.5) / n - 0.5), 0);
    c /= n * n;
    float3 col = saturate(c.rgb);
    if (p0.w > 0)
        col = float3(t1.SampleLevel(ps, float2(col.r, 0.5), 0).r, t1.SampleLevel(ps, float2(col.g, 0.5), 0).g,
                     t1.SampleLevel(ps, float2(col.b, 0.5), 0).b);
    col = pow(saturate(col), p2.x);
    col = (col - 0.5) * p1.y + 0.5 + p1.x;
    col = lerp(Luma(col).xxx, col, p1.z);
    float sat = max(col.r, max(col.g, col.b)) - min(col.r, min(col.g, col.b));
    col = lerp(Luma(col).xxx, col, 1 + p1.w * (1 - saturate(sat)));
    col = saturate(col);
    return float4(col, Luma(col));
}
// FXAA (Lottes' compact PC version) on luma in alpha. p0: texel size, strength
float4 fxaa(O i) : SV_Target {
    float2 r = p0.xy;
    float3 m = t0.SampleLevel(ls, i.uv, 0).rgb;
    float lNW = t0.SampleLevel(ls, i.uv + float2(-1, -1) * r, 0).a;
    float lNE = t0.SampleLevel(ls, i.uv + float2(1, -1) * r, 0).a;
    float lSW = t0.SampleLevel(ls, i.uv + float2(-1, 1) * r, 0).a;
    float lSE = t0.SampleLevel(ls, i.uv + float2(1, 1) * r, 0).a;
    float lM = Luma(m);
    float lMin = min(lM, min(min(lNW, lNE), min(lSW, lSE)));
    float lMax = max(lM, max(max(lNW, lNE), max(lSW, lSE)));
    float2 dir = float2(-((lNW + lNE) - (lSW + lSE)), (lNW + lSW) - (lNE + lSE));
    float reduce = max((lNW + lNE + lSW + lSE) * (0.25 / 8.0), 1.0 / 128.0);
    float rcpMin = 1.0 / (min(abs(dir.x), abs(dir.y)) + reduce);
    dir = clamp(dir * rcpMin, -8.0, 8.0) * r;
    float3 a = 0.5 * (t0.SampleLevel(ls, i.uv + dir * (1.0 / 3.0 - 0.5), 0).rgb +
                      t0.SampleLevel(ls, i.uv + dir * (2.0 / 3.0 - 0.5), 0).rgb);
    float3 b = a * 0.5 + 0.25 * (t0.SampleLevel(ls, i.uv - dir * 0.5, 0).rgb + t0.SampleLevel(ls, i.uv + dir * 0.5, 0).rgb);
    float lb = Luma(b);
    float3 col = lerp(m, (lb < lMin || lb > lMax) ? a : b, p0.z);
    return float4(col, Luma(col));
}
// AMD FidelityFX CAS (MIT), 3x3: p0: texel size, sharpness 0..1
float4 cas(O i) : SV_Target {
    float2 r = p0.xy;
    float3 a = t0.SampleLevel(ps, i.uv + float2(-1, -1) * r, 0).rgb;
    float3 b = t0.SampleLevel(ps, i.uv + float2(0, -1) * r, 0).rgb;
    float3 c = t0.SampleLevel(ps, i.uv + float2(1, -1) * r, 0).rgb;
    float3 d = t0.SampleLevel(ps, i.uv + float2(-1, 0) * r, 0).rgb;
    float3 e = t0.SampleLevel(ps, i.uv, 0).rgb;
    float3 f = t0.SampleLevel(ps, i.uv + float2(1, 0) * r, 0).rgb;
    float3 g = t0.SampleLevel(ps, i.uv + float2(-1, 1) * r, 0).rgb;
    float3 h = t0.SampleLevel(ps, i.uv + float2(0, 1) * r, 0).rgb;
    float3 k = t0.SampleLevel(ps, i.uv + float2(1, 1) * r, 0).rgb;
    float3 mn = min(min(min(d, e), min(f, b)), h);
    mn += min(mn, min(min(a, c), min(g, k)));
    float3 mx = max(max(max(d, e), max(f, b)), h);
    mx += max(mx, max(max(a, c), max(g, k)));
    float3 amp = sqrt(saturate(min(mn, 2.0 - mx) / max(mx, 1e-5)));
    float3 w = amp * (-1.0 / lerp(8.0, 5.0, saturate(p0.z)));
    float3 col = saturate((b * w + d * w + f * w + h * w + e) / (1.0 + 4.0 * w));
    return float4(col, Luma(col));
}
)";

const char* kClearHlsl = R"(
cbuffer B : register(b1) { float4 colour; };
float4 vs(uint id : SV_VertexID) : SV_Position {
    float2 t = float2((id << 1) & 2, id & 2);
    return float4(t * float2(2, -2) + float2(-1, 1), 0, 1);
}
float4 psmain() : SV_Target { return colour; }
)";

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

// D3DBLEND, the console uses GL values
constexpr uint32_t kBlendDestColor = 0x306;  // D3DBLEND_DESTCOLOR

D3D12_BLEND Blend(uint32_t v, bool alpha) {
    switch (v) {
        case 0: return D3D12_BLEND_ZERO;
        case 1: return D3D12_BLEND_ONE;
        case 0x300: return alpha ? D3D12_BLEND_SRC_ALPHA : D3D12_BLEND_SRC_COLOR;
        case 0x301: return alpha ? D3D12_BLEND_INV_SRC_ALPHA : D3D12_BLEND_INV_SRC_COLOR;
        case 0x302: return D3D12_BLEND_SRC_ALPHA;
        case 0x303: return D3D12_BLEND_INV_SRC_ALPHA;
        case 0x304: return D3D12_BLEND_DEST_ALPHA;
        case 0x305: return D3D12_BLEND_INV_DEST_ALPHA;
        case kBlendDestColor: return alpha ? D3D12_BLEND_DEST_ALPHA : D3D12_BLEND_DEST_COLOR;
        case 0x307: return alpha ? D3D12_BLEND_INV_DEST_ALPHA : D3D12_BLEND_INV_DEST_COLOR;
        case 0x308: return D3D12_BLEND_SRC_ALPHA_SAT;
        case 0x8001: case 0x8003: return D3D12_BLEND_BLEND_FACTOR;
        case 0x8002: case 0x8004: return D3D12_BLEND_INV_BLEND_FACTOR;
        default: return D3D12_BLEND_ONE;
    }
}

// the console's signed blend equations, which D3D12 lacks: Draw runs them as two passes
constexpr uint32_t kBlendOpAdd = 0x8006, kBlendOpRevSubtract = 0x800B;
constexpr uint32_t kBlendOpRevSubtractSigned = 0xF005, kBlendOpAddSigned = 0xF006;

// D3DBLENDOP, GL values
D3D12_BLEND_OP BlendOp(uint32_t v) {
    switch (v) {
        case 0x800A: return D3D12_BLEND_OP_SUBTRACT;
        case 0x800B: return D3D12_BLEND_OP_REV_SUBTRACT;
        case 0x8007: return D3D12_BLEND_OP_MIN;
        case 0x8008: return D3D12_BLEND_OP_MAX;
        default: return D3D12_BLEND_OP_ADD;
    }
}

// D3DCMP_NEVER 0x200 to D3DCMP_ALWAYS 0x207, D3D12's order from 1
D3D12_COMPARISON_FUNC Cmp(uint32_t v) {
    uint32_t n = v >= 0x200 && v <= 0x207 ? v - 0x200 : 7;
    return D3D12_COMPARISON_FUNC(n + 1);
}

// D3DSTENCILOP, GL values
D3D12_STENCIL_OP StencilOp(uint32_t v) {
    switch (v) {
        case 0: return D3D12_STENCIL_OP_ZERO;
        case 0x1E01: return D3D12_STENCIL_OP_REPLACE;
        case 0x1E02: return D3D12_STENCIL_OP_INCR_SAT;
        case 0x1E03: return D3D12_STENCIL_OP_DECR_SAT;
        case 0x150A: return D3D12_STENCIL_OP_INVERT;
        case 0x8507: return D3D12_STENCIL_OP_INCR;
        case 0x8508: return D3D12_STENCIL_OP_DECR;
        default: return D3D12_STENCIL_OP_KEEP;
    }
}

// D3DTADDRESS: 1 wrap, 2 mirror, 3 clamp, 4 border, 5 clamp to edge
D3D12_TEXTURE_ADDRESS_MODE Address(uint32_t v) {
    switch (v) {
        case 2: return D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
        case 3: case 5: return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        case 4: return D3D12_TEXTURE_ADDRESS_MODE_BORDER;
        default: return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    }
}

DXGI_FORMAT HostFormat(Host h) {
    switch (h) {
        case Host::BC1: return DXGI_FORMAT_BC1_UNORM;
        case Host::BC2: return DXGI_FORMAT_BC2_UNORM;
        case Host::BC3: return DXGI_FORMAT_BC3_UNORM;
        case Host::R32F: return DXGI_FORMAT_R32_FLOAT;
        default: return DXGI_FORMAT_B8G8R8A8_UNORM;
    }
}

constexpr uint32_t kFmtD16 = 0x2C;     // D3DFMT_D16
constexpr uint32_t kFmtLinD16 = 0x30;  // D3DFMT_LIN_D16

// TexInfo::shadow of a depth texture
uint8_t ShadowDepthKind(uint32_t format) { return format == kFmtD16 || format == kFmtLinD16 ? 2 : 1; }

// bytes per row of a 32 bit surface in guest memory, rounded to 64
uint32_t GuestPitch(uint32_t width) { return (width * 4 + 63) & ~63u; }

// rows of a texture upload start D3D12_TEXTURE_DATA_PITCH_ALIGNMENT apart
uint32_t UploadPitch(uint32_t row_bytes) { return (row_bytes + 255) & ~255u; }

D3D12_RESOURCE_DESC BufferDesc(uint64_t bytes) {
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = bytes;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return bd;
}

// one level, one sample: callers set Flags where they differ
D3D12_RESOURCE_DESC Texture2DDesc(DXGI_FORMAT fmt, uint64_t w, uint32_t h) {
    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = w;
    td.Height = h;
    td.DepthOrArraySize = 1;
    td.MipLevels = 1;
    td.Format = fmt;
    td.SampleDesc.Count = 1;
    return td;
}

// MemSig hashes a surface up to kSigWholeMax bytes whole, a bigger one as evenly spaced samples
constexpr uint32_t kSigWholeMax = 65536;
constexpr uint32_t kSigSamples = 64;
constexpr uint32_t kSigSampleBytes = 64;

uint64_t MemSig(uint32_t data, uint32_t total) {
    const uint8_t* p = static_cast<const uint8_t*>(kernel::HostPtr(data));
    uint64_t sig = Hash(&total, sizeof(total));
    g_perf.sigs++;
    g_perf.sig_bytes += total <= kSigWholeMax ? total : kSigSamples * kSigSampleBytes;
    if (total <= kSigWholeMax) return Hash(p, total, sig);
    for (uint32_t i = 0; i < kSigSamples; i++) {
        const uint8_t* sample = p + uint64_t(total - kSigSampleBytes) * i / (kSigSamples - 1);
        sig = Hash(sample, kSigSampleBytes, sig);
    }
    return sig;
}

// MemSig samples a big surface and misses small writes (XFONT text): after a Sync every byte is compared
bool Changed(const Target& t) {
    if (t.drawn || t.read.empty()) return false;
    uint32_t row = t.gw * 4;
    uint32_t pitch = GuestPitch(t.gw);
    const uint8_t* p = static_cast<const uint8_t*>(kernel::HostPtr(t.data));
    for (uint32_t y = 0; y < t.gh; y++) {
        if (std::memcmp(p + size_t(y) * pitch, &t.read[size_t(y) * row], row)) return true;
    }
    return false;
}

// a lock alone proves nothing, reads lock too: only changed bytes mean the CPU wrote it
bool LatchCpuWrite(Target& t) {
    uint64_t e = DirtyEpoch(t.data);
    if (e <= t.epoch) return false;
    t.epoch = e;
    uint64_t sig = MemSig(t.data, t.bytes);
    if (sig == t.sig && !Changed(t)) return false;
    t.sig = sig;
    return true;
}

struct Tex {
    ComPtr<ID3D12Resource> res;
    TextureDesc d;
    DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
    uint32_t levels = 1;
    uint64_t sig = 0;
    uint64_t frame = 0;
    uint64_t palette = 0;  // hash of the colours a P8 texture was decoded with
    uint8_t flags = 0;  // Extension::Decoded's
    bool replaced = false;  // showing a texture pack image, not the console's pixels
    bool pack_enabled = false;  // selection used for the cached pixels; settings can change without guest writes
    uint32_t uploads = 0;   // times its bytes changed: past a few it is video or drawn to, never packed
    uint8_t role = 0;       // TexRole: a texture the graphics settings scale the draws of
    photo_capture::TextureIdentity photo_identity;  // original bytes; retained across cached binds
    pbr::ColorProof pbr_color;
    uint64_t pbr_proof_epoch = 0;
    ComPtr<ID3D12Resource> pbr_nrm;
    ComPtr<ID3D12Resource> pbr_orm;
    uint32_t pbr_levels = 1;
    uint64_t pbr_hash = 0;
    uint64_t pbr_vs = 0;
    uint64_t pbr_ps = 0;
    uint64_t pbr_draws = 0;
    bool pbr_done = false;
    bool pbr_has_height = false;
    bool pbr_vehicle = false;
    bool pbr_cutout = false;
    const Tex* swap = nullptr;  // the texture pack's image drawn in its place
    uint64_t pack_key = 0;      // its pack image is still loading
    uint32_t pack_gen = 0;      // texturedumper::Generation the key was taken at, 0 after new bytes
};

// textures recognised by their content hash (texture_pack.h ContentHash, the names in textures/source_index.json)
enum TexRole : uint8_t { kRoleNone, kRoleNeon };
// car neon underglow: vehicle/shared_texture/texture_x/vp_neon.tex, texture_x/fx_car_neon_glow.tex (1 and 4 levels)
constexpr uint64_t kNeonTextures[] = {0x2EF7B12D7E0B7B93ull, 0x45CEC7A39FE1B185ull, 0x275CDB0152EEA3A0ull};
// the neon textures are 64x64 and 32x32 DXT5: only small compressed textures are hashed to look for them
constexpr uint32_t kRoleMaxSize = 64;
uint8_t TextureRole(uint64_t name) {
    for (uint64_t n : kNeonTextures)
        if (n == name) return kRoleNeon;
    return kRoleNone;
}

struct PsoKey {
    uint64_t vs;
    uint64_t ps;
    uint32_t regs;
    uint32_t topo;
    uint32_t has_rt;
    uint32_t has_ds;
    uint32_t elements[16];  // per register: DXGI format, buffer << 8, offset << 16
    uint32_t blend;
    uint32_t src;
    uint32_t dst;
    uint32_t op;
    uint32_t wmask;
    uint32_t zen;
    uint32_t zwrite;
    uint32_t zfunc;
    uint32_t sten;
    uint32_t sfunc;
    uint32_t smask;
    uint32_t swmask;
    uint32_t sfail;
    uint32_t szfail;
    uint32_t spass;
    uint32_t cull;
    uint32_t ccw;
    uint32_t fill;
    int32_t zbias;
    float zslope;
    uint32_t rtfmt;
    uint32_t sprite;  // DrawShaders::sprite
};

// Blit's settings past the source and its rectangle
struct BlitOptions {
    bool gamma = false;       // through the gamma lookup
    bool alpha = false;       // the source's alpha, else 1
    uint32_t taps = 1;        // n x n box filter per pixel
    float mul = 1;            // colour scale
    DXGI_FORMAT rtv = kBgra;  // the target's format
    bool over = false;        // premultiplied over the target, which keeps its alpha
};

// Shrink's settings past the source and the size
struct ShrinkOptions {
    uint32_t taps = 1;
    // into: a copy from an earlier call to draw over again, in state into_state
    ID3D12Resource* into = nullptr;
    D3D12_RESOURCE_STATES into_state = D3D12_RESOURCE_STATE_COPY_SOURCE;
    float mul = 1;
};

// a draw's four texture stages: BindStageTexturesAndTargets fills them, the constants, views and samplers read them
struct DrawTextures {
    TexInfo info[4];
    D3D12_SHADER_RESOURCE_VIEW_DESC sv[4];
    ID3D12Resource* res[4];
    bool rt[4] = {};            // stage samples a render target
    float k[4] = {1, 1, 1, 1};  // Extension::Sample's k
    float wide[4] = {};         // host texels per console texel of a target read into a smaller one, 0 otherwise
    Target* tgt[4] = {};        // the target a stage samples
    uint8_t role[4] = {};       // TexRole of a guest texture
    photo_capture::TextureIdentity photo_identity[4];
    bool photo_replaced[4] = {};
    Tex* accepted_color[4] = {}; // Actual accepted upload; not a render target/movie or a filename-derived map.
    ID3D12Resource* pbr_nrm = nullptr;
    ID3D12Resource* pbr_orm = nullptr;
    uint32_t pbr_levels = 1;
    bool pbr_has_height = false;
    Tex* pbr_tex = nullptr;
};

// what the last draw set on the command list, so the next one sets only what differs; anything else that sets them
// clears it
struct DrawBinds {
    ID3D12PipelineState* pso = nullptr;
    D3D_PRIMITIVE_TOPOLOGY topology = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
    bool rasterizer = false;  // viewport and scissor hold
    D3D12_VIEWPORT viewport{};
    D3D12_RECT scissor{};
    bool output = false;  // blend and stencil hold
    float blend[4] = {};
    uint32_t stencil = 0;
    D3D12_GPU_VIRTUAL_ADDRESS vsc = 0;  // root constant buffers
    D3D12_GPU_VIRTUAL_ADDRESS dc = 0;
    uint32_t views = UINT32_MAX;  // root descriptor table slots
    uint32_t samplers = UINT32_MAX;
};

// the last targets EnsureTarget and FindTarget marked used, so target_used_ is written once a frame each
struct UsedTarget {
    uint32_t data = 0;
    uint64_t frame = UINT64_MAX;
};

// a draw's shaders: what they are made from, their cache keys, the hashes Compile returned and the compiled blobs
struct DrawShaders {
    bool prog = false;  // a vertex program, else the fixed function pipeline
    bool quad = false;
    bool fixed_transform = false;  // fixed function on untransformed vertices: the shader reads matrices and lights
    bool car_paint = false;  // the enhanced car paint variants (gpu/car_paint.h)
    bool sprite = false;           // a program's points drawn as point sprites (SpriteHlsl)
    FixedKey fk;
    uint64_t vkey = 0;
    uint64_t vh = 0;
    uint64_t pkey = 0;
    uint64_t ph = 0;
    ID3DBlob* vs_code = nullptr;
    ID3DBlob* ps_code = nullptr;
};

// the render target's console size in pixels, or the depth buffer's without one
struct GuestExtent {
    uint32_t w;
    uint32_t h;
};

class D3D12Backend final : public Backend, public Context {
public:
    ~D3D12Backend() override { pbr::observation::RequestGuestProof(false); }
    bool Init(uint32_t width, uint32_t height) override;
    void Clear(const State& s, uint32_t count, const uint8_t* rects, uint32_t flags, uint32_t color, float z,
               uint32_t stencil) override;
    void Draw(const State& s, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices) override;
    void Copy(const Surface& src, const Surface& dst, uint32_t count, uint32_t src_rects, uint32_t dst_points) override;
    void Present(const Surface& front) override;
    void WaitGpuFrame(uint64_t n) override;
    void Sync(uint32_t data, uint32_t size) override;
    void SyncLate(uint32_t data) override;
    bool DeliverLate(uint32_t data) override;
    void RecordLate(uint32_t data) override;
    void Flush() override;
    void SetGamma(const uint8_t* ramp) override;
    void ClearReport() override { counted_.clear(); }
    // done once the list being recorded, the next fence value, has run
    void GetReport(uint32_t report) override { reports_.push_back({report, counted_, fence_value_ + 1}); }

    ID3D12Device* Device() override { return dev_.Get(); }
    ID3D12GraphicsCommandList* List() override { return cl_.Get(); }
    void Release(ComPtr<ID3D12Resource> r) override { CurrentFrame().garbage.push_back(std::move(r)); }
    D3D12_CPU_DESCRIPTOR_HANDLE View(const Target& t) override { return Cpu(rtv_heap_.Get(), t.view, rtv_inc_); }
    void Unbind() override { bound_rt_ = bound_ds_ = nullptr; }
    void Capture(Target& t, const char* tag, float mul) override;
    float Scale() const override { return scale_; }
    void GuestSize(uint32_t& w, uint32_t& h) const override {
        w = guest_w_;
        h = guest_h_;
    }
    uint64_t Presents() const override { return presents_; }

private:
    struct Frame {
        ComPtr<ID3D12CommandAllocator> alloc;
        ComPtr<ID3D12Resource> upload;
        uint8_t* map = nullptr;
        uint64_t used = 0;
        uint64_t fence = 0;
        std::vector<ComPtr<ID3D12Resource>> garbage;
        std::vector<ComPtr<IUnknown>> pbr_retained; // Same actual frame fence as upload/allocator ownership.
        std::shared_ptr<const pbr::Library> pbr_owner;
        std::vector<uint64_t> pbr_pending_draws; // Published only after this frame's ACTUAL fence completes.
        uint32_t srv = 0;
    };
    struct Alloc {
        ID3D12Resource* res;
        uint64_t off;
        uint8_t* cpu;
        D3D12_GPU_VIRTUAL_ADDRESS gpu;
    };

    Alloc Upload(uint64_t size, uint64_t align = 256);
    Frame& CurrentFrame() { return frames_[frame_ % kFrames]; }
    uint32_t AllocSrvSlots(uint32_t n);
    D3D12_CPU_DESCRIPTOR_HANDLE Cpu(ID3D12DescriptorHeap* h, uint32_t i, uint32_t inc) {
        auto c = h->GetCPUDescriptorHandleForHeapStart();
        c.ptr += size_t(i) * inc;
        return c;
    }
    D3D12_GPU_DESCRIPTOR_HANDLE Gpu(ID3D12DescriptorHeap* h, uint32_t i, uint32_t inc) {
        auto g = h->GetGPUDescriptorHandleForHeapStart();
        g.ptr += uint64_t(i) * inc;
        return g;
    }
    void Transition(ID3D12Resource* r, D3D12_RESOURCE_STATES& cur, D3D12_RESOURCE_STATES want) override;
    void Begin();
    void Submit(bool wait);
    void WaitFence(uint64_t value);
    void DrainDebugMessages(const char* operation);
    void ReportDebugDred(const char* operation);
    void SetFullViewport(uint32_t w, uint32_t h);
    void SetBlitTarget(D3D12_CPU_DESCRIPTOR_HANDLE rtv, uint32_t w, uint32_t h);
    void Blit(ID3D12Resource* src, DXGI_FORMAT fmt, const float uvrect[4], bool gamma, bool alpha, uint32_t taps = 1,
              float mul = 1, DXGI_FORMAT rtv = kBgra) override {
        Blit(src, fmt, uvrect, BlitOptions{.gamma = gamma, .alpha = alpha, .taps = taps, .mul = mul, .rtv = rtv});
    }
    void Blit(ID3D12Resource* src, DXGI_FORMAT fmt, const float uvrect[4], const BlitOptions& o);
    uint32_t EnsureBlitSamplers();
    void DrawFullScreen(ID3D12PipelineState* pso, D3D12_GPU_VIRTUAL_ADDRESS cb, uint32_t srv_slot,
                        std::optional<uint32_t> sampler_slot = {});
    ID3D12Resource* Feedback(Target& t) override;
    void Resolve(Target& t) {
        if (t.range != 1 && ext_) ext_->Resolve(*this, t);
    }
    // Pass, then its step
    void Pass(const char* hlsl, const char* entry, DXGI_FORMAT rtv, D3D12_CPU_DESCRIPTOR_HANDLE rh, uint32_t w,
              uint32_t h, ID3D12Resource* const src[3], const DXGI_FORMAT fmt[3], const float p[8],
              float blend) override;
    ID3D12PipelineState* EnsurePassPipeline(const char* hlsl, const char* entry, DXGI_FORMAT rtv, float blend);
    // Draw, in call order, each callee right after its caller
    bool DepthCopy(const State& s);
    ID3D12PipelineState* EnsureDepthCopyPipeline();
    void DrawDepthCopy(Target& src, Target& dst, ID3D12PipelineState* pso);
    bool AssembleDraw(const State& s, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices,
                      Assembled& a);
    bool BindTargets(const State& s);
    void EnsureVertexShader(const State& s, const Assembled& a, DrawShaders& sh);
    bool BindStageTexturesAndTargets(const State& s, DrawTextures& stages);
    ID3D12Resource* BindTexture(const State& s, int stage, DrawTextures& stages);
    ID3D12Resource* BindTargetTexture(const State& s, int stage, const TextureDesc& d, Target& t, bool box,
                                      DrawTextures& stages);
    ID3D12Resource* BindGuestTexture(const State& s, int stage, const TextureDesc& d, const FormatInfo& fi,
                                     DrawTextures& stages);
    bool CreateTexture(Tex& tx, const TextureDesc& d, const FormatInfo& fi, uint64_t key);
    void UploadTexture(Tex& tx, const TextureDesc& d, const FormatInfo& fi, const uint8_t* p, const uint32_t* colours,
                       bool same, uint64_t dump = 0);
    // a texture pack image in place of the console's texture: BGRA, its own size and mip chain
    bool UploadReplacement(Tex& tx, const pack::Image& img);
    ComPtr<ID3D12Resource> UploadPackImage(const pack::Image& img);
    void PublishPbrTelemetry();
    void UpdatePbrSettings();
    void CompletePbrFrame(Frame&);
    bool TryPbrDraw(const State&,const Assembled&,const DrawShaders&,const PixelKey&,const PsoKey&,
                    const DrawTextures&,GuestExtent,ID3D12PipelineState* legacy_pso);
    // an HD movie's picture in place of the texture the title decodes its Bink into (video_hd.h)
    ID3D12Resource* BindMovie(const TextureDesc& d, int stage, DrawTextures& stages);
    void UploadTextureLevel(Tex& tx, uint32_t f, uint32_t l, const std::vector<uint8_t>& px, uint32_t pitch);
    void LatchPackKey(Tex& tx, const TextureDesc& d, const FormatInfo& fi, const uint32_t* colours);
    void LatchPackImage(Tex& tx);
    void CreatePackTexture(Tex& r, const texturedumper::Image& img);
    void ReleasePackTextures();
    void EnsurePixelShader(const PixelKey& pk, DrawShaders& sh);
    void BuildPipelineKey(const State& s, const Assembled& a, uint64_t vh, uint64_t ph, PsoKey& k);
    void KeyDepthStencil(const State& s, const Assembled& a, PsoKey& k);
    ID3D12PipelineState* EnsurePipeline(const PsoKey& k, ID3DBlob* vs, ID3DBlob* ps);
    void FillPipelineDesc(const PsoKey& k, ID3DBlob* vs, ID3DBlob* ps, D3D12_GRAPHICS_PIPELINE_STATE_DESC& d) const;
    void ExpandPoints(D3D12_GRAPHICS_PIPELINE_STATE_DESC& d, bool sprite);
    float TargetScale() const;  // host pixels per console pixel of the bound target
    float RenderTargetShift(const State& s, const DrawShaders& sh) const;
    void FillDepthConstants(const State& s, const DrawShaders& sh, const PsoKey& k, DrawConstants& c) const;
    uint8_t* UploadVertexConstants(const State& s);
    void UploadConstants(const State& s, const DrawShaders& sh, const PsoKey& k, const DrawTextures& stages,
                         GuestExtent size, DrawConstants& c, draw_evidence::Recorder* evidence = nullptr);
    void FillTextureConstants(const State& s, const DrawShaders& sh, const DrawTextures& stages, DrawConstants& c);
    void FillTexelConstants(int i, const TextureDesc& d, const DrawTextures& stages, DrawConstants& c);
    void CaptureDraw(const State& s, uint32_t prim, uint32_t count, const PsoKey& k, const DrawConstants& c,
                     const DrawTextures& stages);
    void CaptureTextures(const State& s, const DrawTextures& stages);
    void EndDrawCapture(const State& s, const Assembled& a, uint32_t count, const DrawConstants& c, bool fixed);
    void CaptureInputs(const State& s, const Assembled& a, const DrawConstants& c, bool fixed);
    void CaptureRawVertices(const State& s, const Assembled& a, const DrawConstants& c, bool fixed);
    void CaptureShaders(const State& s, const Assembled& a, const DrawShaders& sh, const PixelKey& pk);
    void CaptureShader(const char* kind, uint64_t key, const std::string& src);
    void SetTextureViewsAndSamplers(const State& s, const Assembled& a, DrawTextures& stages);
    uint32_t EnsureViewSet(const DrawTextures& stages);
    uint32_t EnsureStageSamplers(const State& s, const DrawTextures& stages, bool clamp);
    bool ScaleScreenEffect(const PsoKey& k, const DrawTextures& stages, uint32_t count, DrawConstants& c);
    bool ScaleTextureEffect(const PsoKey& k, const DrawTextures& stages, DrawConstants& c);
    void SetViewportScissor(const State& s, uint32_t gw);
    void SetBlendFactorStencilRef(const State& s);
    void UpdateTargetUsed(uint32_t data);
    void DrawGeometry(const Assembled& a, draw_evidence::Recorder* evidence = nullptr);
    void UploadVertexBuffers(const Assembled& a, draw_evidence::Recorder* evidence = nullptr);
    // visibility tests
    bool EnsureQueries();
    uint32_t BeginCountedDraw(const State& s);
    void EndCountedDraw(uint32_t query);
    float SamplesPerPixel() const;
    void DeliverReports();
    // shaders by source hash: Compile returns the bytecode and sets hash
    ID3DBlob* Compile(const std::string& src, const char* entry, const char* profile, uint64_t& hash);
    // Blit's pipeline
    ID3D12PipelineState* EnsureBlitPipeline(DXGI_FORMAT rtv, bool over = false);
    D3D12_GRAPHICS_PIPELINE_STATE_DESC FullScreenPipelineDesc(ID3DBlob* vs, ID3DBlob* ps, DXGI_FORMAT rtv) const;
    // Init, in order, each callee right after its caller
    bool CreateDevice();
    bool CreateSwapChain(HWND hwnd);
    void CreateHeaps();
    uint32_t CreateHeap(D3D12_DESCRIPTOR_HEAP_TYPE t, uint32_t n, bool visible, ComPtr<ID3D12DescriptorHeap>& out);
    void CreateBackBufferViews();
    bool CreateRootSignature();
    void CreateFrames();
    void CreateGammaLut();
    void LoadSettings();
    // Present, in call order, each callee right after its caller; DrawFrame runs the two after it
    void FinishCapture(const Surface& front);
    void StartCapture();
    void InitPhotoCapture();
    void ObservePhotoDraw(const State& s, const Assembled& a, const PsoKey& k,
                          const DrawShaders& sh, const DrawTextures& stages);
    void CapturePhoto(const photo_capture::Decision& decision, uint32_t backbuffer);
    void ResizeSwap();
    void UploadGammaRamp();
    void DrawFrame(Target* t, uint32_t bi);
    void BlitFrame(Target& t, D3D12_CPU_DESCRIPTOR_HANDLE bb);
    bool EnsurePostTargets(uint32_t w, uint32_t h);
    // a kPostHlsl pass into post_[dst]: sources t0 t1, n floats of constants
    void PostPass(const char* entry, int dst, ID3D12Resource* s0, DXGI_FORMAT f0, ID3D12Resource* s1, DXGI_FORMAT f1,
                  const float* p, uint32_t n);
    // the frame through the picture chain into the bound back buffer viewport; false leaves it to the plain blit
    bool PostProcess(Target& t, D3D12_CPU_DESCRIPTOR_HANDLE bb, const D3D12_VIEWPORT& vp);
    void RenderUi();
    void ReleaseOldTextures();
    void ResetFrame();
    void CountFps();
    void ApplySettings();
    ComPtr<ID3D12Resource> Shrink(ID3D12Resource* src, D3D12_RESOURCE_STATES& st, DXGI_FORMAT fmt, uint32_t w,
                                  uint32_t h, const ShrinkOptions& o = {});
    bool ReadBack(ID3D12Resource* res, uint32_t w, uint32_t h, std::vector<uint8_t>& out);
    void CapStep(const Surface& rt);
    // a resource CapSave shrinks to w x h and writes, in state *state, colour times mul
    struct CapImage {
        ID3D12Resource* res;
        D3D12_RESOURCE_STATES* state;
        DXGI_FORMAT fmt;
        uint32_t w;
        uint32_t h;
        float mul = 1;
    };
    void CapSave(const CapImage& img, const char* name);
    void CapSave(Target& t, const char* name);
    // a surface's place in targets_: its key, console size and entry, end when it has none
    struct TargetSlot {
        uint32_t data;
        uint64_t key;
        uint32_t gw;
        uint32_t gh;
        std::unordered_map<uint64_t, Target>::iterator it;
        bool sized;  // the entry is there at the surface's size
    };
    Target* FindTarget(const Surface& s, bool depth);
    Target* EnsureTarget(const Surface& s, bool depth);
    // EnsureTarget's steps, in call order
    TargetSlot FindTargetSlot(const Surface& s, bool depth, uint64_t alt);
    uint64_t SizedTargetKey(uint32_t data, bool depth, uint32_t gw, uint32_t gh) const;
    bool TargetStale(uint32_t data) const;
    void ReleaseTarget(std::unordered_map<uint64_t, Target>::iterator it);
    bool CreateTargetResource(Target& t, const Surface& s);
    bool CreateTargetView(Target& t);
    // UploadTarget, then its step
    void UploadTarget(Target& t, const Surface& s);
    ComPtr<ID3D12Resource> CreatePixelTexture(const std::vector<uint8_t>& px, uint32_t pitch, uint32_t w, uint32_t h);
    // Clear's arguments past the state
    struct ClearArgs {
        uint32_t count;
        const uint8_t* rects;
        uint32_t flags;
        uint32_t color;
        float z;
        uint32_t stencil;
    };
    void ClearTarget(Target& t, const State& s, const ClearArgs& c, std::vector<D3D12_RECT>& rs);
    void ClearChannels(Target& t, const float col[4], uint8_t mask, const std::vector<D3D12_RECT>& rs);
    ID3D12PipelineState* EnsureClearPipeline(DXGI_FORMAT rtv, uint8_t mask);
    // Copy's guest lists: count src_rects, dst_points 0 or one per rect
    struct CopyList {
        uint32_t count;
        uint32_t src_rects;
        uint32_t dst_points;
    };
    void CopyByDraw(const Target& src_t, ID3D12Resource* src_res, D3D12_RESOURCE_STATES& src_state, Target& dst_t,
                    const CopyList& l);
    void CopyByRegion(const Target& src_t, ID3D12Resource* src_res, D3D12_RESOURCE_STATES& src_state, Target& dst_t,
                      const CopyList& l);
    // the target as a guest sized BGRA copy source, and the shrunk copy that holds it when it is not one already
    struct CopySource {
        ID3D12Resource* res;
        ComPtr<ID3D12Resource> down;
    };
    CopySource PrepareCopySource(Target& t, const ShrinkOptions& o);
    // RecordLate's steps, under late_lock_
    struct Late;
    void ResetLate(Late& l, const Target& t);
    ID3D12Resource* PrepareLateSource(Target& t, Late& l);
    void RecordLateCopy(Late& l, uint32_t slot, ID3D12Resource* src);

    ComPtr<IDXGIFactory4> factory_;
    ComPtr<ID3D12Device> dev_;
    ComPtr<ID3D12InfoQueue> debug_info_;
    bool debug_gpu_ = false, debug_dred_reported_ = false;
    diagnostics::Budget debug_budget_;
    uint64_t debug_discarded_ = 0;
    ComPtr<ID3D12CommandQueue> queue_;
    ComPtr<IDXGISwapChain3> swap_;
    ComPtr<ID3D12Resource> bb_[kFrames];
    ComPtr<ID3D12Fence> fence_;
    HANDLE event_ = nullptr;
    uint64_t fence_value_ = 0;
    Frame frames_[kFrames];
    uint64_t frame_ = 0;
    // texture view sets made since the descriptor ring last started over, by a hash of their resources and views, to
    // their first slot; resources freed in that time go to the frame's garbage, so no other one takes their address
    std::unordered_map<uint64_t, uint32_t> view_sets_;
    uint64_t view_sets_frame_ = UINT64_MAX;
    // the constants last uploaded in this command list, reused while a draw's are the same; 0 after a submit
    decltype(State::vsc) last_vsc_{};
    D3D12_GPU_VIRTUAL_ADDRESS last_vsc_gpu_ = 0;
    DrawConstants last_dc_{};
    D3D12_GPU_VIRTUAL_ADDRESS last_dc_gpu_ = 0;
    ComPtr<ID3D12GraphicsCommandList> cl_;
    ComPtr<ID3D12DescriptorHeap> srv_heap_;
    ComPtr<ID3D12DescriptorHeap> smp_heap_;
    ComPtr<ID3D12DescriptorHeap> rtv_heap_;
    ComPtr<ID3D12DescriptorHeap> dsv_heap_;
    ComPtr<ID3D12DescriptorHeap> ui_heap_;
    uint32_t srv_inc_ = 0;
    uint32_t smp_inc_ = 0;
    uint32_t rtv_inc_ = 0;
    uint32_t dsv_inc_ = 0;
    std::vector<uint32_t> free_rtv_;
    std::vector<uint32_t> free_dsv_;
    ComPtr<ID3D12RootSignature> root_;
    std::unordered_map<uint64_t, ComPtr<ID3DBlob>> blobs_;
    std::unordered_map<uint64_t, uint64_t> shader_src_;  // vertex and pixel program keys to source hash
    std::unordered_map<uint64_t, ComPtr<ID3D12PipelineState>> psos_;
    std::unordered_map<uint64_t, uint32_t> samplers_;
    uint32_t smp_used_ = 0;
    std::unordered_map<uint64_t, Target> targets_;
    std::unordered_map<uint32_t, uint64_t> target_used_;  // target address to the frame it was last used
    std::array<UsedTarget, 4> recent_used_{};
    uint32_t recent_used_next_ = 0;
    DrawBinds binds_;
    std::unordered_map<uint64_t, Tex> textures_;  // by data address, a P8 texture's with its palette's above
    std::unordered_map<uint32_t, Tex> movies_;    // HD movie pictures by the data address of the texture they replace
    pbr::LibraryCache pbr_library_cache_;
    pbr::LibrarySnapshot pbr_library_;
    std::unique_ptr<pbr::controls::PreparedSettings> pbr_settings_;
    pbr::device::Device pbr_device_;
    uint64_t pbr_proof_epoch_ = 0;
    std::vector<pbr::controls::CatalogMaterial> pbr_observed_;
    std::vector<std::string> pbr_trace_;
    // Built PBR meshes by MixHash of everything fixed_mesh::Build reads (vertex/index bytes included). A hit reuses
    // the vertices/indices and refreshes only the transform-dependent constants; deterministic failures are kept too.
    struct PbrMeshEntry {
        pbr::fixed_mesh::Mesh mesh;
        pbr::fixed_mesh::Status status = pbr::fixed_mesh::Status::Ready;
        std::string detail;
    };
    std::unordered_map<uint64_t, PbrMeshEntry> pbr_mesh_cache_;
    static constexpr size_t kPbrMeshCacheEntries = 1024;  // reasons already logged per material row, so the log says each once
    pbr::controls::Capabilities pbr_capabilities_;
    std::unordered_map<uint64_t, Tex> pack_textures_;  // texture pack images by key, kept for the run
    std::unordered_map<uint64_t, ComPtr<ID3D12PipelineState>> blit_psos_;
    Extension* ext_ = nullptr;
    // one frame capture: every draw in a text file, each target as the frame leaves it, the textures it read
    FILE* cap_ = nullptr;
    photo_capture::Config photo_config_;
    photo_capture::Policy photo_policy_;
    std::array<photo_capture::Observation, photo_capture::kMaxTextures> photo_observed_{};
    std::unordered_map<uint64_t, uint32_t> photo_sampled_;
    std::filesystem::path cap_dir_;
    capture::AutoSchedule auto_capture_;  // F1 > Capture: automatic captures while playing
    bool cap_auto_ = false;               // the open capture was an automatic one
    uint32_t cap_i_ = 0;
    draw_evidence::Selector draw_evidence_selector_;
    uint32_t cap_rt_ = 0;
    std::unordered_set<uint32_t> cap_seen_;
    std::unordered_set<uint64_t> cap_sh_;
    std::array<std::array<float, 4>, 192> cap_vsc_{};  // vertex constants at the previous captured draw
    struct CapTex {
        ComPtr<ID3D12Resource> res;
        DXGI_FORMAT fmt;
        uint32_t data;
        uint32_t w;
        uint32_t h;
    };
    std::vector<CapTex> cap_tex_;
    ComPtr<ID3D12Resource> lut_;
    // the picture chain's two targets, output sized, BGRA
    ComPtr<ID3D12Resource> post_[2];
    D3D12_RESOURCE_STATES post_state_[2] = {D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_RENDER_TARGET};
    uint32_t post_rtv_[2] = {~0u, ~0u};
    uint32_t post_w_ = 0, post_h_ = 0;
    D3D12_RESOURCE_STATES lut_state_ = D3D12_RESOURCE_STATE_COPY_DEST;
    uint8_t ramp_[768] = {};
    bool ramp_dirty_ = false;
    bool gamma_ = false;
    Target* bound_rt_ = nullptr;
    Target* bound_ds_ = nullptr;
    Target* draw_rt_ = nullptr;  // the draw's targets, the bound ones go when a texture's copy is drawn
    Target* draw_ds_ = nullptr;
    uint32_t guest_w_ = 640;
    uint32_t guest_h_ = 480;
    uint32_t bb_w_ = 0;
    uint32_t bb_h_ = 0;
    float scale_ = 1.0f;
    app::Mode scene_{640, 480};
    bool allocation_failed_ = false;  // one allocation failure per selection; UI continues on the swap chain
    bool post_failed_ = false;
    // surfaces locked without a wait: copies in flight, the newest finished one goes to guest memory
    struct Late {
        ComPtr<ID3D12Resource> rb[kFrames];
        ComPtr<ID3D12Resource> down;
        D3D12_RESOURCE_STATES down_state = D3D12_RESOURCE_STATE_COPY_SOURCE;
        uint64_t fence[kFrames] = {};
        uint64_t frame[kFrames] = {};
        uint64_t delivered = 0;
        uint32_t gw = 0;
        uint32_t gh = 0;
        uint32_t row_pitch = 0;
        uint32_t next = 0;
    };
    std::unordered_map<uint32_t, Late> late_;
    std::mutex late_lock_;                      // DeliverLate runs on the title's thread
    std::atomic<uint64_t> presents_seen_{0};    // presents_ for DeliverLate
    std::atomic<uint64_t> present_fence_[kFrames + 1] = {};  // by presents_, for WaitGpuFrame
    Assembled asm_;  // kept between draws for its buffers
    // visibility tests: an occlusion query per counted draw, summed into report memory once the GPU is done
    struct Query {
        uint32_t index;
        float samples_per_pixel;  // host samples one console pixel covers
    };
    struct Report {
        uint32_t va;
        std::vector<Query> queries;
        uint64_t fence;
    };
    ComPtr<ID3D12QueryHeap> query_heap_;
    ComPtr<ID3D12Resource> query_rb_;
    const uint64_t* query_counts_ = nullptr;
    uint32_t query_next_ = 0;
    std::vector<Query> counted_;  // since the last ClearReport
    std::deque<Report> reports_;
    bool tearing_ = false;
    uint64_t presents_ = 0;
    double fps_ = 0;
    std::chrono::steady_clock::time_point fps_t_ = std::chrono::steady_clock::now();
    uint64_t fps_n_ = 0;
};

bool D3D12Backend::Init(uint32_t width, uint32_t height) {
    InitPhotoCapture();
    g_reversed_depth = true;
    guest_w_ = width;
    guest_h_ = height;
    HWND hwnd = static_cast<HWND>(app::Window());
    if (!CreateDevice()) return false;
    if (!CreateSwapChain(hwnd)) return false;
    CreateHeaps();
    CreateBackBufferViews();
    if (!CreateRootSignature()) return false;
    CreateFrames();
    CreateGammaLut();
    ImGui_ImplDX12_Init(dev_.Get(), kFrames, DXGI_FORMAT_B8G8R8A8_UNORM, ui_heap_.Get(),
                        ui_heap_->GetCPUDescriptorHandleForHeapStart(), ui_heap_->GetGPUDescriptorHandleForHeapStart());
    LoadSettings();
    Begin();
    LOGI("d3d12: %ux%u guest, %ux%u window, render scale %.3f", width, height, bb_w_, bb_h_, scale_);
    return true;
}

bool D3D12Backend::CreateDevice() {
    const auto evidence_enabled = DrawEvidenceEnv("XBE_PBR_DRAW_EVIDENCE");
    if (!evidence_enabled.empty() && evidence_enabled != "0") {
        draw_evidence_selector_ = draw_evidence::ParseSelector(evidence_enabled,
            DrawEvidenceEnv("XBE_PBR_EVIDENCE_VERTEX_KEY"), DrawEvidenceEnv("XBE_PBR_EVIDENCE_PIXEL_KEY"),
            DrawEvidenceEnv("XBE_PBR_EVIDENCE_DRAW"));
        if (draw_evidence_selector_.invalid) LOGI("draw evidence: invalid opt-in selector, capture disabled");
    }
    debug_gpu_ = GetEnvironmentVariableW(L"XBE_D3D12_DEBUG", nullptr, 0) != 0;
    if (debug_gpu_) {
        ComPtr<ID3D12Debug> dbg;
        const HRESULT debug_result = D3D12GetDebugInterface(IID_PPV_ARGS(&dbg));
        if (SUCCEEDED(debug_result)) dbg->EnableDebugLayer();
        LOGR("d3d12 debug: debug layer %s (0x%08lX)", SUCCEEDED(debug_result) ? "enabled" : "unavailable",
             static_cast<unsigned long>(debug_result));
        // DRED enablement must precede device creation. No GPU validation, queue wait or render policy is changed.
        ComPtr<ID3D12DeviceRemovedExtendedDataSettings> dred;
        const HRESULT dred_result = D3D12GetDebugInterface(IID_PPV_ARGS(&dred));
        if (SUCCEEDED(dred_result)) {
            dred->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
            dred->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
            ComPtr<ID3D12DeviceRemovedExtendedDataSettings1> context;
            if (SUCCEEDED(dred.As(&context))) context->SetBreadcrumbContextEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        }
        LOGR("d3d12 debug: DRED breadcrumbs/page faults %s (0x%08lX)", SUCCEEDED(dred_result) ? "requested" : "unavailable",
             static_cast<unsigned long>(dred_result));
    }
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory_)))) return false;
    // dedicated GPU first on hybrid laptops
    ComPtr<IDXGIFactory6> f6;
    ComPtr<IDXGIAdapter1> adapter;
    if (SUCCEEDED(factory_.As(&f6))) {
        f6->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter));
    }
    if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev_))) &&
        FAILED(D3D12CreateDevice((adapter = nullptr).Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev_)))) {
        LOGE("d3d12: no device");
        return false;
    }
    DXGI_ADAPTER_DESC1 ad{};
    if (adapter) adapter->GetDesc1(&ad);
    LOGR("D3D12 adapter: %ls", adapter ? ad.Description : L"default");
    if (debug_gpu_) {
        const HRESULT info_result = dev_.As(&debug_info_);
        LOGR("d3d12 debug: InfoQueue %s (0x%08lX)", SUCCEEDED(info_result) ? "available" : "unavailable",
             static_cast<unsigned long>(info_result));
        if (debug_info_) {
            const HRESULT limit = debug_info_->SetMessageCountLimit(diagnostics::Budget::kStoredMessages);
            D3D12_MESSAGE_SEVERITY denied[] = {D3D12_MESSAGE_SEVERITY_INFO, D3D12_MESSAGE_SEVERITY_MESSAGE};
            D3D12_INFO_QUEUE_FILTER filter{};
            filter.DenyList.NumSeverities = UINT(std::size(denied));
            filter.DenyList.pSeverityList = denied;
            const HRESULT filtered = debug_info_->AddStorageFilterEntries(&filter);
            LOGR("d3d12 debug: bounded warning/error queue limit 0x%08lX filter 0x%08lX",
                 static_cast<unsigned long>(limit), static_cast<unsigned long>(filtered));
        }
    }
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    dev_->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue_));
    if (debug_gpu_ && queue_) queue_->SetName(L"MC3 direct queue");
    ComPtr<IDXGIFactory5> f5;
    BOOL tear = FALSE;
    if (SUCCEEDED(factory_.As(&f5))) f5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tear, sizeof(tear));
    tearing_ = tear;
    return true;
}

bool D3D12Backend::CreateSwapChain(HWND hwnd) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    bb_w_ = std::max<LONG>(rc.right - rc.left, 1);
    bb_h_ = std::max<LONG>(rc.bottom - rc.top, 1);
    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = bb_w_;
    sd.Height = bb_h_;
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = kFrames;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.Flags = tearing_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
    ComPtr<IDXGISwapChain1> sc1;
    if (FAILED(factory_->CreateSwapChainForHwnd(queue_.Get(), hwnd, &sd, nullptr, nullptr, &sc1))) {
        LOGE("d3d12: no swap chain");
        return false;
    }
    factory_->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
    sc1.As(&swap_);
    return true;
}

void D3D12Backend::DrainDebugMessages(const char* operation) {
    if (!debug_gpu_ || !debug_info_) return;
    const uint64_t available = debug_info_->GetNumStoredMessagesAllowedByRetrievalFilter();
    const uint64_t scan = diagnostics::Budget::ScanCount(available);
    for (uint64_t index = 0; index < scan; ++index) {
        SIZE_T bytes = 0;
        if (FAILED(debug_info_->GetMessage(index, nullptr, &bytes)) ||
            !diagnostics::Budget::MessageSize(bytes, sizeof(D3D12_MESSAGE))) continue;
        std::vector<uint8_t> storage(bytes);
        auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
        if (FAILED(debug_info_->GetMessage(index, message, &bytes)) ||
            !debug_budget_.Message(uint32_t(message->Severity))) continue;
        // The returned description belongs to this bounded buffer. Do not trust an oversized length.
        const uintptr_t begin = reinterpret_cast<uintptr_t>(storage.data());
        const uintptr_t end = begin + storage.size();
        const uintptr_t description = reinterpret_cast<uintptr_t>(message->pDescription);
        if (description < begin || description >= end) continue;
        const size_t length = diagnostics::Budget::DescriptionSize(message->DescriptionByteLength, end - description);
        std::string text(message->pDescription, length);
        for (char& c : text) if (c == '\r' || c == '\n' || c == '\0') c = ' ';
        LOGR("d3d12 debug: %s severity %u id %u: %s", operation, unsigned(message->Severity),
             unsigned(message->ID), text.c_str());
    }
    const uint64_t discarded = debug_info_->GetNumMessagesDiscardedByMessageCountLimit();
    if ((discarded != debug_discarded_ || available > scan) && debug_budget_.Message(2))
        LOGR("d3d12 debug: bounded queue omitted %llu stored messages, driver discarded %llu total",
             static_cast<unsigned long long>(available - scan), static_cast<unsigned long long>(discarded));
    debug_discarded_ = discarded;
    debug_info_->ClearStoredMessages();
    if (available || discarded) logx::Flush();
}

void D3D12Backend::ReportDebugDred(const char* operation) {
    if (!debug_gpu_ || debug_dred_reported_) return;
    debug_dred_reported_ = true;
    ComPtr<ID3D12DeviceRemovedExtendedData1> dred;
    const HRESULT query = dev_.As(&dred);
    if (FAILED(query)) {
        LOGR("d3d12 debug: %s DRED interface unavailable (0x%08lX)", operation, static_cast<unsigned long>(query));
        return;
    }
    D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1 breadcrumbs{};
    D3D12_DRED_PAGE_FAULT_OUTPUT1 fault{};
    const HRESULT breadcrumb_result = dred->GetAutoBreadcrumbsOutput1(&breadcrumbs);
    const HRESULT fault_result = dred->GetPageFaultAllocationOutput1(&fault);
    LOGR("d3d12 debug: %s DRED breadcrumbs 0x%08lX page fault 0x%08lX VA 0x%llX", operation,
         static_cast<unsigned long>(breadcrumb_result), static_cast<unsigned long>(fault_result),
         static_cast<unsigned long long>(fault.PageFaultVA));
    const auto* node = SUCCEEDED(breadcrumb_result) ? breadcrumbs.pHeadAutoBreadcrumbNode : nullptr;
    for (unsigned i = 0; node && i < 8; ++i, node = node->pNext) {
        const uint32_t done = node->pLastBreadcrumbValue ? *node->pLastBreadcrumbValue : 0;
        uint32_t last = ~0u, next = ~0u;
        if (node->pCommandHistory && done <= node->BreadcrumbCount) {
            if (done) last = uint32_t(node->pCommandHistory[done - 1]);
            if (done < node->BreadcrumbCount) next = uint32_t(node->pCommandHistory[done]);
        }
        LOGR("d3d12 debug: breadcrumb node %u completed %u/%u last op %u next op %u", i, done,
             node->BreadcrumbCount, last, next);
    }
    logx::Flush();
}

uint32_t D3D12Backend::CreateHeap(D3D12_DESCRIPTOR_HEAP_TYPE t, uint32_t n, bool visible,
                                  ComPtr<ID3D12DescriptorHeap>& out) {
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = t;
    hd.NumDescriptors = n;
    hd.Flags = visible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE : D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    dev_->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&out));
    return dev_->GetDescriptorHandleIncrementSize(t);
}

void D3D12Backend::CreateBackBufferViews() {
    for (int i = 0; i < kFrames; i++) {
        swap_->GetBuffer(i, IID_PPV_ARGS(&bb_[i]));
        dev_->CreateRenderTargetView(bb_[i].Get(), nullptr, Cpu(rtv_heap_.Get(), kRtvCount + i, rtv_inc_));
    }
}

void D3D12Backend::CreateHeaps() {
    srv_inc_ = CreateHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kSrvCount, true, srv_heap_);
    smp_inc_ = CreateHeap(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, kSamplerCount, true, smp_heap_);
    rtv_inc_ = CreateHeap(D3D12_DESCRIPTOR_HEAP_TYPE_RTV, kRtvCount + kFrames, false, rtv_heap_);
    dsv_inc_ = CreateHeap(D3D12_DESCRIPTOR_HEAP_TYPE_DSV, kDsvCount, false, dsv_heap_);
    CreateHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, true, ui_heap_);
    for (uint32_t i = kRtvCount; i-- > 0;) free_rtv_.push_back(i);
    for (uint32_t i = kDsvCount; i-- > 0;) free_dsv_.push_back(i);
}

bool D3D12Backend::CreateRootSignature() {
    D3D12_DESCRIPTOR_RANGE ranges[3] = {};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 4;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
    ranges[1].NumDescriptors = 4;
    ranges[2].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[2].BaseShaderRegister = 4;
    ranges[2].NumDescriptors = 2;
    D3D12_ROOT_PARAMETER params[5] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].Descriptor.ShaderRegister = 0;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[1].Descriptor.ShaderRegister = 1;
    for (int i = 0; i < 3; i++) {
        params[2 + i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[2 + i].DescriptorTable.NumDescriptorRanges = 1;
        params[2 + i].DescriptorTable.pDescriptorRanges = &ranges[i];
        params[2 + i].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    }
    D3D12_ROOT_SIGNATURE_DESC rsd{};
    rsd.NumParameters = 5;
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 4;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
    ranges[1].NumDescriptors = 4;
#if 0
    // root signature parameters 0..4 configured above
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].Descriptor.ShaderRegister = 0;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[1].Descriptor.ShaderRegister = 1;
    for (int i = 0; i < 2; i++) {
        params[2 + i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[2 + i].DescriptorTable.NumDescriptorRanges = 1;
        params[2 + i].DescriptorTable.pDescriptorRanges = &ranges[i];
        params[2 + i].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    }
    D3D12_ROOT_SIGNATURE_DESC rsd{};
    rsd.NumParameters = 4;
#endif
    rsd.pParameters = params;
    rsd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ComPtr<ID3DBlob> rs;
    ComPtr<ID3DBlob> err;
    if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &rs, &err))) return false;
    dev_->CreateRootSignature(0, rs->GetBufferPointer(), rs->GetBufferSize(), IID_PPV_ARGS(&root_));
    return true;
}

void D3D12Backend::CreateFrames() {
    for (auto& f : frames_) {
        dev_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&f.alloc));
        D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_UPLOAD};
        D3D12_RESOURCE_DESC bd = BufferDesc(kUploadSize);
        dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                      IID_PPV_ARGS(&f.upload));
        f.upload->Map(0, nullptr, reinterpret_cast<void**>(&f.map));
    }
    dev_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_));
    event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    dev_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, frames_[0].alloc.Get(), nullptr, IID_PPV_ARGS(&cl_));
}

// gamma ramp as a 256 x 1 lookup
void D3D12Backend::CreateGammaLut() {
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC td = Texture2DDesc(DXGI_FORMAT_B8G8R8A8_UNORM, 256, 1);
    dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                  IID_PPV_ARGS(&lut_));
}

void D3D12Backend::LoadSettings() {
    scene_ = app::RenderResolution();
    scale_ = app::EnhancedRenderScale(guest_h_);
    texturedumper::Update();
    ext_ = g_extension.get();
    if (ext_) {
        ext_->Settings();
        ext_->Reset(*this);
    }
}

void D3D12Backend::Begin() {
    ID3D12DescriptorHeap* heaps[] = {srv_heap_.Get(), smp_heap_.Get()};
    cl_->SetDescriptorHeaps(2, heaps);
    cl_->SetGraphicsRootSignature(root_.Get());
    bound_rt_ = bound_ds_ = nullptr;
    binds_ = {};
}

void D3D12Backend::Submit(bool wait) {
    last_vsc_gpu_ = 0;
    last_dc_gpu_ = 0;
    const HRESULT close = cl_->Close();
    if (FAILED(close)) { DrainDebugMessages("command-list close failure"); ReportDebugDred("command-list close failure"); }
    CheckGpu(dev_.Get(), close, "closing the command list");
    ID3D12CommandList* lists[] = {cl_.Get()};
    queue_->ExecuteCommandLists(1, lists);
    CheckGpu(dev_.Get(), queue_->Signal(fence_.Get(), ++fence_value_), "signalling the submission fence");
    Frame& f = CurrentFrame();
    f.fence = fence_value_;
    if (debug_gpu_ && debug_budget_.Submission()) DrainDebugMessages("initial submission");
    static unsigned reports = 0;
    if (reports++ < 8)
        LOGR("d3d12: submitted frame %llu slot %llu fence %llu, completed %llu, upload %llu bytes%s",
             static_cast<unsigned long long>(frame_), static_cast<unsigned long long>(frame_ % kFrames),
             static_cast<unsigned long long>(fence_value_), static_cast<unsigned long long>(fence_->GetCompletedValue()),
             static_cast<unsigned long long>(f.used), wait ? ", synchronous" : "");
    if (wait) {
        WaitFence(fence_value_);
        CompletePbrFrame(f);
        CheckGpu(dev_.Get(), f.alloc->Reset(), "resetting the synchronous frame allocator");
        f.garbage.clear();
        f.pbr_retained.clear();
        f.used = 0;
        f.srv = 0;
        view_sets_.clear();
        CheckGpu(dev_.Get(), cl_->Reset(f.alloc.Get(), nullptr), "resetting the synchronous command list");
        Begin();
    }
}

void D3D12Backend::Flush() { Submit(true); }

void D3D12Backend::WaitFence(uint64_t value) {
    CheckGpu(dev_.Get(), fence_->SetEventOnCompletion(value, event_), "arming the submission fence event");
    static unsigned reports = 0;
    for (;;) {
        const DWORD result = WaitForSingleObject(event_, 1000);
        const DWORD wait_error = result == WAIT_FAILED ? GetLastError() : ERROR_SUCCESS;
        const uint64_t completed = fence_->GetCompletedValue();
        if (completed == UINT64_MAX) {
            DrainDebugMessages("removed device during fence wait");
            ReportDebugDred("removed device during fence wait");
            CheckGpu(dev_.Get(), DXGI_ERROR_DEVICE_REMOVED, "waiting for GPU completion");
        }
        if (completed >= value) return;  // Only actual completion permits allocator/upload-ring reuse.
        const HRESULT removed = dev_->GetDeviceRemovedReason();
        if (FAILED(removed)) { DrainDebugMessages("device removal during fence wait"); ReportDebugDred("device removal during fence wait"); }
        CheckGpu(dev_.Get(), removed, "waiting for GPU completion");
        if (result == WAIT_FAILED)
            CheckGpu(dev_.Get(), HRESULT_FROM_WIN32(wait_error), "waiting for the submission fence event");
        if (debug_gpu_ && debug_budget_.Pending()) {
            LOGR("d3d12 debug: pending frame %llu slot %llu SRVs %u/%u samplers %u/%u",
                 static_cast<unsigned long long>(frame_), static_cast<unsigned long long>(frame_ % kFrames),
                 CurrentFrame().srv, kSrvCount / kFrames, smp_used_, kSamplerCount);
            DrainDebugMessages("pending GPU fence");
            logx::Flush();
        }
        if (reports++ < 8) {
            LOGR("d3d12: fence still pending after 1 s: frame %llu target %llu completed %llu last signal %llu "
                 "slots [%llu,%llu,%llu], upload %llu bytes, event result 0x%08lX",
                 static_cast<unsigned long long>(frame_), static_cast<unsigned long long>(value),
                 static_cast<unsigned long long>(completed), static_cast<unsigned long long>(fence_value_),
                 static_cast<unsigned long long>(frames_[0].fence), static_cast<unsigned long long>(frames_[1].fence),
                 static_cast<unsigned long long>(frames_[2].fence),
                 static_cast<unsigned long long>(CurrentFrame().used), static_cast<unsigned long>(result));
            logx::Flush();
        }
        if (result == WAIT_OBJECT_0)
            CheckGpu(dev_.Get(), fence_->SetEventOnCompletion(value, event_), "rearming the submission fence event");
    }
}

void D3D12Backend::SetFullViewport(uint32_t w, uint32_t h) {
    D3D12_VIEWPORT vp{0, 0, float(w), float(h), 0, 1};
    D3D12_RECT scissor{0, 0, LONG(w), LONG(h)};
    cl_->RSSetViewports(1, &vp);
    cl_->RSSetScissorRects(1, &scissor);
    binds_ = {};
}

// one w x h colour target for a Blit, the draw's bound targets forgotten
void D3D12Backend::SetBlitTarget(D3D12_CPU_DESCRIPTOR_HANDLE rtv, uint32_t w, uint32_t h) {
    cl_->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    bound_rt_ = bound_ds_ = nullptr;
    SetFullViewport(w, h);
}

D3D12Backend::Alloc D3D12Backend::Upload(uint64_t size, uint64_t align) {
    Frame& f = CurrentFrame();
    g_perf.upload += size;
    uint64_t o = (f.used + align - 1) & ~(align - 1);
    if (o + size <= kUploadSize) {
        f.used = o + size;
        return {f.upload.Get(), o, f.map + o, f.upload->GetGPUVirtualAddress() + o};
    }
    // too big for the ring: a buffer of its own, released with the frame
    g_perf.overflow++;
    ComPtr<ID3D12Resource> r;
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_UPLOAD};
    constexpr uint64_t kOwnAlign = 0x10000;  // a committed buffer's size granularity
    D3D12_RESOURCE_DESC bd = BufferDesc((size + kOwnAlign - 1) & ~(kOwnAlign - 1));
    CheckGpu(dev_.Get(), dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd,
             D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&r)), "allocating a spill upload buffer");
    uint8_t* p = nullptr;
    CheckGpu(dev_.Get(), r->Map(0, nullptr, reinterpret_cast<void**>(&p)), "mapping a spill upload buffer");
    if (!p || !r->GetGPUVirtualAddress()) throw std::runtime_error("Invalid mapped upload buffer.");
    f.garbage.push_back(r);
    return {r.Get(), 0, p, r->GetGPUVirtualAddress()};
}

uint32_t D3D12Backend::AllocSrvSlots(uint32_t n) {
    Frame& f = CurrentFrame();
    const uint32_t per = kSrvCount / kFrames;
    if (f.srv + n > per) {
        LOG_FIRST(4, "d3d12: descriptor ring wrapped within a frame");
        f.srv = 0;
        view_sets_.clear();
    }
    uint32_t i = uint32_t(frame_ % kFrames) * per + f.srv;
    f.srv += n;
    return i;
}

void D3D12Backend::Transition(ID3D12Resource* r, D3D12_RESOURCE_STATES& cur, D3D12_RESOURCE_STATES want) {
    if (cur == want) return;
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = cur;
    b.Transition.StateAfter = want;
    cl_->ResourceBarrier(1, &b);
    cur = want;
}

// a resized target gives back its view and its resources
void D3D12Backend::ReleaseTarget(std::unordered_map<uint64_t, Target>::iterator it) {
    if (bound_rt_ == &it->second || bound_ds_ == &it->second) bound_rt_ = bound_ds_ = nullptr;
    (it->second.depth ? free_dsv_ : free_rtv_).push_back(it->second.view);
    CurrentFrame().garbage.push_back(it->second.res);
    if (it->second.fb) CurrentFrame().garbage.push_back(it->second.fb);
    targets_.erase(it);
}

// the host texture of a new target, at render scale or the extension's
bool D3D12Backend::CreateTargetResource(Target& t, const Surface& s) {
    if (allocation_failed_) return false;
    float sc = scale_;
    if (ext_) ext_->MakeTarget(s, t.depth, t.gw, t.gh, sc, t.fmt);
    app::Mode size = std::fabs(sc - scale_) > 1e-4f
                         ? app::resolution::Scale({t.gw, t.gh}, sc)
                         : app::resolution::Target({t.gw, t.gh}, {guest_w_, guest_h_}, scene_, sc);
    t.w = size.width;
    t.h = size.height;
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC td = Texture2DDesc(DXGI_FORMAT_UNKNOWN, t.w, t.h);
    D3D12_CLEAR_VALUE cv{};
    if (t.depth) {
        td.Format = kDepthTypeless;
        td.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        cv.Format = kDepthFormat;
        cv.DepthStencil.Depth = kDepthFar;
        t.state = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    } else {
        td.Format = t.fmt;
        td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        cv.Format = td.Format;
        t.state = D3D12_RESOURCE_STATE_RENDER_TARGET;
    }
    HRESULT result = dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, t.state, &cv, IID_PPV_ARGS(&t.res));
    if (FAILED(result)) {
        allocation_failed_ = true;
        LOGE("d3d12: target allocation %ux%u failed (%08lX); restoring display selection", t.w, t.h, result);
        app::ReportGraphicsFailure("The GPU could not allocate that scene resolution. Previous display settings were restored. Try a lower scene resolution or render scale.");
        return false;
    }
    return true;
}

// a view from the free pool, cleared; false when the pool is empty
bool D3D12Backend::CreateTargetView(Target& t) {
    auto& pool = t.depth ? free_dsv_ : free_rtv_;
    if (pool.empty()) return false;
    t.view = pool.back();
    pool.pop_back();
    if (t.depth) {
        D3D12_DEPTH_STENCIL_VIEW_DESC dv{};
        dv.Format = kDepthFormat;
        dv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        dev_->CreateDepthStencilView(t.res.Get(), &dv, Cpu(dsv_heap_.Get(), t.view, dsv_inc_));
        cl_->ClearDepthStencilView(Cpu(dsv_heap_.Get(), t.view, dsv_inc_),
                                   D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, kDepthFar, 0, 0, nullptr);
    } else {
        dev_->CreateRenderTargetView(t.res.Get(), nullptr, Cpu(rtv_heap_.Get(), t.view, rtv_inc_));
        float black[4] = {0, 0, 0, 0};
        cl_->ClearRenderTargetView(Cpu(rtv_heap_.Get(), t.view, rtv_inc_), black, 0, nullptr);
    }
    return true;
}

// targets_ key of a surface's colour or depth target
uint64_t TargetMapKey(uint32_t data, bool depth) { return uint64_t(data) << 1 | (depth ? 1 : 0); }

// a smaller depth surface's console size, above the address bits
constexpr int kKeyWidthShift = 34;
constexpr int kKeyHeightShift = 48;

// a smaller depth surface laid on the depth buffer's memory is a target of its own
uint64_t D3D12Backend::SizedTargetKey(uint32_t data, bool depth, uint32_t gw, uint32_t gh) const {
    uint64_t key = TargetMapKey(data, depth);
    bool small_depth = depth && (gw != guest_w_ || gh != guest_h_);
    if (!small_depth) return key;
    return key | uint64_t(gw) << kKeyWidthShift | uint64_t(gh) << kKeyHeightShift;
}

// alt: the extension's bits of the key
D3D12Backend::TargetSlot D3D12Backend::FindTargetSlot(const Surface& s, bool depth, uint64_t alt) {
    TargetSlot slot;
    slot.data = mem::VirtualFromPhysical(s.data);
    SurfaceSize(s, slot.gw, slot.gh);
    slot.key = SizedTargetKey(slot.data, depth, slot.gw, slot.gh) | alt;
    slot.it = targets_.find(slot.key);
    slot.sized = slot.it != targets_.end() && slot.it->second.gw == slot.gw && slot.it->second.gh == slot.gh;
    return slot;
}

// the surface's target at its size, null when there is none
Target* D3D12Backend::FindTarget(const Surface& s, bool depth) {
    if (!s.header) return nullptr;
    uint64_t alt = 0;
    if (ext_ && !ext_->TargetKey(s, depth, alt)) return nullptr;
    TargetSlot slot = FindTargetSlot(s, depth, alt);
    if (!slot.sized) return nullptr;
    UpdateTargetUsed(slot.data);
    return &slot.it->second;
}

// the surface's target, made again at its size when that changed
Target* D3D12Backend::EnsureTarget(const Surface& s, bool depth) {
    if (!s.header) return nullptr;
    uint64_t alt = 0;
    if (ext_ && !ext_->TargetKey(s, depth, alt)) return nullptr;
    TargetSlot slot = FindTargetSlot(s, depth, alt);
    UpdateTargetUsed(slot.data);
    if (slot.sized) return &slot.it->second;
    if (slot.it != targets_.end()) ReleaseTarget(slot.it);
    Target t;
    t.depth = depth;
    t.gw = slot.gw;
    t.gh = slot.gh;
    if (!CreateTargetResource(t, s)) return nullptr;
    g_perf.targets++;
    // an empty pool drops the new resource with t
    if (!CreateTargetView(t)) return nullptr;
    t.data = slot.data;
    t.bytes = GuestPitch(slot.gw) * slot.gh;
    if (depth) t.bytes = slot.gw * slot.gh * 4;
    t.epoch = DirtyEpoch(slot.data);
    t.sig = MemSig(slot.data, t.bytes);
    auto& ref = targets_[slot.key] = std::move(t);
    return &ref;
}

void D3D12Backend::UpdateTargetUsed(uint32_t data) {
    for (const UsedTarget& u : recent_used_) {
        if (u.data == data && u.frame == frame_) return;
    }
    target_used_[data] = frame_;
    recent_used_[recent_used_next_++ % recent_used_.size()] = {data, frame_};
}

bool D3D12Backend::BindTargets(const State& s) {
    Target* rt = EnsureTarget(s.rt, false);
    Target* ds = s.rs[kRsZEnable] || s.rs[kRsStencilEnable] ? EnsureTarget(s.zs, true) : nullptr;
    if (rt && ds && (rt->w != ds->w || rt->h != ds->h)) ds = nullptr;
    if (!rt && !ds) return false;
    if (rt) {
        if (LatchCpuWrite(*rt)) UploadTarget(*rt, s.rt);
        Transition(rt->res.Get(), rt->state, D3D12_RESOURCE_STATE_RENDER_TARGET);
        rt->drawn = true;
    }
    if (ds) {
        Transition(ds->res.Get(), ds->state, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        ds->drawn = true;
    }
    if (rt != bound_rt_ || ds != bound_ds_) {
        D3D12_CPU_DESCRIPTOR_HANDLE rh = rt ? Cpu(rtv_heap_.Get(), rt->view, rtv_inc_) : D3D12_CPU_DESCRIPTOR_HANDLE{};
        D3D12_CPU_DESCRIPTOR_HANDLE dh = ds ? Cpu(dsv_heap_.Get(), ds->view, dsv_inc_) : D3D12_CPU_DESCRIPTOR_HANDLE{};
        cl_->OMSetRenderTargets(rt ? 1 : 0, rt ? &rh : nullptr, FALSE, ds ? &dh : nullptr);
        bound_rt_ = rt;
        bound_ds_ = ds;
    }
    return true;
}

constexpr uint64_t kDepthCopyKey = ~0ull;  // DepthCopy's pipeline in blit_psos_

// DepthCopy's pipeline, kept in blit_psos_: writes depth only, always passing; null when it can't be made
ID3D12PipelineState* D3D12Backend::EnsureDepthCopyPipeline() {
    auto& pso = blit_psos_[kDepthCopyKey];
    if (pso) return pso.Get();
    uint64_t h;
    ID3DBlob* vs = Compile(kDepthHlsl, "vs", "vs_5_0", h);
    ID3DBlob* ps = Compile(kDepthHlsl, "psmain", "ps_5_0", h);
    if (!vs || !ps) return nullptr;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC p{};
    p.pRootSignature = root_.Get();
    p.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    p.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    p.SampleMask = UINT_MAX;
    p.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    p.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    p.RasterizerState.DepthClipEnable = TRUE;
    p.DepthStencilState.DepthEnable = TRUE;
    p.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    p.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    p.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    p.DSVFormat = kDepthFormat;
    p.SampleDesc.Count = 1;
    if (FAILED(dev_->CreateGraphicsPipelineState(&p, IID_PPV_ARGS(&pso)))) return nullptr;
    return pso.Get();
}

// src's depth drawn into dst's depth buffer
void D3D12Backend::DrawDepthCopy(Target& src, Target& dst, ID3D12PipelineState* pso) {
    if (cap_) fprintf(cap_, "%04u depth copy %08X to %08X\n", cap_i_++, src.data, dst.data);
    Transition(src.res.Get(), src.state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    Transition(dst.res.Get(), dst.state, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    D3D12_CPU_DESCRIPTOR_HANDLE dh = Cpu(dsv_heap_.Get(), dst.view, dsv_inc_);
    cl_->OMSetRenderTargets(0, nullptr, FALSE, &dh);
    SetFullViewport(dst.w, dst.h);
    uint32_t slot = AllocSrvSlots(1);
    D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.Format = kDepthRead;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Texture2D.MipLevels = 1;
    dev_->CreateShaderResourceView(src.res.Get(), &sv, Cpu(srv_heap_.Get(), slot, srv_inc_));
    Alloc cb = Upload(256, 256);
    DrawFullScreen(pso, cb.gpu, slot);
    dst.drawn = true;
    bound_rt_ = bound_ds_ = nullptr;
}

// a depth buffer drawn as colour into another depth buffer's memory: copied as depth
bool D3D12Backend::DepthCopy(const State& s) {
    TextureDesc d;
    if (!s.rt.header || !s.textures[0] || !Describe(s.textures[0], d) || Format(d.format).depth || d.cube ||
        d.depth != 1) {
        return false;
    }
    uint32_t gw;
    uint32_t gh;
    SurfaceSize(s.rt, gw, gh);
    auto di = targets_.find(SizedTargetKey(mem::VirtualFromPhysical(s.rt.data), true, gw, gh));
    auto si = targets_.find(SizedTargetKey(d.data, true, d.width, d.height));
    if (di == targets_.end() || si == targets_.end() || di == si) return false;
    Target& dst = di->second;
    Target& src = si->second;
    ID3D12PipelineState* pso = EnsureDepthCopyPipeline();
    if (!pso) return false;
    DrawDepthCopy(src, dst, pso);
    return true;
}

// the pixels the CPU changed since the last Sync, opaque, the rest 0; t.read takes them
std::vector<uint8_t> LatchCpuChanges(Target& t) {
    std::vector<uint8_t> px(t.read.size(), 0);
    uint32_t guest_pitch = GuestPitch(t.gw);
    const uint8_t* guest = static_cast<const uint8_t*>(kernel::HostPtr(t.data));
    for (uint32_t y = 0; y < t.gh; y++) {
        for (uint32_t x = 0; x < t.gw; x++) {
            const uint8_t* c = guest + size_t(y) * guest_pitch + x * 4;
            size_t i = (size_t(y) * t.gw + x) * 4;
            if (!std::memcmp(c, &t.read[i], 4)) continue;
            std::memcpy(&t.read[i], c, 4);
            std::memcpy(&px[i], c, 3);
            px[i + 3] = 255;
        }
    }
    return px;
}

// BGRA pixels into a new w x h texture, left as a shader resource
ComPtr<ID3D12Resource> D3D12Backend::CreatePixelTexture(const std::vector<uint8_t>& px, uint32_t pitch, uint32_t w,
                                                        uint32_t h) {
    ComPtr<ID3D12Resource> tex;
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC td = Texture2DDesc(DXGI_FORMAT_B8G8R8A8_UNORM, w, h);
    dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                  IID_PPV_ARGS(&tex));
    uint32_t row = UploadPitch(w * 4);
    Alloc a = Upload(uint64_t(row) * h, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
    for (uint32_t y = 0; y < h; y++) std::memcpy(a.cpu + size_t(y) * row, &px[size_t(y) * pitch], w * 4);
    D3D12_TEXTURE_COPY_LOCATION dst{tex.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    D3D12_TEXTURE_COPY_LOCATION src{a.res, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    src.PlacedFootprint.Offset = a.off;
    src.PlacedFootprint.Footprint = {DXGI_FORMAT_B8G8R8A8_UNORM, w, h, 1, row};
    cl_->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    D3D12_RESOURCE_STATES st = D3D12_RESOURCE_STATE_COPY_DEST;
    Transition(tex.Get(), st, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    return tex;
}

// CPU pixels of a color surface into its host target, scaled
void D3D12Backend::UploadTarget(Target& t, const Surface& s) {
    g_perf.rtups++;
    TextureDesc d;
    if (!Describe(s.header, d) || Format(d.format).depth) return;
    std::vector<uint8_t> px;
    uint32_t pitch;
    // after a Sync only the pixels the CPU changed go over the picture, which stays at render scale
    bool over = !t.drawn && d.width == t.gw && d.height == t.gh && t.read.size() == size_t(t.gw) * t.gh * 4;
    if (over) {
        LOG_FIRST(4, "d3d12: CPU pixels drawn over target %08X", t.data);
        pitch = t.gw * 4;
        px = LatchCpuChanges(t);
    } else {
        DecodeLevel(d, 0, 0, static_cast<const uint8_t*>(kernel::HostPtr(d.data)), nullptr, px, pitch);
    }
    ComPtr<ID3D12Resource> tex = CreatePixelTexture(px, pitch, d.width, d.height);
    Transition(t.res.Get(), t.state, D3D12_RESOURCE_STATE_RENDER_TARGET);
    SetBlitTarget(Cpu(rtv_heap_.Get(), t.view, rtv_inc_), t.w, t.h);
    float uv[4] = {0, 0, 1, 1};
    float mul = 1 / t.range;
    Blit(tex.Get(), kBgra, uv, {.alpha = true, .mul = mul, .rtv = t.fmt, .over = over});
    CurrentFrame().garbage.push_back(tex);
    t.epoch = DirtyEpoch(t.data);
    t.sig = MemSig(t.data, t.bytes);
}

// samplers_ key of the full screen passes' samplers, apart from the stage samplers' hashed keys
constexpr uint64_t kBlitSamplerKey = 0xB117;

// four clamp samplers: linear at slot 0, point at slots 1 to 3
uint32_t D3D12Backend::EnsureBlitSamplers() {
    uint64_t key = kBlitSamplerKey;
    auto it = samplers_.find(key);
    uint32_t si;
    if (it == samplers_.end()) {
        if (smp_used_ + 4 > kSamplerCount) {
            samplers_.clear();
            smp_used_ = 0;
        }
        si = smp_used_;
        smp_used_ += 4;
        for (int i = 0; i < 4; i++) {
            D3D12_SAMPLER_DESC sd{};
            sd.Filter = i == 0 ? D3D12_FILTER_MIN_MAG_MIP_LINEAR : D3D12_FILTER_MIN_MAG_MIP_POINT;
            sd.AddressU = sd.AddressV = sd.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            sd.MaxLOD = D3D12_FLOAT32_MAX;
            dev_->CreateSampler(&sd, Cpu(smp_heap_.Get(), si + i, smp_inc_));
        }
        samplers_[key] = si;
    } else {
        si = it->second;
    }
    return si;
}

// the full screen triangle: cb at b0 and b1, views at srv_slot; without a sampler slot EnsureBlitSamplers gives it
void D3D12Backend::DrawFullScreen(ID3D12PipelineState* pso, D3D12_GPU_VIRTUAL_ADDRESS cb, uint32_t srv_slot,
                                  std::optional<uint32_t> sampler_slot) {
    binds_ = {};
    cl_->SetPipelineState(pso);
    cl_->SetGraphicsRootConstantBufferView(0, cb);
    cl_->SetGraphicsRootConstantBufferView(1, cb);
    cl_->SetGraphicsRootDescriptorTable(2, Gpu(srv_heap_.Get(), srv_slot, srv_inc_));
    // without a slot the passes take the blit samplers, looked up at this point as they always were
    uint32_t si = sampler_slot ? *sampler_slot : EnsureBlitSamplers();
    cl_->SetGraphicsRootDescriptorTable(3, Gpu(smp_heap_.Get(), si, smp_inc_));
    cl_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cl_->DrawInstanced(3, 1, 0, 0);
}

void D3D12Backend::Blit(ID3D12Resource* src, DXGI_FORMAT fmt, const float uvrect[4], const BlitOptions& o) {
    uint32_t slot = AllocSrvSlots(2);
    D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.Format = fmt;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Texture2D.MipLevels = 1;
    dev_->CreateShaderResourceView(src, &sv, Cpu(srv_heap_.Get(), slot, srv_inc_));
    sv.Format = kBgra;
    dev_->CreateShaderResourceView(lut_.Get(), &sv, Cpu(srv_heap_.Get(), slot + 1, srv_inc_));
    uint32_t si = EnsureBlitSamplers();
    Alloc cb = Upload(256, 256);
    float c[8] = {uvrect[0], uvrect[1], uvrect[2], uvrect[3],
                  o.gamma ? 1.0f : 0.0f, o.alpha ? 1.0f : 0.0f, float(o.taps), o.mul};
    std::memcpy(cb.cpu, c, sizeof(c));
    DrawFullScreen(EnsureBlitPipeline(o.rtv, o.over), cb.gpu, slot, si);
}

ID3DBlob* D3D12Backend::Compile(const std::string& src, const char* entry, const char* profile, uint64_t& hash) {
    // one source can hold several entry points
    uint64_t profile_seed = Hash(profile, std::strlen(profile));
    uint64_t entry_seed = Hash(entry, std::strlen(entry), profile_seed);
    hash = Hash(src.data(), src.size(), entry_seed);
    auto it = blobs_.find(hash);
    if (it != blobs_.end()) return it->second.Get();
    ComPtr<ID3DBlob> code;
    ComPtr<ID3DBlob> err;
    PerfScope perf_compile{g_perf.compile};
    g_perf.shaders++;
    HRESULT hr = D3DCompile(src.data(), src.size(), nullptr, nullptr, nullptr, entry, profile,
                            D3DCOMPILE_OPTIMIZATION_LEVEL1, 0, &code, &err);
    if (FAILED(hr)) {
        LOGE("d3d12: %s compile failed: %s", profile, err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
        LOGE("%s", src.c_str());
        blobs_[hash] = nullptr;
        return nullptr;
    }
    blobs_[hash] = code;
    return code.Get();
}

// a full screen pass's pipeline: solid, no culling, one target, writing all channels; callers set the blend
D3D12_GRAPHICS_PIPELINE_STATE_DESC D3D12Backend::FullScreenPipelineDesc(ID3DBlob* vs, ID3DBlob* ps,
                                                                        DXGI_FORMAT rtv) const {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC d{};
    d.pRootSignature = root_.Get();
    d.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    d.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    d.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    d.SampleMask = UINT_MAX;
    d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    d.RasterizerState.DepthClipEnable = TRUE;
    d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    d.NumRenderTargets = 1;
    d.RTVFormats[0] = rtv;
    d.SampleDesc.Count = 1;
    return d;
}

ID3D12PipelineState* D3D12Backend::EnsureBlitPipeline(DXGI_FORMAT rtv, bool over) {
    auto& pso = blit_psos_[uint64_t(rtv) | uint64_t(over) << 32];
    if (pso) return pso.Get();
    uint64_t h;
    ID3DBlob* vs = Compile(kBlitHlsl, "vs", "vs_5_0", h);
    ID3DBlob* ps = Compile(kBlitHlsl, "psmain", "ps_5_0", h);
    D3D12_GRAPHICS_PIPELINE_STATE_DESC d = FullScreenPipelineDesc(vs, ps, rtv);
    if (over) {
        auto& blend = d.BlendState.RenderTarget[0];
        // premultiplied colour, the target keeps its alpha
        blend.BlendEnable = TRUE;
        blend.SrcBlend = D3D12_BLEND_ONE;
        blend.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        blend.SrcBlendAlpha = D3D12_BLEND_ZERO;
        blend.DestBlendAlpha = D3D12_BLEND_ONE;
        blend.BlendOp = blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    }
    dev_->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&pso));
    return pso.Get();
}

// Pass's pipeline for the shader, kept in blit_psos_; null when it can't be made
ID3D12PipelineState* D3D12Backend::EnsurePassPipeline(const char* hlsl, const char* entry, DXGI_FORMAT rtv,
                                                      float blend) {
    uint64_t seed = uint64_t(rtv) << 1 | (blend >= 0 ? 1 : 0);
    uint64_t source_seed = Hash(hlsl, std::strlen(hlsl), seed);
    auto& pso = blit_psos_[Hash(entry, std::strlen(entry), source_seed)];
    if (pso) return pso.Get();
    uint64_t h;
    ID3DBlob* vs = Compile(hlsl, "vs", "vs_5_0", h);
    ID3DBlob* ps = Compile(hlsl, entry, "ps_5_0", h);
    if (!vs || !ps) return nullptr;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC d = FullScreenPipelineDesc(vs, ps, rtv);
    if (blend >= 0) {
        auto& b = d.BlendState.RenderTarget[0];
        b.BlendEnable = TRUE;
        b.SrcBlend = b.SrcBlendAlpha = D3D12_BLEND_BLEND_FACTOR;
        b.DestBlend = b.DestBlendAlpha = D3D12_BLEND_INV_BLEND_FACTOR;
        b.BlendOp = b.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    }
    if (FAILED(dev_->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&pso)))) return nullptr;
    return pso.Get();
}

void D3D12Backend::Pass(const char* hlsl, const char* entry, DXGI_FORMAT rtv, D3D12_CPU_DESCRIPTOR_HANDLE rh,
                        uint32_t w, uint32_t h, ID3D12Resource* const src[3], const DXGI_FORMAT fmt[3],
                        const float p[8], float blend) {
    ID3D12PipelineState* pso = EnsurePassPipeline(hlsl, entry, rtv, blend);
    if (!pso) return;
    uint32_t slot = AllocSrvSlots(4);
    for (int i = 0; i < 4; i++) {
        D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
        sv.Format = i < 3 && src[i] ? fmt[i] : kBgra;
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sv.Texture2D.MipLevels = 1;
        dev_->CreateShaderResourceView(i < 3 ? src[i] : nullptr, &sv, Cpu(srv_heap_.Get(), slot + i, srv_inc_));
    }
    Alloc cb = Upload(256, 256);
    std::memcpy(cb.cpu, p, 8 * sizeof(float));
    cl_->OMSetRenderTargets(1, &rh, FALSE, nullptr);
    SetFullViewport(w, h);
    if (blend >= 0) {
        float f[4] = {blend, blend, blend, blend};
        cl_->OMSetBlendFactor(f);
        binds_ = {};
    }
    DrawFullScreen(pso, cb.gpu, slot);
    bound_rt_ = bound_ds_ = nullptr;
}

// a copy of the target to read while drawing to it
ID3D12Resource* D3D12Backend::Feedback(Target& t) {
    if (!t.fb) {
        D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
        D3D12_RESOURCE_DESC td = t.res->GetDesc();
        td.Flags = D3D12_RESOURCE_FLAG_NONE;
        t.fb_state = D3D12_RESOURCE_STATE_COPY_DEST;
        if (FAILED(dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, t.fb_state, nullptr,
                                                 IID_PPV_ARGS(&t.fb)))) {
            return nullptr;
        }
    }
    Transition(t.res.Get(), t.state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Transition(t.fb.Get(), t.fb_state, D3D12_RESOURCE_STATE_COPY_DEST);
    cl_->CopyResource(t.fb.Get(), t.res.Get());
    Transition(t.fb.Get(), t.fb_state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    return t.fb.Get();
}

// the key's vertex attributes, ATTR0 to ATTR15 in register order; returns how many
uint32_t FillInputLayout(const PsoKey& k, D3D12_INPUT_ELEMENT_DESC el[16]) {
    uint32_t n = 0;
    for (uint32_t r = 0; r < 16; r++) {
        if (!((k.regs >> r) & 1)) continue;
        uint32_t e = k.elements[r];
        el[n++] = {"ATTR", r, DXGI_FORMAT(e & 0xFF), (e >> 8) & 0xFF, e >> 16,
                   D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0};
    }
    return n;
}

D3D12_PRIMITIVE_TOPOLOGY_TYPE TopologyType(uint32_t topo) {
    switch (topo) {
        case 0: return D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
        case 1: return D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
        default: return D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    }
}

constexpr uint32_t kFillWireframe = 0x1B01;  // D3DFILL_WIREFRAME
constexpr uint32_t kCullCw = 0x900;          // D3DCULL_CW

// a D3DCMP test on reversed depth: less and greater trade places
D3D12_COMPARISON_FUNC ReversedCmp(uint32_t v) {
    static const uint8_t mirror[8] = {0, 4, 2, 6, 1, 5, 3, 7};
    return D3D12_COMPARISON_FUNC(mirror[Cmp(v) - 1] + 1);
}

void FillDepthStencilDesc(const PsoKey& k, D3D12_DEPTH_STENCIL_DESC& ds) {
    ds.DepthEnable = k.zen;
    ds.DepthWriteMask = k.zwrite ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
    ds.DepthFunc = ReversedCmp(k.zfunc);
    ds.StencilEnable = k.sten;
    ds.StencilReadMask = UINT8(k.smask);
    ds.StencilWriteMask = UINT8(k.swmask);
    D3D12_DEPTH_STENCILOP_DESC so{StencilOp(k.sfail), StencilOp(k.szfail), StencilOp(k.spass), Cmp(k.sfunc)};
    ds.FrontFace = ds.BackFace = so;
}

// everything of the pipeline but its input layout
void D3D12Backend::FillPipelineDesc(const PsoKey& k, ID3DBlob* vs, ID3DBlob* ps,
                                    D3D12_GRAPHICS_PIPELINE_STATE_DESC& d) const {
    d.pRootSignature = root_.Get();
    d.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    d.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    auto& b = d.BlendState.RenderTarget[0];
    b.BlendEnable = k.blend;
    b.SrcBlend = Blend(k.src, false);
    b.DestBlend = Blend(k.dst, false);
    b.BlendOp = BlendOp(k.op);
    b.SrcBlendAlpha = Blend(k.src, true);
    b.DestBlendAlpha = Blend(k.dst, true);
    b.BlendOpAlpha = BlendOp(k.op);
    b.RenderTargetWriteMask = UINT8(k.wmask);
    d.SampleMask = UINT_MAX;
    d.RasterizerState.FillMode = k.fill == kFillWireframe ? D3D12_FILL_MODE_WIREFRAME : D3D12_FILL_MODE_SOLID;
    d.RasterizerState.CullMode = k.cull ? D3D12_CULL_MODE_BACK : D3D12_CULL_MODE_NONE;
    d.RasterizerState.FrontCounterClockwise = k.ccw;
    d.RasterizerState.DepthBias = k.zbias;
    d.RasterizerState.SlopeScaledDepthBias = k.zslope;
    d.RasterizerState.DepthClipEnable = TRUE;
    FillDepthStencilDesc(k, d.DepthStencilState);
    d.PrimitiveTopologyType = TopologyType(k.topo);
    d.NumRenderTargets = k.has_rt ? 1 : 0;
    d.RTVFormats[0] = k.has_rt ? DXGI_FORMAT(k.rtfmt) : DXGI_FORMAT_UNKNOWN;
    d.DSVFormat = k.has_ds ? kDepthFormat : DXGI_FORMAT_UNKNOWN;
    d.SampleDesc.Count = 1;
}

// A point sprite is a square of the program's point size in console pixels (oPts, which SpriteSizeHlsl passes in
// tc3.x) with the sprite's coordinates across it, 0 to 1 from the top left, as the console makes them for stage 3.
std::string SpriteHlsl() {
    std::string s = PointHlsl();
    s.erase(s.find("[maxvertexcount"));
    return s + R"([maxvertexcount(4)]
void main(point VOut v[1], inout TriangleStream<VOut> strip) {
    float2 half_size = v[0].tc3.x * v[0].pos.w / surf.xy;  // in clip units, the target spans 2 across surf.xy pixels
    for (int i = 0; i < 4; i++) {
        VOut o = v[0];
        float2 corner = float2(i & 1, i >> 1);
        o.pos.x += (corner.x * 2 - 1) * half_size.x;
        o.pos.y -= (corner.y * 2 - 1) * half_size.y;
        o.tc0 = float4(corner, 0, 1);  // stage 3 drawn as stage 0 (SpriteStageZero)
        o.tc3 = o.tc0;
        strip.Append(o);
    }
}
)";
}

// a point is drawn as its whole console pixel, or a sprite as its square, by a geometry shader, solid, front facing
// and never culled
void D3D12Backend::ExpandPoints(D3D12_GRAPHICS_PIPELINE_STATE_DESC& d, bool sprite) {
    uint64_t hash;
    ID3DBlob* gs = Compile(sprite ? SpriteHlsl() : PointHlsl(), "main", "gs_5_0", hash);
    if (!gs) return;
    d.GS = {gs->GetBufferPointer(), gs->GetBufferSize()};
    d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    d.RasterizerState.FrontCounterClockwise = FALSE;
}

float D3D12Backend::TargetScale() const {
    const Target* t = bound_rt_ ? bound_rt_ : bound_ds_;
    if (!t || !t->gw) return 1;
    return float(t->w) / float(t->gw);
}

// the XDK's screen space offset puts the console's positions half a pixel right and down of the host's, unless a
// program writes them without c-37; render target reads move back that far (TexUv)
float D3D12Backend::RenderTargetShift(const State& s, const DrawShaders& sh) const {
    bool offset = !sh.prog || ReadsViewportOffset(s.vs->program);
    return offset ? -0.5f * TargetScale() : 0.0f;
}

ID3D12PipelineState* D3D12Backend::EnsurePipeline(const PsoKey& k, ID3DBlob* vs, ID3DBlob* ps) {
    uint64_t h = Hash(&k, sizeof(k));
    auto it = psos_.find(h);
    if (it != psos_.end()) return it->second.Get();
    D3D12_INPUT_ELEMENT_DESC el[16];
    uint32_t n = FillInputLayout(k, el);
    D3D12_GRAPHICS_PIPELINE_STATE_DESC d{};
    d.InputLayout = {el, n};
    FillPipelineDesc(k, vs, ps, d);
    if (k.topo == uint32_t(Topology::Points)) ExpandPoints(d, k.sprite);
    ComPtr<ID3D12PipelineState> pso;
    g_perf.psos++;
    if (FAILED(dev_->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&pso)))) LOGE("d3d12: pipeline creation failed");
    psos_[h] = pso;
    return pso.Get();
}

TexKind TextureKind(const TextureDesc& d) {
    if (d.cube) return kTexCube;
    if (d.depth > 1) return kTex3D;
    return kTex2D;
}

constexpr float kUpscaledMin = 1.01f;  // host texels per console texel past rounding: the target is upscaled

ID3D12Resource* D3D12Backend::BindTargetTexture(const State& s, int stage, const TextureDesc& d, Target& t, bool box,
                                                DrawTextures& stages) {
    TexInfo& info = stages.info[stage];
    D3D12_SHADER_RESOURCE_VIEW_DESC& sv = stages.sv[stage];
    if (&t == draw_ds_) return nullptr;
    bool feedback = &t == draw_rt_;
    stages.rt[stage] = true;
    stages.tgt[stage] = &t;
    info.flags |= kTexTarget;
    if (!t.depth && LatchCpuWrite(t)) {
        Surface su;
        su.header = s.textures[size_t(stage)];
        UploadTarget(t, su);
    }
    info.kind = kTex2D;
    // a pass shrinking it averages each tap over a console texel, else upscaled taps skip texels and ghost
    Target* rt = FindTarget(s.rt, false);
    float k = std::max(float(t.w) / t.gw, float(t.h) / t.gh) / stages.k[stage];
    if (!t.depth && box && k > kUpscaledMin && rt && rt != &t && (t.gw > rt->gw || t.gh > rt->gh)) {
        stages.wide[stage] = k;
        info.flags |= kTexWide;
    }
    if (ext_) ext_->Sampled(d, &t, 0, info);
    sv.Format = t.depth ? kDepthRead : t.fmt;
    // the console reads the pixels it is about to overwrite: give the draw a copy
    if (feedback) return Feedback(t);
    Transition(t.res.Get(), t.state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    if (t.depth) info.shadow = ShadowDepthKind(d.format);
    info.kind = kTex2D;
    return t.res.Get();
}

bool SameTexture(const Tex& tx, const TextureDesc& d) {
    return tx.res && tx.d.format == d.format && tx.d.width == d.width && tx.d.height == d.height &&
           tx.d.depth == d.depth && tx.d.levels == d.levels && tx.d.cube == d.cube && tx.d.pitch == d.pitch;
}

// false when the texture could not be made, its entry dropped
bool D3D12Backend::CreateTexture(Tex& tx, const TextureDesc& d, const FormatInfo& fi, uint64_t key) {
    g_perf.newtex++;
    if (tx.res) CurrentFrame().garbage.push_back(tx.res);
    if (tx.pbr_nrm) CurrentFrame().garbage.push_back(tx.pbr_nrm);
    if (tx.pbr_orm) CurrentFrame().garbage.push_back(tx.pbr_orm);
    tx = Tex{};
    tx.d = d;
    tx.fmt = HostFormat(fi.host);
    bool bc = fi.compressed;
    uint32_t w = d.width;
    uint32_t h = d.height;
    tx.levels = d.levels;
    if (bc && ((w & 3) || (h & 3))) {
        w = (w + 3) & ~3u;
        h = (h + 3) & ~3u;
        tx.levels = 1;
    }
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC td{};
    td.Dimension = d.depth > 1 ? D3D12_RESOURCE_DIMENSION_TEXTURE3D : D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = w;
    td.Height = h;
    td.DepthOrArraySize = UINT16(d.cube ? 6 : d.depth);
    td.MipLevels = UINT16(tx.levels);
    td.Format = tx.fmt;
    td.SampleDesc.Count = 1;
    if (FAILED(dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                             IID_PPV_ARGS(&tx.res)))) {
        textures_.erase(key);
        return false;
    }
    return true;
}

void CopyLevelRows(uint8_t* out, const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& fp, UINT rows, UINT64 row_size,
                   const std::vector<uint8_t>& px, uint32_t pitch) {
    uint32_t slices = fp.Footprint.Depth;
    size_t src_rows = pitch ? px.size() / pitch / std::max(slices, 1u) : 0;
    for (uint32_t z = 0; z < slices; z++) {
        for (uint32_t y = 0; y < rows; y++) {
            uint8_t* dst = out + (size_t(z) * rows + y) * fp.Footprint.RowPitch;
            if (y < src_rows) {
                std::memcpy(dst, &px[(size_t(z) * src_rows + y) * pitch], std::min<size_t>(pitch, row_size));
            } else {
                std::memset(dst, 0, size_t(row_size));
            }
        }
    }
}

void D3D12Backend::UploadTextureLevel(Tex& tx, uint32_t f, uint32_t l, const std::vector<uint8_t>& px,
                                      uint32_t pitch) {
    D3D12_RESOURCE_DESC rd = tx.res->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;
    UINT rows;
    UINT64 row_size;
    UINT64 bytes;
    uint32_t sub = f * tx.levels + l;
    dev_->GetCopyableFootprints(&rd, sub, 1, 0, &fp, &rows, &row_size, &bytes);
    Alloc a = Upload(bytes, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
    CopyLevelRows(a.cpu, fp, rows, row_size, px, pitch);
    fp.Offset = a.off;
    D3D12_TEXTURE_COPY_LOCATION dl{tx.res.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    dl.SubresourceIndex = sub;
    D3D12_TEXTURE_COPY_LOCATION sl{a.res, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    sl.PlacedFootprint = fp;
    cl_->CopyTextureRegion(&dl, 0, 0, 0, &sl, nullptr);
}

bool D3D12Backend::UploadReplacement(Tex& tx, const pack::Image& img) {
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = img.width;
    td.Height = img.height;
    td.DepthOrArraySize = 1;
    td.MipLevels = UINT16(img.levels.size());
    td.Format = kBgra;
    td.SampleDesc.Count = 1;
    ComPtr<ID3D12Resource> res;
    if (FAILED(dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                             IID_PPV_ARGS(&res))))
        return false;
    if (tx.res) CurrentFrame().garbage.push_back(tx.res);
    if (tx.pbr_nrm) CurrentFrame().garbage.push_back(tx.pbr_nrm);
    if (tx.pbr_orm) CurrentFrame().garbage.push_back(tx.pbr_orm);
    tx.res = res;
    tx.fmt = kBgra;
    tx.levels = uint32_t(img.levels.size());
    tx.replaced = true;
    tx.pbr_color = {};
    if (pbr_library_.library && !img.levels.empty())
        tx.pbr_color = pbr::ProveUploadedColor(Host::BGRA8, img.width, img.height, img.width * 4, img.levels[0]);
    tx.pbr_proof_epoch = pbr_proof_epoch_;
    uint32_t w = img.width, h = img.height;
    for (uint32_t l = 0; l < tx.levels; l++) {
        if (pbr_library_.library && l > 0) {
            const auto level=pbr::ProveUploadedColor(Host::BGRA8,w,h,w*4,img.levels[l]);
            tx.pbr_color.constant_alpha &= level.ready() && level.constant_alpha && level.alpha==tx.pbr_color.alpha;
        }
        UploadTextureLevel(tx, 0, l, img.levels[l], w * 4);
        w = std::max(w / 2, 1u);
        h = std::max(h / 2, 1u);
    }
    D3D12_RESOURCE_STATES st = D3D12_RESOURCE_STATE_COPY_DEST;
    Transition(tx.res.Get(), st, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    return true;
}

ComPtr<ID3D12Resource> D3D12Backend::UploadPackImage(const pack::Image& img) {
    if (!img.width || !img.height || img.levels.empty()) return nullptr;
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = img.width;
    td.Height = img.height;
    td.DepthOrArraySize = 1;
    td.MipLevels = UINT16(img.levels.size());
    td.Format = kBgra;
    td.SampleDesc.Count = 1;
    ComPtr<ID3D12Resource> res;
    if (FAILED(dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                             IID_PPV_ARGS(&res))))
        return nullptr;
    Tex tmp{};
    tmp.res = res;
    tmp.levels = uint32_t(img.levels.size());
    uint32_t w = img.width;
    for (uint32_t l = 0; l < tmp.levels; l++) {
        UploadTextureLevel(tmp, 0, l, img.levels[l], w * 4);
        w = std::max(w / 2, 1u);
    }
    D3D12_RESOURCE_STATES st = D3D12_RESOURCE_STATE_COPY_DEST;
    Transition(res.Get(), st, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    return res;
}

void D3D12Backend::UploadTexture(Tex& tx, const TextureDesc& d, const FormatInfo& fi, const uint8_t* p,
                                 const uint32_t* colours, bool same, uint64_t dump) {
    D3D12_RESOURCE_STATES st = same ? D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE : D3D12_RESOURCE_STATE_COPY_DEST;
    Transition(tx.res.Get(), st, D3D12_RESOURCE_STATE_COPY_DEST);
    uint32_t faces = d.cube ? 6 : 1;
    std::vector<uint8_t> px;
    tx.pbr_color = {};
    tx.pbr_proof_epoch = pbr_proof_epoch_;
    for (uint32_t f = 0; f < faces; f++) {
        for (uint32_t l = 0; l < tx.levels; l++) {
            uint32_t pitch;
            DecodeLevel(d, l, f, p, colours, px, pitch);
            if (dump && f == 0 && l == 0) pack::Dump(dump, d, fi.host, px, pitch);
            if (ext_) ext_->Decoded(d, fi, f, l, px, pitch, tx.flags);
            if (pbr_library_.library && f == 0 && faces == 1 && d.depth == 1 && !fi.depth) {
                const auto level=pbr::ProveUploadedColor(fi.host,std::max(1u,d.width>>l),std::max(1u,d.height>>l),pitch,px);
                if (l==0) tx.pbr_color=level;
                else tx.pbr_color.constant_alpha &= level.ready() && level.constant_alpha && level.alpha==tx.pbr_color.alpha;
            }
            UploadTextureLevel(tx, f, l, px, pitch);
        }
    }
    Transition(tx.res.Get(), st, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
}

void FillTextureView(const Tex& tx, const TextureDesc& d, D3D12_SHADER_RESOURCE_VIEW_DESC& sv) {
    sv.Format = tx.fmt;
    if (d.cube) {
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
        sv.TextureCube.MipLevels = tx.levels;
    } else if (d.depth > 1) {
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
        sv.Texture3D.MipLevels = tx.levels;
    } else {
        sv.Texture2D.MipLevels = tx.levels;
    }
}

// a P8 texture's palette: its colours, their address (part of the texture's key) and their hash, all 0 without one
struct StagePalette {
    uint32_t colours[256] = {};
    uint32_t va = 0;
    uint64_t hash = 0;
};

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

// a texture in guest memory: made again when its layout changed, uploaded again when its bytes did
ID3D12Resource* D3D12Backend::BindGuestTexture(const State& s, int stage, const TextureDesc& d, const FormatInfo& fi,
                                               DrawTextures& stages) {
    TexInfo& info = stages.info[stage];
    StagePalette palette;
    ReadStagePalette(s, stage, d, palette);
    const uint64_t key = uint64_t(palette.va) << 32 | d.data;
    const uint32_t* colours = palette.va ? palette.colours : nullptr;
    Tex& tx = textures_[key];
    uint32_t total = TotalSize(d);
    bool same = SameTexture(tx, d);
    if (!same && !CreateTexture(tx, d, fi, key)) return nullptr;
    const bool pack_enabled = app::Get().graphics.texture_pack;
    const bool selection_changed = tx.pack_enabled != pack_enabled;
    const bool proof_needed = pbr_library_.library && tx.pbr_proof_epoch != pbr_proof_epoch_;
    // cheap change check once per frame: lock epochs plus a sample of the bytes
    if (tx.frame != presents_ + 1 || !same || tx.palette != palette.hash || selection_changed || proof_needed) {
        tx.frame = presents_ + 1;
        tx.palette = palette.hash;
        const uint8_t* p = static_cast<const uint8_t*>(kernel::HostPtr(d.data));
        auto perf_t = PerfClock::now();
        uint64_t sig = MemSig(d.data, total) ^ DirtyEpoch(d.data) ^ palette.hash;
        g_perf.sig += PerfMs(perf_t);
        const bool content_changed = sig != tx.sig || !same;
        if (content_changed || selection_changed || proof_needed) {
            PerfScope perf_up{g_perf.texup};
            g_perf.texups++;
            g_perf.texup_bytes += total;
            tx.sig = sig;
            // a texture pack: the console's bytes name the texture, a replacement takes its place
            constexpr uint32_t kDynamicUploads = 4;
            uint64_t name = 0;
            bool packable = !d.cube && d.depth <= 1 && !fi.depth && tx.uploads < kDynamicUploads;
            if (content_changed) tx.uploads++;
            bool replace = packable && pack::ReplaceEnabled(), dump = packable && pack::DumpEnabled();
            bool roled = fi.compressed && d.width <= kRoleMaxSize && d.height <= kRoleMaxSize;
            const bool photo = photo_policy_.Armed() && d.format == 0x0F && d.width == 256 && d.height == 256
                               && !d.cube && d.depth == 1;
            if (replace || dump || roled || photo || packable) name = pack::ContentHash(d, p, total, palette.hash);
            if (!photo_capture::UploadWithIdentity(tx.photo_identity, name, [&]() -> bool {
                auto img = replace ? pack::Find(name) : nullptr;
                if (!(img && UploadReplacement(tx, *img))) {
                    if (tx.replaced) {
                        // its bytes changed to a texture the pack has no image for: back to the console's own
                        uint32_t uploads = tx.uploads;
                        if (!CreateTexture(tx, d, fi, key)) return false;
                        tx.uploads = uploads;
                        tx.sig = sig;
                        tx.frame = presents_ + 1;
                        tx.palette = palette.hash;
                        same = false;
                    }
                    UploadTexture(tx, d, fi, p, palette.va ? palette.colours : nullptr, same, dump ? name : 0);
                }
                return tx.res.Get() != nullptr;
            })) return nullptr;
            tx.pack_enabled = pack_enabled;
            tx.pack_gen = 0;  // new bytes: texturedumper keys them again
            tx.role = roled ? TextureRole(name) : kRoleNone;
            if (content_changed) {
                if (tx.pbr_nrm) CurrentFrame().garbage.push_back(std::move(tx.pbr_nrm));
                if (tx.pbr_orm) CurrentFrame().garbage.push_back(std::move(tx.pbr_orm));
                tx.pbr_hash = packable ? name : 0;
                tx.pbr_done = !packable;
                tx.pbr_has_height = false;
                tx.pbr_draws = 0;
            }
        }
    }
    stages.accepted_color[stage] = &tx;
    if (stage == 0 && !tx.pbr_done && tx.pbr_hash != 0 && pack::PbrEnabled()) {
        std::unique_ptr<pack::PbrMaps> maps;
        const auto poll = pack::PollPbr(tx.pbr_hash, maps);
        if (poll == pack::PbrPoll::None) {
            tx.pbr_done = true;
        } else if (poll == pack::PbrPoll::Ready && maps) {
            tx.pbr_nrm = UploadPackImage(maps->nrm);
            tx.pbr_orm = UploadPackImage(maps->orm);
            tx.pbr_levels = uint32_t(maps->nrm.levels.size());
            tx.pbr_has_height = maps->has_height;
            tx.pbr_done = true;
        }
    }
    if (stage == 0 && tx.pbr_nrm && tx.pbr_orm && app::Get().pbr.enabled) {
        stages.pbr_nrm = tx.pbr_nrm.Get();
        stages.pbr_orm = tx.pbr_orm.Get();
        stages.pbr_levels = tx.pbr_levels;
        stages.pbr_has_height = tx.pbr_has_height;
        stages.pbr_tex = &tx;
    }
    if (tx.pack_gen != texturedumper::Generation()) LatchPackKey(tx, d, fi, colours);
    if (tx.pack_key) LatchPackImage(tx);
    const Tex& drawn = tx.swap ? *tx.swap : tx;
    FillTextureView(drawn, d, stages.sv[stage]);
    stages.role[stage] = tx.role;
    stages.photo_identity[stage] = tx.photo_identity;
    stages.photo_replaced[stage] = tx.replaced;
    if (fi.depth) info.shadow = ShadowDepthKind(d.format);
    if (ext_) ext_->Sampled(d, nullptr, tx.flags, info);
    return drawn.res.Get();
}

// after new bytes or a texture setting change: the name the pack and the dumps know the bytes by
void D3D12Backend::LatchPackKey(Tex& tx, const TextureDesc& d, const FormatInfo& fi, const uint32_t* colours) {
    const uint8_t* p = static_cast<const uint8_t*>(kernel::HostPtr(d.data));
    tx.pack_gen = texturedumper::Generation();
    tx.swap = nullptr;
    uint64_t key = texturedumper::Key(d, fi, p, colours);
    texturedumper::Dump(key, d, p, colours);
    // dumping alone keys the bytes too: the pack image is not drawn then
    tx.pack_key = texturedumper::Replacing() ? key : 0;
}

// the pack image for tx's bytes once it has loaded; textures with the same bytes share its GPU copy
void D3D12Backend::LatchPackImage(Tex& tx) {
    auto it = pack_textures_.find(tx.pack_key);
    if (it == pack_textures_.end()) {
        texturedumper::Image img;
        texturedumper::Lookup got = texturedumper::Take(tx.pack_key, img);
        if (got == texturedumper::Lookup::Loading) return;
        if (got == texturedumper::Lookup::None) {
            tx.pack_key = 0;
            return;
        }
        it = pack_textures_.emplace(tx.pack_key, Tex{}).first;
        CreatePackTexture(it->second, img);
    }
    if (it->second.res) tx.swap = &it->second;
    tx.pack_key = 0;
}

void D3D12Backend::CreatePackTexture(Tex& r, const texturedumper::Image& img) {
    r.fmt = img.format;
    r.levels = uint32_t(img.levels.size());
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC td = Texture2DDesc(r.fmt, img.width, img.height);
    td.MipLevels = UINT16(r.levels);
    if (FAILED(dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                             IID_PPV_ARGS(&r.res)))) {
        LOGE("texture pack: a %ux%u image could not be made", img.width, img.height);
        return;
    }
    for (uint32_t l = 0; l < r.levels; l++) UploadTextureLevel(r, 0, l, img.levels[l].px, img.levels[l].pitch);
    D3D12_RESOURCE_STATES st = D3D12_RESOURCE_STATE_COPY_DEST;
    Transition(r.res.Get(), st, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
}

// the pack's files changed: each image is made again when next drawn; Generation moved on, so every texture lets
// go of its old copy before it draws
void D3D12Backend::ReleasePackTextures() {
    for (auto& entry : pack_textures_) CurrentFrame().garbage.push_back(entry.second.res);
    pack_textures_.clear();
}

ID3D12Resource* D3D12Backend::BindTexture(const State& s, int stage, DrawTextures& stages) {
    TexInfo& info = stages.info[stage];
    D3D12_SHADER_RESOURCE_VIEW_DESC& sv = stages.sv[stage];
    info = {};
    sv = {};
    stages.rt[stage] = false;
    stages.k[stage] = 1;
    stages.wide[stage] = 0;
    stages.role[stage] = kRoleNone;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sv.Texture2D.MipLevels = 1;
    TextureDesc d;
    if (!s.textures[size_t(stage)] || !Describe(s.textures[size_t(stage)], d)) return nullptr;
    const FormatInfo& fi = Format(d.format);
    info.kind = TextureKind(d);
    info.rect = d.linear;

    // rendered by the GPU: sample the target itself; DXT or P8 data loaded over a target's memory is not it
    uint64_t alt = 0;
    bool box = true;
    if (ext_) ext_->Sample(d.data, fi.depth, stage, alt, stages.k[stage], box);
    auto tit = targets_.find(SizedTargetKey(d.data, fi.depth, d.width, d.height) | alt);
    if (tit != targets_.end() && !d.cube && d.depth == 1 && CanBeTargetFormat(d.format, fi.compressed)) {
        return BindTargetTexture(s, stage, d, tit->second, box, stages);
    }
    if (video::Playing(d.data))
        if (ID3D12Resource* r = BindMovie(d, stage, stages)) return r;
    return BindGuestTexture(s, stage, d, fi, stages);
}

ID3D12Resource* D3D12Backend::BindMovie(const TextureDesc& d, int stage, DrawTextures& stages) {
    Tex& tx = movies_[d.data];
    bool ok = video::Picture(d.data, tx.sig, [&](const uint8_t* px, uint32_t w, uint32_t h) {
        D3D12_RESOURCE_STATES st = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        if (!tx.res || tx.res->GetDesc().Width != w || tx.res->GetDesc().Height != h) {
            D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
            D3D12_RESOURCE_DESC td{};
            td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            td.Width = w;
            td.Height = h;
            td.DepthOrArraySize = 1;
            td.MipLevels = 1;
            td.Format = kBgra;
            td.SampleDesc.Count = 1;
            ComPtr<ID3D12Resource> res;
            if (FAILED(dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST,
                                                     nullptr, IID_PPV_ARGS(&res))))
                return;
            if (tx.res) CurrentFrame().garbage.push_back(tx.res);
    if (tx.pbr_nrm) CurrentFrame().garbage.push_back(tx.pbr_nrm);
    if (tx.pbr_orm) CurrentFrame().garbage.push_back(tx.pbr_orm);
            tx.res = res;
            tx.fmt = kBgra;
            tx.levels = 1;
            st = D3D12_RESOURCE_STATE_COPY_DEST;
        } else {
            Transition(tx.res.Get(), st, D3D12_RESOURCE_STATE_COPY_DEST);
        }
        UploadTextureLevel(tx, 0, 0, std::vector<uint8_t>(px, px + size_t(w) * h * 4), w * 4);
        Transition(tx.res.Get(), st, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    });
    if (!ok || !tx.res) return nullptr;
    if (stage == 0 && !tx.pbr_done && tx.pbr_hash != 0 && pack::PbrEnabled()) {
        std::unique_ptr<pack::PbrMaps> maps;
        const auto poll = pack::PollPbr(tx.pbr_hash, maps);
        if (poll == pack::PbrPoll::None) {
            tx.pbr_done = true;
        } else if (poll == pack::PbrPoll::Ready && maps) {
            tx.pbr_nrm = UploadPackImage(maps->nrm);
            tx.pbr_orm = UploadPackImage(maps->orm);
            tx.pbr_levels = uint32_t(maps->nrm.levels.size());
            tx.pbr_has_height = maps->has_height;
            tx.pbr_done = true;
        }
    }
    if (stage == 0 && tx.pbr_nrm && tx.pbr_orm && app::Get().pbr.enabled) {
        stages.pbr_nrm = tx.pbr_nrm.Get();
        stages.pbr_orm = tx.pbr_orm.Get();
        stages.pbr_levels = tx.pbr_levels;
        stages.pbr_has_height = tx.pbr_has_height;
        stages.pbr_tex = &tx;
    }
    FillTextureView(tx, d, stages.sv[stage]);
    return tx.res.Get();
}

// D3DTEXF: 1 point, 2 linear, 3 anisotropic
constexpr uint32_t kTexfLinear = 2;
constexpr uint32_t kTexfAnisotropic = 3;
// D3DTADDRESS values the samplers test
constexpr uint32_t kTaddressWrap = 1;
constexpr uint32_t kTaddressClamp = 3;

// texture detail (Settings::Graphics::texture_detail): 1 and 2 lift the title's mip limits
constexpr uint32_t kDetailFull = 1, kDetailMax = 2;
constexpr float kDetailBias = -0.5f;  // full: half a mip sharper than the hardware picks

D3D12_SAMPLER_DESC StageSampler(const State& s, int i, const DrawTextures& stages, bool clamp, uint32_t aniso,
                                uint32_t detail) {
    D3D12_SAMPLER_DESC sd = {};
    const auto& t = s.tss[size_t(i)];
    uint32_t mag = t[kTssMagFilter];
    uint32_t min = t[kTssMinFilter];
    uint32_t mip = t[kTssMipFilter];
    auto lin = [](uint32_t f) { return f >= kTexfLinear; };
    D3D12_FILTER_TYPE fmin = lin(min) ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT;
    D3D12_FILTER_TYPE fmag = lin(mag) ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT;
    D3D12_FILTER_TYPE fmip = mip == kTexfLinear ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT;
    sd.Filter = D3D12_FILTER(fmin << 4 | fmag << 2 | fmip);
    bool anisotropic = min == kTexfAnisotropic || mag == kTexfAnisotropic;
    uint32_t an = anisotropic ? std::max<uint32_t>(t[kTssMaxAnisotropy], 1) : 1;
    // forced only where the title filters across mip levels and the texture has them
    if (detail >= kDetailFull && !aniso) aniso = 16;
    if (aniso && lin(min) && mip && stages.sv[i].Texture2D.MipLevels > 1 && !stages.info[i].shadow) an = aniso;
    if (an > 1) {
        sd.Filter = D3D12_FILTER_ANISOTROPIC;
        sd.MaxAnisotropy = std::min<uint32_t>(an, 16);
    }
    // clamp: the draw's coordinates all lie in 0..1 (UnitTexcoords), so wrap is read as clamp
    sd.AddressU = Address(clamp && t[kTssAddressU] == kTaddressWrap ? kTaddressClamp : t[kTssAddressU]);
    sd.AddressV = Address(clamp && t[kTssAddressV] == kTaddressWrap ? kTaddressClamp : t[kTssAddressV]);
    sd.AddressW = Address(t[kTssAddressW]);
    sd.MipLODBias = Bits(t[kTssMipLodBias]);
    sd.MinLOD = float(t[kTssMaxMipLevel]);
    sd.MaxLOD = mip ? D3D12_FLOAT32_MAX : sd.MinLOD;
    if (detail >= kDetailFull && !stages.info[i].shadow) {
        // the title's top mip limit (D3DTSS_MAXMIPLEVEL) and any blurring bias dropped: the sharpest level is used
        sd.MinLOD = 0;
        sd.MipLODBias = std::min(sd.MipLODBias, 0.0f) + kDetailBias;
        if (!mip) sd.MaxLOD = 0;
        if (detail >= kDetailMax) sd.MaxLOD = 0;
    }
    Color(t[kTssBorderColor], sd.BorderColor);
    if (stages.info[i].shadow) {
        // the console tests texel OP r, D3D12 tests r OP texel; with both reversed (g_reversed_depth) it is the same OP
        sd.Filter = lin(min) || lin(mag) ? D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT
                                         : D3D12_FILTER_COMPARISON_MIN_MAG_MIP_POINT;
        sd.ComparisonFunc = Cmp(s.rs[kRsShadowFunc]);
        sd.MaxAnisotropy = 0;
    }
    return sd;
}

uint32_t D3D12Backend::EnsureStageSamplers(const State& s, const DrawTextures& stages, bool clamp) {
    D3D12_SAMPLER_DESC sd[4];
    uint32_t aniso = app::Get().aniso, detail = app::Get().graphics.texture_detail;
    for (int i = 0; i < 4; i++) sd[i] = StageSampler(s, i, stages, clamp, aniso, detail);
    uint64_t key = Hash(sd, sizeof(sd));
    auto it = samplers_.find(key);
    if (it != samplers_.end()) return it->second;
    if (smp_used_ + 4 > kSamplerCount) {
        samplers_.clear();
        smp_used_ = 0;
    }
    uint32_t base = smp_used_;
    smp_used_ += 4;
    for (int i = 0; i < 4; i++) dev_->CreateSampler(&sd[i], Cpu(smp_heap_.Get(), base + i, smp_inc_));
    samplers_[key] = base;
    return base;
}

uint32_t SimpleState(const State& s, uint32_t method) {
    return s.simple[method >> 2];
}

constexpr uint32_t kQuadVertices = 6;  // a quad drawn as two triangles

DrawShaders ClassifyShaders(const State& s, const Assembled& a, uint32_t count) {
    DrawShaders sh;
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

uint64_t VertexKey(const State& s, const Assembled& a, const DrawShaders& sh) {
    uint64_t seed = sh.prog ? s.vs->hash ^ a.regs : Hash(&s.vs_handle, 4, a.regs | kDeclarationSalt);
    uint64_t vkey = Hash(&a.decode, 8, seed);
    if (!sh.prog && a.pretransformed) vkey ^= kPretransformedSalt;
    if (sh.quad) vkey ^= kQuadSalt;
    if (sh.sprite) vkey ^= kSpriteSalt;
    if (!sh.prog) vkey = Hash(&sh.fk, sizeof(sh.fk), vkey);
    return vkey;
}

// the enhanced car paint variant, apart from the program's plain shader
constexpr uint64_t kCarPaintSalt = 0xCA9A1A7ull << 32;

// enhanced car paint applies to the programs that light cars the console's way (gpu/car_paint.h); known per program
bool CarLightingProgram(const State& s, const Assembled& a) {
    if (s.vs->car_lighting < 0)
        s.vs->car_lighting = car_paint::LightingProgram(VertexProgramHlsl(s.vs->program, a.regs, a.decode)) ? 1 : 0;
    return s.vs->car_lighting > 0;
}

// a point sprite's program hands its point size to SpriteHlsl in tc3, whose coordinates the sprite makes
std::string SpriteSizeHlsl(std::string s) {
    const char* tc3 = "    o.tc3 = oT3;\n";
    size_t at = s.find(tc3);
    if (at == std::string::npos) {
        LOGE("d3d12: a vertex shader no longer fits SpriteSizeHlsl");
        return s;
    }
    s.replace(at, std::strlen(tc3), "    o.tc3 = oPts;\n");
    return s;
}

std::string VertexHlsl(const State& s, const Assembled& a, const DrawShaders& sh) {
    if (sh.prog && sh.car_paint) return car_paint::VertexHlsl(VertexProgramHlsl(s.vs->program, a.regs, a.decode));
    if (sh.prog) {
        std::string src = VertexProgramHlsl(s.vs->program, a.regs, a.decode);
        return sh.sprite ? SpriteSizeHlsl(std::move(src)) : src;
    }
    return FixedVertexHlsl(a.regs, a.pretransformed, a.decode, sh.fk);
}

// sets sh.vh and sh.vs_code, compiling the shader on a cache miss
void D3D12Backend::EnsureVertexShader(const State& s, const Assembled& a, DrawShaders& sh) {
    auto vit = shader_src_.find(sh.vkey);
    if (vit != shader_src_.end()) {
        sh.vs_code = blobs_[vit->second].Get();
        sh.vh = vit->second;
        return;
    }
    std::string src = ReversedDepthHlsl(VertexHlsl(s, a, sh));
    sh.vs_code = Compile(sh.quad ? QuadHlsl(std::move(src)) : src, "main", "vs_5_0", sh.vh);
    shader_src_[sh.vkey] = sh.vh;
}

bool D3D12Backend::BindStageTexturesAndTargets(const State& s, DrawTextures& stages) {
    draw_rt_ = bound_rt_;
    draw_ds_ = bound_ds_;
    {
        PerfScope perf_bind{g_perf.bind};
        for (int i = 0; i < 4; i++) stages.res[i] = BindTexture(s, i, stages);
    }
    // textures may have recorded copies that unbound the targets or moved them out of their target states
    if (bound_rt_ != draw_rt_ || bound_ds_ != draw_ds_) return BindTargets(s);
    if (bound_rt_) Transition(bound_rt_->res.Get(), bound_rt_->state, D3D12_RESOURCE_STATE_RENDER_TARGET);
    if (bound_ds_) Transition(bound_ds_->res.Get(), bound_ds_->state, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    return true;
}

constexpr uint64_t kPixelKeySalt = 0x9E3779B97F4A7C15ull;

// sets sh.ph and sh.ps_code, compiling the shader on a cache miss
uint32_t CarPaintTargetMask(const PixelKey& pk) {
    uint32_t m = 0;
    for (int st = 0; st < 3; st++)
        if (pk.tex[st].kind == kTex2D && (pk.tex[st].flags & kTexTarget)) m |= 1u << st;
    return m;
}

void D3D12Backend::EnsurePixelShader(const PixelKey& pk, DrawShaders& sh) {
    // pixel shaders share the map with vertex shaders, the salt keeps their keys apart
    auto pit = shader_src_.find(sh.pkey ^ kPixelKeySalt);
    if (pit != shader_src_.end()) {
        sh.ps_code = blobs_[pit->second].Get();
        sh.ph = pit->second;
        return;
    }
    sh.ps_code = Compile(pk.car_paint ? car_paint::PixelHlsl(PixelHlsl(pk), CarPaintTargetMask(pk)) : PixelHlsl(pk),
                         "main", "ps_5_0", sh.ph);
    shader_src_[sh.pkey ^ kPixelKeySalt] = sh.ph;
}

void KeyBlend(const State& s, PsoKey& k) {
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

constexpr uint32_t kZUseW = 2;  // D3DZB_USEW

bool OffsetsPolygons(const State& s, Topology topology) {
    return topology == Topology::Triangles && SimpleState(s, kMSolidOffsetEnable);
}

// polygon offset slopes are per console pixel, an upscaled target's pixels are smaller
float HostSlopeScale(const State& s, const Target* ds) {
    return Bits(SimpleState(s, kMSlopeScale)) * (ds ? float(ds->w) / ds->gw : 1.0f);
}

void D3D12Backend::KeyDepthStencil(const State& s, const Assembled& a, PsoKey& k) {
    k.zen = s.rs[kRsZEnable] != 0;
    if (k.zen) {
        k.zwrite = SimpleState(s, kMZWriteEnable) != 0;
        k.zfunc = SimpleState(s, kMZFunc);
    }
    k.sten = s.rs[kRsStencilEnable] != 0;
    if (k.sten) {
        k.sfunc = SimpleState(s, kMStencilFunc);
        k.smask = SimpleState(s, kMStencilMask) & 0xFF;
        k.swmask = SimpleState(s, kMStencilWriteMask) & 0xFF;
        k.sfail = s.rs[kRsStencilFail];
        k.szfail = SimpleState(s, kMStencilZFail);
        k.spass = SimpleState(s, kMStencilPass);
    }
    // polygon offset: the slope here, toward the camera is up on reversed depth; the units are in ReversedDepthHlsl
    if (OffsetsPolygons(s, a.topology)) k.zslope = -HostSlopeScale(s, bound_ds_);
}

void KeyRasterizer(const State& s, const Assembled& a, PsoKey& k) {
    uint32_t cull = s.rs[kRsCullMode];
    k.cull = cull != 0 && a.topology == Topology::Triangles;
    k.ccw = cull == kCullCw;  // culling clockwise leaves counter clockwise as the front
    k.fill = s.rs[kRsFillMode];
}

void D3D12Backend::BuildPipelineKey(const State& s, const Assembled& a, uint64_t vh, uint64_t ph, PsoKey& k) {
    std::memset(&k, 0, sizeof(k));
    k.vs = vh;
    k.ps = ph;
    k.regs = a.regs;
    for (int r = 0; r < 16; r++) {
        if ((a.regs >> r) & 1) {
            const VertexElement& e = a.elements[size_t(r)];
            k.elements[r] = e.format | uint32_t(e.buffer) << 8 | uint32_t(e.offset) << 16;
        }
    }
    k.topo = uint32_t(a.topology);
    k.has_rt = bound_rt_ != nullptr;
    k.has_ds = bound_ds_ != nullptr;
    k.rtfmt = bound_rt_ ? bound_rt_->fmt : 0;
    KeyBlend(s, k);
    if (k.has_ds) KeyDepthStencil(s, a, k);
    KeyRasterizer(s, a, k);
}

// alpha test off: compare always, in Cmp() minus 1 terms like the enabled test
constexpr float kAlphaTestAlways = float(D3D12_COMPARISON_FUNC_ALWAYS - 1);

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
    c.misc[1] = SimpleState(s, kMAlphaTestEnable) ? float(Cmp(SimpleState(s, kMAlphaFunc)) - 1) : kAlphaTestAlways;
    c.misc[2] = float(Cmp(s.rs[kRsShadowFunc]) - 1);
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

constexpr float kGlowBoxTexels = 3.0f;  // console texels across the box each wide tap averages

void D3D12Backend::FillTexelConstants(int i, const TextureDesc& d, const DrawTextures& stages, DrawConstants& c) {
    c.texsize[i][0] = float(d.width);
    c.texsize[i][1] = float(d.height);
    if (!stages.rt[i]) return;
    float k = stages.k[i];
    // a 3 console texel box per tap keeps the console chain's glow width and brightness
    if (stages.wide[i] > 0) {
        float n = std::ceil(kGlowBoxTexels * stages.wide[i] * 0.5f);
        c.spread[i][0] = kGlowBoxTexels / (n * float(d.width) * k);
        c.spread[i][1] = kGlowBoxTexels / (n * float(d.height) * k);
        c.spread[i][2] = n;
    }
}

void D3D12Backend::FillTextureConstants(const State& s, const DrawShaders& sh, const DrawTextures& stages,
                                        DrawConstants& c) {
    for (int i = 0; i < 4; i++) {
        TextureDesc d;
        if (s.textures[size_t(i)] && Describe(s.textures[size_t(i)], d)) FillTexelConstants(i, d, stages, c);
        if (stages.rt[i]) c.rtshift[i] = RenderTargetShift(s, sh);
        // Existing unused spread.w carries the Y/X shift ratio; the shared constant-buffer layout stays exact.
        const Target* target = bound_rt_ ? bound_rt_ : bound_ds_;
        c.spread[i][3] = target && target->gw && target->gh && target->w
                             ? (float(target->h) / target->gh) / (float(target->w) / target->gw) : 1.0f;
        const auto& t = s.tss[size_t(i)];
        c.bump[i][0] = Bits(t[kTssBumpEnvMat00]);
        c.bump[i][1] = Bits(t[kTssBumpEnvMat01]);
        c.bump[i][2] = Bits(t[kTssBumpEnvMat10]);
        c.bump[i][3] = Bits(t[kTssBumpEnvMat11]);
        c.bumpl[i][0] = Bits(t[kTssBumpEnvLScale]);
        c.bumpl[i][1] = Bits(t[kTssBumpEnvLOffset]);
    }
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

void InverseTranspose3x3(const float* wv, float* n) {
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

// D3DTRANSFORMSTATETYPE indices into s.transforms
constexpr size_t kTsView = 2;
constexpr size_t kTsProjection = 3;
constexpr size_t kTsTexture0 = 16;
constexpr size_t kTsWorld = 256;

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

void LightToView(const std::array<uint32_t, kLightFloats>& l, const float* vm, float (&o)[7][4]) {
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

// a w buffer's clip range in the units of a draw's clip w
struct WRange {
    float near_w;
    float far_w;
};

// the projection's near and far clip w as D3DDevice_SetTransform finds them
WRange ProjectionWRange(const float* m) {
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

// ReversedDepthHlsl's zparams: x the w buffer's scale (0 for a z buffer), y its far w, z the polygon offset in units of
// the clip w for a w buffer, of depth for a z buffer
void D3D12Backend::FillDepthConstants(const State& s, const DrawShaders& sh, const PsoKey& k, DrawConstants& c) const {
    bool offset = OffsetsPolygons(s, Topology(k.topo));
    float units = offset ? Bits(SimpleState(s, kMDepthOffset)) : 0;
    if (s.rs[kRsZEnable] != kZUseW) {
        c.zparams[2] = units / s.zscale;
        return;
    }
    WRange r = DrawWRange(s, sh.prog);
    c.zparams[0] = r.near_w / (r.far_w - r.near_w);
    c.zparams[1] = r.far_w;
    c.zparams[2] = sh.prog ? units : units * r.far_w / s.zscale;
}

// a fresh copy of the draw's vertex constants, kept as the last upload
uint8_t* D3D12Backend::UploadVertexConstants(const State& s) {
    Alloc vc = Upload(sizeof(s.vsc), 256);
    std::memcpy(vc.cpu, s.vsc.data(), sizeof(s.vsc));
    std::memcpy(last_vsc_.data(), s.vsc.data(), sizeof(s.vsc));
    last_vsc_gpu_ = vc.gpu;
    return vc.cpu;
}

void D3D12Backend::UploadConstants(const State& s, const DrawShaders& sh, const PsoKey& k, const DrawTextures& stages,
                                   GuestExtent size, DrawConstants& c, draw_evidence::Recorder* evidence) {
    // the extension edits the uploaded copy, so its draws always get their own
    bool same = !ext_ && last_vsc_gpu_ && !std::memcmp(last_vsc_.data(), s.vsc.data(), sizeof(s.vsc));
    uint8_t* vsc = same ? nullptr : UploadVertexConstants(s);
    FillSurfaceConstants(s, size, c);
    // points: host pixels per console pixel, also keeps them off the far edge pull out of screen passes
    if (k.topo == uint32_t(Topology::Points)) c.surf[3] = TargetScale();
    FillFogConstants(s, sh.prog, c);
    FillCombinerConstants(s, c);
    if (ext_) ext_->Constants(reinterpret_cast<float(*)[4]>(vsc), c.ext, bound_rt_, k.blend ? k.src : 1);
    FillDepthConstants(s, sh, k, c);
    FillTextureConstants(s, sh, stages, c);
    if (sh.fixed_transform) {
        FillTransformConstants(s, size, c);
        FillLightConstants(s, sh.fk, c);
    }
    if (!last_dc_gpu_ || std::memcmp(&last_dc_, &c, sizeof(c))) {
        Alloc dc = Upload(sizeof(DrawConstants), 256);
        std::memcpy(dc.cpu, &c, sizeof(c));
        last_dc_ = c;
        last_dc_gpu_ = dc.gpu;
    }
    // the bytes this draw reads, after extension edits and fills; upload-ring pointers are not kept
    if (evidence) {
        evidence->VertexConstants(draw_evidence::Bytes(vsc ? static_cast<const void*>(vsc) : last_vsc_.data(), sizeof(s.vsc)));
        evidence->DrawConstantsUpload(draw_evidence::Bytes(&last_dc_, sizeof(c)));
    }
    if (last_vsc_gpu_ != binds_.vsc) cl_->SetGraphicsRootConstantBufferView(0, last_vsc_gpu_);
    if (last_dc_gpu_ != binds_.dc) cl_->SetGraphicsRootConstantBufferView(1, last_dc_gpu_);
    binds_.vsc = last_vsc_gpu_;
    binds_.dc = last_dc_gpu_;
}

void D3D12Backend::CaptureTextures(const State& s, const DrawTextures& stages) {
    for (int i = 0; i < 4; i++) {
        ID3D12Resource* res = stages.res[i];
        TextureDesc d;
        if (!s.textures[size_t(i)] || !Describe(s.textures[size_t(i)], d)) continue;
        bool tg = targets_.count(SizedTargetKey(d.data, Format(d.format).depth, d.width, d.height));
        const auto& t = s.tss[size_t(i)];
        // address u v, filters min mag mip, and the stage's colour op (fixed function); then the texture pack
        // content hash (0 when not packable) and coordinate index, the identity a PBR material assignment names
        fprintf(cap_, " | t%d %08X fmt %X %ux%u%s%s addr %u%u filt %u%u%u cop %u%s hash %016llX tci %u", i, d.data,
                d.format, d.width, d.height, tg ? " target" : "", res ? "" : " unbound", t[kTssAddressU],
                t[kTssAddressV], t[kTssMinFilter], t[kTssMagFilter], t[kTssMipFilter], t[kTssColorOp],
                stages.role[i] == kRoleNeon ? " neon" : "", (unsigned long long)stages.photo_identity[i].original_hash,
                t[kTssTexCoordIndex]);
        if (res && !tg && stages.info[i].kind == kTex2D && cap_seen_.insert(d.data).second) {
            cap_tex_.push_back({res, stages.sv[i].Format, d.data, d.width, d.height});
        }
    }
}

// s.vsc holds the 192 vertex shader constants in hardware order: D3D register + 96
constexpr int kVsConstants = 192;
constexpr int kVsConstantBias = 96;

// floats a raw vertex element holds, 0 for one the capture prints as a hex dword (colours, packed formats)
uint32_t ElementFloats(uint8_t format) {
    switch (DXGI_FORMAT(format)) {
        case DXGI_FORMAT_R32_FLOAT: return 1;
        case DXGI_FORMAT_R32G32_FLOAT: return 2;
        case DXGI_FORMAT_R32G32B32_FLOAT: return 3;
        case DXGI_FORMAT_R32G32B32A32_FLOAT: return 4;
        default: return 0;
    }
}

// a raw path draw's first vertices as the title wrote them (the converted path has them in a.vertices), and for a
// fixed function draw where each lands: the frame's pixel x y after world, view, projection and viewport, the same
// matrix the vertex shader multiplies by (HUD placement: docs/notes/hud_minimap_popup.md)
void D3D12Backend::CaptureRawVertices(const State& s, const Assembled& a, const DrawConstants& c, bool fixed) {
    const uint32_t nv = std::min<uint32_t>(4, a.vertex_count);
    for (uint32_t v = 0; v < nv; v++) {
        fprintf(cap_, "\n     v%u", v);
        for (int r = 0; r < 16; r++) {
            if (!((a.regs >> r) & 1)) continue;
            const VertexElement& e = a.elements[size_t(r)];
            const VertexBuffer& vb = a.buffers[e.buffer];
            const uint32_t nf = ElementFloats(e.format);
            const size_t at = size_t(v) * vb.stride + e.offset;
            if (!vb.src || !vb.stride || at + std::max<uint32_t>(nf, 1) * 4 > vb.bytes) continue;
            fprintf(cap_, " r%d(", r);
            if (nf) {
                float f[4];
                std::memcpy(f, vb.src + at, nf * 4);
                for (uint32_t k = 0; k < nf; k++) fprintf(cap_, " %g", f[k]);
            } else {
                uint32_t x;
                std::memcpy(&x, vb.src + at, 4);
                fprintf(cap_, "%08X", x);
            }
            fputc(')', cap_);
            if (r != 0 || !fixed || nf < 3) continue;
            float p[4] = {0, 0, 0, 1};
            std::memcpy(p, vb.src + at, 12);
            float q[4];
            for (int j = 0; j < 4; j++) q[j] = p[0] * c.wvp[j * 4] + p[1] * c.wvp[j * 4 + 1] + p[2] * c.wvp[j * 4 + 2] + c.wvp[j * 4 + 3];
            if (q[3] != 0) fprintf(cap_, " px(%.1f %.1f)", (q[0] / q[3] + 1) * 0.5f * c.surf[0], (1 - q[1] / q[3]) * 0.5f * c.surf[1]);
        }
    }
    if (!fixed) return;
    const auto& vp = s.viewport;
    fprintf(cap_, "\n     viewport %g %g %g %g", float(vp.x), float(vp.y), float(vp.w), float(vp.h));
    struct Named {
        const char* name;
        size_t index;
    };
    const Named mats[] = {{"world", kTsWorld}, {"view", kTsView}, {"proj", kTsProjection}};
    for (const Named& t : mats) {
        const float* m = s.transforms[t.index].data();
        fprintf(cap_, "\n     %s", t.name);
        for (int row = 0; row < 4; row++)
            fprintf(cap_, " | %g %g %g %g", m[row * 4], m[row * 4 + 1], m[row * 4 + 2], m[row * 4 + 3]);
    }
    // fixed function lighting: what the generated shader lights with (material sources are in its HLSL)
    if (!s.rs[kRsLighting]) return;
    auto rgba = [&](const char* name, const float* v) { fprintf(cap_, " %s(%.3f %.3f %.3f %.3f)", name, v[0], v[1], v[2], v[3]); };
    fprintf(cap_, "\n     lighting colorvertex %u sources d%u s%u a%u e%u", s.rs[kRsColorVertex],
            s.rs[kRsDiffuseMaterialSource], s.rs[kRsSpecularMaterialSource], s.rs[kRsAmbientMaterialSource],
            s.rs[kRsEmissiveMaterialSource]);
    rgba("mat_d", c.mat[0]); rgba("mat_a", c.mat[1]); rgba("mat_s", c.mat[2]); rgba("mat_e", c.mat[3]);
    rgba("ambient", c.amb); rgba("normal", s.vattr[2].data());
    for (int l = 0; l < 8; l++)
        if ((s.light_enable >> l) & 1) {
            fprintf(cap_, "\n     light%d type %u", l, s.lights[size_t(l)][0]);
            rgba("d", c.lt[l][0]); rgba("a", c.lt[l][2]); rgba("dir", c.lt[l][4]);
        }
}

void D3D12Backend::CaptureInputs(const State& s, const Assembled& a, const DrawConstants& c, bool fixed) {
    fprintf(cap_, "\n     verts");
    uint32_t n = 0;
    for (int r = 0; r < 16; r++) n += (a.regs >> r) & 1;
    for (size_t v = 0; v < std::min<size_t>(4, n ? a.vertices.size() / (n * 4) : 0); v++) {
        fprintf(cap_, " [");
        for (uint32_t k = 0; k < n * 4; k++) fprintf(cap_, " %g", a.vertices[v * n * 4 + k]);
        fprintf(cap_, " ]");
    }
    if (a.vertices.empty()) CaptureRawVertices(s, a, c, fixed);
    for (int r = 0; r < kVsConstants; r++) {
        const auto& v = s.vsc[r];
        if (v[0] || v[1] || v[2] || v[3]) {
            fprintf(cap_, "\n     c%d %g %g %g %g", r - kVsConstantBias, v[0], v[1], v[2], v[3]);
        }
    }
    fprintf(cap_, "\n     pc");
    for (size_t r = 0; r < std::size(c.pc); r++) {
        fprintf(cap_, " (%.3f %.3f %.3f %.3f)", c.pc[r][0], c.pc[r][1], c.pc[r][2], c.pc[r][3]);
    }
}

// the draw's line: its state, then its textures
void D3D12Backend::CaptureDraw(const State& s, uint32_t prim, uint32_t count, const PsoKey& k, const DrawConstants& c,
                               const DrawTextures& stages) {
    fprintf(cap_,
            "%04u draw rt %08X ds %08X prim %u count %u vs %08X ps %08X blend %u %X %X zen %u %X zwrite %u "
            "zbias %d %.2f cull %u cw %X c0 %.3f %.3f %.3f %.3f op %X sten %u %X %X %X",
            cap_i_++, s.rt.data, bound_ds_ ? s.zs.data : 0, prim, count, s.vs_handle, s.ps ? s.ps->handle : 0, k.blend,
            k.src, k.dst, k.zen, k.zfunc, k.zwrite, k.zbias, k.zslope, k.cull, k.wmask, c.pc[0][0], c.pc[0][1],
            c.pc[0][2], c.pc[0][3], k.op, k.sten, k.sfunc, k.smask, k.swmask);
    CaptureTextures(s, stages);
}

// small draws: their inputs and constants; then ends the draw's line
void D3D12Backend::EndDrawCapture(const State& s, const Assembled& a, uint32_t count, const DrawConstants& c,
                                  bool fixed) {
    if (a.tire_fit_applied)
        fprintf(cap_, "\n     tire bead fit %g -> %g vertices %u", s.tire_fit.source_ratio,
                s.tire_fit.target_ratio, a.vertex_count);
    if (s.tire_fit.vertex_data[0]) {
        fprintf(cap_, "\n     tire scope %g -> %g applied %u ids", s.tire_fit.source_ratio,
                s.tire_fit.target_ratio, unsigned(a.tire_fit_applied));
        for (auto id : s.tire_fit.vertex_data) fprintf(cap_, " %08X", id);
        fprintf(cap_, " streams");
        for (size_t n = 0; n < s.streams.size(); ++n)
            if (s.streams[n].vb) fprintf(cap_, " %zu:%08X:%u", n, kernel::R32(s.streams[n].vb + 4), s.streams[n].stride);
    }
    const bool skin = !fixed && s.vs && IsIndexedSkinProgram(s.vs->program);
    if (skin) {
        fprintf(cap_, "\n     skin scale %g bone_rows", s.vsc[105][3]);
        const auto rows = SkinBoneRows(a, s.vsc[105][3]);
        for (size_t r = 0; r < rows.size(); ++r) if (rows[r]) fprintf(cap_, " %zu", r);
        fprintf(cap_, "\n     skin vdata");
        for (size_t n = 0; n < s.streams.size(); ++n)
            if (s.streams[n].vb) fprintf(cap_, " %zu:%08X", n, kernel::R32(s.streams[n].vb + 4));
        fprintf(cap_, "\n     skin layout");
        for (size_t r = 0; r < s.vs->inputs.size(); ++r) {
            const auto& e = s.vs->inputs[r];
            if (e.type) fprintf(cap_, " %zu:%02X:%d:%u", r, e.type, e.stream, e.offset);
        }
    }
    if (skin || count <= kQuadVertices) {
        CaptureInputs(s, a, c, fixed);
    } else {
        // larger draws: what changed since the last captured draw, enough to tell repeated draws apart
        for (int r = 0; r < kVsConstants; r++) {
            const auto& v = s.vsc[r];
            if (std::memcmp(v.data(), cap_vsc_[r].data(), sizeof(float) * 4) != 0)
                fprintf(cap_, "\n     c%d %g %g %g %g", r - kVsConstantBias, v[0], v[1], v[2], v[3]);
        }
        for (uint32_t b = 0; b < a.buffer_count && b < 2; b++) {
            const VertexBuffer& vb = a.buffers[b];
            if (!vb.src || !vb.stride) continue;
            fprintf(cap_, "\n     vb%u stride %u v0", b, vb.stride);
            for (uint32_t w = 0; w + 4 <= vb.stride && w < 64; w += 4) {
                uint32_t x;
                std::memcpy(&x, vb.src + w, 4);
                fprintf(cap_, " %08X", x);
            }
        }
    }
    cap_vsc_ = s.vsc;
    fputc('\n', cap_);
}

// each shader once, as translated
void D3D12Backend::CaptureShader(const char* kind, uint64_t key, const std::string& src) {
    if (!cap_sh_.insert(key).second) return;
    char name[48];
    snprintf(name, sizeof(name), "%s_%016llX.hlsl", kind, (unsigned long long)key);
    if (FILE* f = _wfopen((cap_dir_ / name).c_str(), L"w")) {
        fwrite(src.data(), 1, src.size(), f);
        fclose(f);
    }
    fprintf(cap_, "     %s\n", name);
}

void D3D12Backend::CaptureShaders(const State& s, const Assembled& a, const DrawShaders& sh, const PixelKey& pk) {
    std::string src = ReversedDepthHlsl(VertexHlsl(s, a, sh));
    CaptureShader("vs", sh.vkey, sh.quad ? QuadHlsl(std::move(src)) : src);
    CaptureShader("ps", sh.pkey, pk.car_paint ? car_paint::PixelHlsl(PixelHlsl(pk), CarPaintTargetMask(pk)) : PixelHlsl(pk));
    // every draw's own keys, since the source files above are written once per key
    fprintf(cap_, "     keys vs %016llX ps %016llX\n", (unsigned long long)sh.vkey, (unsigned long long)sh.pkey);
}

D3D12_SRV_DIMENSION NullViewDimension(uint8_t kind) {
    if (kind == kTexCube) return D3D12_SRV_DIMENSION_TEXTURECUBE;
    if (kind == kTex3D) return D3D12_SRV_DIMENSION_TEXTURE3D;
    return D3D12_SRV_DIMENSION_TEXTURE2D;
}

// A small draw whose every float2 input lies in 0..1: sprites, HUD pieces, text, full screen passes. The console's
// pixel centres sample such a quad from its first texel to half a texel short of its far edge; upscaled, the far
// edge's last half texel is drawn too, and wrapped it blends in the opposite edge (thin dark lines on HUD sprites).
// Clamped, it takes the edge texel instead, which a wrapped texture used once over 0..1 has there anyway.
bool UnitTexcoords(const Assembled& a) {
    constexpr float kLo = -1e-4f;
    constexpr float kHi = 1.0001f;
    constexpr uint32_t kSmallDrawVertices = 4096;  // most vertices a small draw has
    if (!a.vertices.empty() || !a.vertex_count || a.vertex_count > kSmallDrawVertices) return false;
    bool any = false;
    for (int r = 0; r < 16; r++) {
        const VertexElement& e = a.elements[size_t(r)];
        if (!(a.regs >> r & 1) || e.format != DXGI_FORMAT_R32G32_FLOAT) continue;
        const VertexBuffer& vb = a.buffers[e.buffer];
        if (!vb.src || !vb.stride || uint64_t(a.vertex_count) * vb.stride > vb.bytes) return false;
        for (uint32_t v = 0; v < a.vertex_count; v++) {
            float uv[2];
            std::memcpy(uv, vb.src + size_t(v) * vb.stride + e.offset, sizeof(uv));
            if (!(uv[0] >= kLo && uv[0] <= kHi && uv[1] >= kLo && uv[1] <= kHi)) return false;
        }
        any = true;
    }
    return any;
}

// UnitTexcoords only changes stages that wrap
bool AnyStageWraps(const State& s) {
    for (const auto& t : s.tss) {
        if (t[kTssAddressU] == kTaddressWrap || t[kTssAddressV] == kTaddressWrap) return true;
    }
    return false;
}

// the first slot of the stages' four views, made only when this frame has not made the same set yet
uint32_t D3D12Backend::EnsureViewSet(const DrawTextures& stages) {
    if (view_sets_frame_ != frame_) {
        view_sets_.clear();
        view_sets_frame_ = frame_;
    }
    uint64_t key = Hash(stages.sv, sizeof(stages.sv), Hash(stages.res, sizeof(stages.res)));
    auto it = view_sets_.find(key);
    if (it != view_sets_.end()) return it->second;
    uint32_t slot = AllocSrvSlots(4);
    for (int i = 0; i < 4; i++) {
        dev_->CreateShaderResourceView(stages.res[i], &stages.sv[i], Cpu(srv_heap_.Get(), slot + i, srv_inc_));
    }
    view_sets_[key] = slot;
    return slot;
}

// a stage without a texture gets a null view of its kind; the samplers read the views after
void D3D12Backend::SetTextureViewsAndSamplers(const State& s, const Assembled& a, DrawTextures& stages) {
    for (int i = 0; i < 4; i++) {
        if (stages.res[i]) continue;
        stages.sv[i].ViewDimension = NullViewDimension(stages.info[i].kind);
        stages.sv[i].Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    }
    uint32_t views = EnsureViewSet(stages);
    if (views != binds_.views) cl_->SetGraphicsRootDescriptorTable(2, Gpu(srv_heap_.Get(), views, srv_inc_));
    binds_.views = views;
    uint32_t samplers = EnsureStageSamplers(s, stages, AnyStageWraps(s) && UnitTexcoords(a));
    if (samplers != binds_.samplers) cl_->SetGraphicsRootDescriptorTable(3, Gpu(smp_heap_.Get(), samplers, smp_inc_));
    binds_.samplers = samplers;
    if (stages.pbr_nrm && stages.pbr_orm) {
        uint32_t pbr_slot = AllocSrvSlots(2);
        D3D12_SHADER_RESOURCE_VIEW_DESC psv{};
        psv.Format = kBgra;
        psv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        psv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        psv.Texture2D.MipLevels = stages.pbr_levels;
        dev_->CreateShaderResourceView(stages.pbr_nrm, &psv, Cpu(srv_heap_.Get(), pbr_slot, srv_inc_));
        dev_->CreateShaderResourceView(stages.pbr_orm, &psv, Cpu(srv_heap_.Get(), pbr_slot + 1, srv_inc_));
        cl_->SetGraphicsRootDescriptorTable(4, Gpu(srv_heap_.Get(), pbr_slot, srv_inc_));
    }
}

// viewport covers the target, the guest viewport is in the shader; scissor clips to it
void D3D12Backend::SetViewportScissor(const State& s, uint32_t gw) {
    float W = float(bound_rt_ ? bound_rt_->w : bound_ds_->w);
    float H = float(bound_rt_ ? bound_rt_->h : bound_ds_->h);
    D3D12_VIEWPORT vp{0, 0, W, H, 0, 1};
    const Target* target = bound_rt_ ? bound_rt_ : bound_ds_;
    float sx = W / gw, sy = H / target->gh;
    LONG x0 = LONG(s.viewport.x * sx);
    LONG y0 = LONG(s.viewport.y * sy);
    LONG x1 = LONG((s.viewport.x + s.viewport.w) * sx);
    LONG y1 = LONG((s.viewport.y + s.viewport.h) * sy);
    if (s.scissor_count) {
        x0 = std::max(x0, LONG(s.scissors[0] * sx));
        y0 = std::max(y0, LONG(s.scissors[1] * sy));
        x1 = std::min(x1, LONG(s.scissors[2] * sx));
        y1 = std::min(y1, LONG(s.scissors[3] * sy));
    }
    D3D12_RECT rect{std::clamp<LONG>(x0, 0, LONG(W)), std::clamp<LONG>(y0, 0, LONG(H)),
                    std::clamp<LONG>(x1, 0, LONG(W)), std::clamp<LONG>(y1, 0, LONG(H))};
    bool same = binds_.rasterizer && !std::memcmp(&vp, &binds_.viewport, sizeof(vp)) &&
                !std::memcmp(&rect, &binds_.scissor, sizeof(rect));
    if (same) return;
    cl_->RSSetViewports(1, &vp);
    cl_->RSSetScissorRects(1, &rect);
    binds_.rasterizer = true;
    binds_.viewport = vp;
    binds_.scissor = rect;
}

void D3D12Backend::SetBlendFactorStencilRef(const State& s) {
    float bf[4];
    Color(SimpleState(s, kMBlendColor), bf);
    uint32_t ref = SimpleState(s, kMStencilRef) & 0xFF;
    if (binds_.output && !std::memcmp(bf, binds_.blend, sizeof(bf)) && ref == binds_.stencil) return;
    cl_->OMSetBlendFactor(bf);
    cl_->OMSetStencilRef(ref);
    binds_.output = true;
    std::memcpy(binds_.blend, bf, sizeof(bf));
    binds_.stencil = ref;
}

D3D_PRIMITIVE_TOPOLOGY PrimitiveTopology(const Assembled& a) {
    if (a.topology == Topology::Points) return D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
    if (a.topology == Topology::Lines) {
        return a.strip ? D3D_PRIMITIVE_TOPOLOGY_LINESTRIP : D3D_PRIMITIVE_TOPOLOGY_LINELIST;
    }
    return a.strip ? D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP : D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
}

void D3D12Backend::UploadVertexBuffers(const Assembled& a, draw_evidence::Recorder* evidence) {
    D3D12_VERTEX_BUFFER_VIEW vbv[16];
    for (uint32_t i = 0; i < a.buffer_count; i++) {
        const VertexBuffer& b = a.buffers[i];
        Alloc vb = Upload(b.bytes, 16);
        std::memcpy(vb.cpu, b.src, b.bytes);
        if (evidence) evidence->VertexStream(i, draw_evidence::Bytes(vb.cpu, b.bytes));
        vbv[i] = {vb.gpu, b.bytes, b.stride};
    }
    cl_->IASetVertexBuffers(0, a.buffer_count, vbv);
}

void D3D12Backend::DrawGeometry(const Assembled& a, draw_evidence::Recorder* evidence) {
    UploadVertexBuffers(a, evidence);
    D3D_PRIMITIVE_TOPOLOGY topology = PrimitiveTopology(a);
    if (topology != binds_.topology) cl_->IASetPrimitiveTopology(topology);
    binds_.topology = topology;
    if (a.index_count) {
        uint32_t ibytes = a.index_count * (a.index16 ? 2 : 4);
        Alloc ib = Upload(ibytes, 4);
        std::memcpy(ib.cpu, a.index_src, ibytes);
        if (evidence) evidence->IndexUpload(draw_evidence::Bytes(ib.cpu, ibytes));
        D3D12_INDEX_BUFFER_VIEW ibv{ib.gpu, ibytes, a.index16 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT};
        cl_->IASetIndexBuffer(&ibv);
        cl_->DrawIndexedInstanced(a.index_count, 1, 0, a.base_vertex, 0);
    } else {
        cl_->DrawInstanced(a.draw_count, 1, 0, 0);
    }
    // This observes successful CPU command-list recording, never GPU execution/completion.
    if (evidence) evidence->DrawWasRecorded();
}

// calls the extension's EndDraw on every return from Draw once made
struct DrawEnd {
    Extension* e;
    ~DrawEnd() {
        if (e) e->EndDraw();
    }
};

// Assemble, timed and counted
bool D3D12Backend::AssembleDraw(const State& s, uint32_t prim, uint32_t start, uint32_t count,
                                const uint8_t* indices, Assembled& a) {
    bool assembled;
    {
        PerfScope perf_assemble{g_perf.assemble};
        assembled = Assemble(s, prim, start, count, indices, a);
    }
    g_perf.verts += a.vertex_count;
    g_perf.raw += a.vertices.empty();
    return assembled;
}

bool D3D12Backend::EnsureQueries() {
    if (query_counts_) return true;
    D3D12_QUERY_HEAP_DESC qd{D3D12_QUERY_HEAP_TYPE_OCCLUSION, kQueries, 0};
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_READBACK};
    D3D12_RESOURCE_DESC bd = BufferDesc(uint64_t(kQueries) * sizeof(uint64_t));
    void* p = nullptr;
    bool made = SUCCEEDED(dev_->CreateQueryHeap(&qd, IID_PPV_ARGS(&query_heap_))) &&
                SUCCEEDED(dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST,
                                                        nullptr, IID_PPV_ARGS(&query_rb_))) &&
                SUCCEEDED(query_rb_->Map(0, nullptr, &p));
    if (!made) {
        LOG_FIRST(1, "d3d12: no occlusion queries, visibility tests stay pending");
        return false;
    }
    query_counts_ = static_cast<const uint64_t*>(p);
    return true;
}

// a draw while the title counts visible pixels gets an occlusion query of its own, kNoQuery otherwise
uint32_t D3D12Backend::BeginCountedDraw(const State& s) {
    if (!s.simple[kMCountEnable >> 2] || !EnsureQueries()) return kNoQuery;
    uint32_t query = query_next_++ % kQueries;
    cl_->BeginQuery(query_heap_.Get(), D3D12_QUERY_TYPE_OCCLUSION, query);
    return query;
}

void D3D12Backend::EndCountedDraw(uint32_t query) {
    if (query == kNoQuery) return;
    cl_->EndQuery(query_heap_.Get(), D3D12_QUERY_TYPE_OCCLUSION, query);
    uint64_t at = uint64_t(query) * sizeof(uint64_t);
    cl_->ResolveQueryData(query_heap_.Get(), D3D12_QUERY_TYPE_OCCLUSION, query, 1, query_rb_.Get(), at);
    counted_.push_back({query, SamplesPerPixel()});
}

// the console counts its own pixels: the bound target's scale and multisampling come out
float D3D12Backend::SamplesPerPixel() const {
    const Target* t = bound_rt_ ? bound_rt_ : bound_ds_;
    if (!t || !t->gw || !t->gh) return 1;
    float area = float(t->w) * float(t->h) / (float(t->gw) * float(t->gh));
    return area * float(t->res->GetDesc().SampleDesc.Count);
}

// reports whose draws the GPU has finished, written as the console's GPU writes them: time, count, then 0 for done
void D3D12Backend::DeliverReports() {
    constexpr uint32_t kTime = 0;
    constexpr uint32_t kTimeHigh = 4;
    constexpr uint32_t kCount = 8;
    constexpr uint32_t kStatus = 12;
    uint64_t done = fence_->GetCompletedValue();
    while (!reports_.empty() && reports_.front().fence <= done) {
        const Report& r = reports_.front();
        double pixels = 0;
        for (const Query& q : r.queries) pixels += double(query_counts_[q.index]) / q.samples_per_pixel;
        kernel::W32(r.va + kTime, uint32_t(presents_));
        kernel::W32(r.va + kTimeHigh, 0);
        kernel::W32(r.va + kCount, uint32_t(std::lround(pixels)));
        std::atomic_thread_fence(std::memory_order_release);
        kernel::W32(r.va + kStatus, 0);
        reports_.pop_front();
    }
}

constexpr uint32_t kPointList = 1;   // D3DPT_POINTLIST
constexpr uint32_t kTopDisable = 1;  // the XDK's D3DTOP_DISABLE
constexpr int kSpriteStage = 3;

// a program's point sprites under the fixed function stages: the XDK builds their combiners from stage 3 alone,
// the stage with the sprite's coordinates; a pixel shader reads t3 itself. SpriteStageZero disables stage 3.
bool SpriteOnStage3(const State& s, uint32_t prim) {
    bool prog = (s.vs_handle & 1) && s.vs && !s.vs->program.empty();
    bool stage3 = s.tss[kSpriteStage][kTssColorOp] != kTopDisable;
    return prim == kPointList && prog && !s.ps && s.rs[kRsPointSpriteEnable] && stage3;
}

// stage 3 drawn as stage 0, the only stage
State SpriteStageZero(const State& s) {
    State moved = s;
    moved.tss[0] = s.tss[kSpriteStage];
    moved.textures[0] = s.textures[kSpriteStage];
    moved.palettes[0] = s.palettes[kSpriteStage];
    for (int i = 1; i < 4; i++) {
        moved.tss[i][kTssColorOp] = kTopDisable;
        moved.tss[i][kTssAlphaOp] = kTopDisable;
        moved.textures[i] = 0;
    }
    return moved;
}

void D3D12Backend::Draw(const State& s, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices) {
    if (SpriteOnStage3(s, prim)) {
        Draw(SpriteStageZero(s), prim, start, count, indices);
        return;
    }
    PerfScope perf_draw{g_perf.draw};
    g_perf.draws++;
    if (cap_) CapStep(s.rt);
    if (DepthCopy(s)) return;
    DrawEnd draw_end{ext_};
    if (ext_) ext_->BeginDraw(s);
    Assembled& a = asm_;
    if (!AssembleDraw(s, prim, start, count, indices, a)) return;
    // past the ring every upload is a buffer of its own: seconds lost on a frame that draws a whole level once
    if (CurrentFrame().used > kUploadSize - kUploadReserve) Submit(true);
    if (!BindTargets(s)) return;
    if (!pbr_settings_) UpdatePbrSettings();
    if (ext_ && ext_->Bound(*this, s, bound_rt_, bound_ds_, a.pretransformed) && !BindTargets(s)) return;

    // vertex shader
    DrawShaders sh = ClassifyShaders(s, a, count);
    if (sh.prog) EnsureProgramHash(s);
    sh.vkey = VertexKey(s, a, sh);
    const auto& graphics = app::Get().graphics;
    sh.car_paint = graphics.car_paint && sh.prog && !sh.quad && !a.pretransformed && CarLightingProgram(s, a);
    if (sh.car_paint) sh.vkey ^= kCarPaintSalt;
    EnsureVertexShader(s, a, sh);

    // stage textures, then the pixel shader made for them
    DrawTextures stages;
    if (!BindStageTexturesAndTargets(s, stages)) return;
    PixelKey pk = MakePixelKey(s, stages.info);
    pk.quad = sh.quad;
    pk.car_paint = sh.car_paint;
    // The old filename-companion shader has no approved assignment or physical scene bridge.
    // Only the separate reviewed-material pipeline may consume the PBR preference.
    pk.pbr = 0;
    stages.pbr_nrm = stages.pbr_orm = nullptr;
    sh.pkey = Hash(&pk, sizeof(pk));
    EnsurePixelShader(pk, sh);
    if (!sh.vs_code || !sh.ps_code) return;

    // pipeline
    PsoKey k;
    BuildPipelineKey(s, a, sh.vh, sh.ph, k);
    k.sprite = sh.sprite;
    // ADDSIGNED d + s and REVSUBTRACTSIGNED d - s on colours read as signed bytes (the console's alpha counters, as in
    // the headlight volumes): an add of one sign of s, then a reverse subtract of the other
    const uint32_t guest_op = k.op;
    const bool signed_op = k.blend && (guest_op == kBlendOpAddSigned || guest_op == kBlendOpRevSubtractSigned);
    if (signed_op) k.op = kBlendOpAdd;
    ID3D12PipelineState* pso = EnsurePipeline(k, sh.vs_code, sh.ps_code);
    if (!pso) return;
    if (ext_) binds_ = {};  // the extension may set anything
    if (pso != binds_.pso) cl_->SetPipelineState(pso);
    binds_.pso = pso;

    // constants
    GuestExtent size;
    SurfaceSize(bound_rt_ ? s.rt : s.zs, size.w, size.h);
    if (TryPbrDraw(s, a, sh, pk, k, stages, size, pso)) return;
    std::unique_ptr<draw_evidence::Recorder> evidence;
    if (draw_evidence_selector_.enabled && draw_evidence_selector_.Claim(cap_ != nullptr, sh.vkey, sh.pkey, cap_i_)) {
        try {
            evidence = std::make_unique<draw_evidence::Recorder>();
            draw_evidence::Identity id;
            id.frame = frame_; id.presents = presents_; id.draw_ordinal = cap_i_;
            id.vertex_key = sh.vkey; id.pixel_key = sh.pkey;
            id.vertex_source_hash = sh.vh; id.pixel_source_hash = sh.ph;
            id.guest_primitive = prim; id.guest_start = start; id.guest_count = count;
            id.guest_width = size.w; id.guest_height = size.h;
            const Target* target = bound_rt_ ? bound_rt_ : bound_ds_;
            id.host_width = target ? target->w : 0; id.host_height = target ? target->h : 0;
            id.flags = (sh.prog ? draw_evidence::ProgrammableVertex : 0) |
                (sh.fixed_transform ? draw_evidence::FixedTransform : 0) |
                (sh.quad ? draw_evidence::Quad : 0) | (k.blend ? draw_evidence::Blended : 0) |
                (signed_op ? draw_evidence::MultiplePasses : 0) | (pk.programmable ? draw_evidence::ProgrammablePixel : 0);
            // Explicit members only: never serialize compiler padding in PsoKey or State.
            std::array<uint32_t, 41> words{};
            size_t at = 0;
            for (uint32_t v : {k.regs, k.topo, k.has_rt, k.has_ds}) words[at++] = v;
            for (uint32_t v : k.elements) words[at++] = v;
            for (uint32_t v : {k.blend, k.src, k.dst, k.op, k.wmask, k.zen, k.zwrite, k.zfunc, k.sten,
                k.sfunc, k.smask, k.swmask, k.sfail, k.szfail, k.spass, k.cull, k.ccw, k.fill,
                std::bit_cast<uint32_t>(k.zbias), std::bit_cast<uint32_t>(k.zslope), k.rtfmt}) words[at++] = v;
            auto result = evidence->Begin(s, a, id,
                draw_evidence::Bytes(sh.vs_code->GetBufferPointer(), sh.vs_code->GetBufferSize()),
                draw_evidence::Bytes(sh.ps_code->GetBufferPointer(), sh.ps_code->GetBufferSize()), words);
            if (result != draw_evidence::Error::None) {
                LOGI("draw evidence: selected draw rejected (%s)", draw_evidence::ErrorText(result));
                evidence.reset();
            }
        } catch (...) {
            LOGI("draw evidence: selected diagnostic allocation failed; drawing continues");
            evidence.reset();
        }
    }
    DrawConstants c{};
    if (sh.car_paint) {
        c.carpaint[0] = std::clamp(graphics.car_relight, 0.0f, 1.0f);
        c.carpaint[1] = std::clamp(graphics.car_specular, 0.0f, 3.0f);
        c.carpaint[2] = std::clamp(graphics.car_roughness, 0.05f, 0.8f);
        c.carpaint[3] = std::clamp(graphics.car_reflection, 0.0f, 2.0f);
    }
    UploadConstants(s, sh, k, stages, size, c, evidence.get());
    if (ext_) binds_ = {};
    auto reupload = [&] {
        Alloc dc = Upload(sizeof(DrawConstants), 256);
        std::memcpy(dc.cpu, &c, sizeof(c));
        // Screen-effect corrections can replace b1; only the last bound copy belongs to this draw.
        if (evidence) evidence->DrawConstantsUpload(draw_evidence::Bytes(dc.cpu, sizeof(c)));
        cl_->SetGraphicsRootConstantBufferView(1, dc.gpu);
        last_dc_ = c;
        last_dc_gpu_ = binds_.dc = dc.gpu;
    };
    // the game's screen effects scaled by the graphics settings: alpha by sblend.y, colour and alpha by sblend.z
    bool fx = ScaleScreenEffect(k, stages, count, c);
    fx |= ScaleTextureEffect(k, stages, c);
    if (bound_rt_) bound_rt_->screen_copy = count == 4 && stages.tgt[0] && stages.tgt[0]->gw >= bound_rt_->gw * 2;
    if (signed_op) c.sblend[0] = guest_op == kBlendOpAddSigned ? 1.0f : 2.0f;  // the part that adds
    if (signed_op || fx) reupload();

    if (cap_) {
        CaptureDraw(s, prim, count, k, c, stages);
        EndDrawCapture(s, a, count, c, sh.fixed_transform);
        CaptureShaders(s, a, sh, pk);
    }

    // texture views and samplers, viewport and scissor, blend factor and stencil reference
    SetTextureViewsAndSamplers(s, a, stages);
    SetViewportScissor(s, size.w);
    SetBlendFactorStencilRef(s);

    // geometry, counted while the title runs a visibility test
    uint32_t query = BeginCountedDraw(s);
    DrawGeometry(a, evidence.get());
    if (signed_op) {
        k.op = kBlendOpRevSubtract;  // dst - src
        if (ID3D12PipelineState* sub = EnsurePipeline(k, sh.vs_code, sh.ps_code)) {
            cl_->SetPipelineState(sub);
            binds_.pso = sub;
            c.sblend[0] = guest_op == kBlendOpAddSigned ? 2.0f : 1.0f;  // the part that subtracts
            reupload();
            DrawGeometry(a);
        }
    }
    EndCountedDraw(query);
    if (evidence) {
        auto result = evidence->error();
        if (result == draw_evidence::Error::None) {
            wchar_t runtime_path[32768] = {};
            DWORD n = GetModuleFileNameW(nullptr, runtime_path, static_cast<DWORD>(std::size(runtime_path)));
            result = n && n < std::size(runtime_path)
                ? draw_evidence::WriteNew(cap_dir_, evidence->record(), std::filesystem::path(runtime_path))
                : draw_evidence::Error::IoFailure;
        }
        LOGI("draw evidence: selected draw result=%s (CPU inputs only)", draw_evidence::ErrorText(result));
    }
    if (photo_policy_.Armed()) ObservePhotoDraw(s, a, k, sh, stages);
    bool snap_tex = false;
    if (cap_ && diag::CaptureSnapTexture())
        for (int i = 0; i < 4; i++) {
            TextureDesc td;
            snap_tex |= s.textures[size_t(i)] && Describe(s.textures[size_t(i)], td) && td.data == diag::CaptureSnapTexture();
        }
    if (cap_ && bound_rt_ && (diag::CaptureSnap(cap_i_ - 1) || snap_tex)) {
        Capture(*bound_rt_, "snap", 1);
        Unbind();  // the save moved the target's state: the next draw binds it again
    }
}

// D3DBLEND factors the effect passes use
constexpr uint32_t kBlendOne = 1, kBlendSrcAlpha = 0x302, kBlendInvSrcAlpha = 0x303;
constexpr uint32_t kFullScreenWidth = 640;  // a target this wide is the frame, not one of the effect buffers

// MC3's post passes, found in race captures (PORT_DOCUMENTATION.md): each lays a half size buffer over the frame
// with a 4 vertex quad. Blended by alpha over it with the depth test on: the speed blur, its strength in the vertex
// alpha. Added by alpha: the light glow when the buffer was last filled by a copy of the frame, else the headlight
// pools drawn into it.
bool D3D12Backend::ScaleScreenEffect(const PsoKey& k, const DrawTextures& stages, uint32_t count, DrawConstants& c) {
    const Target* rt = bound_rt_;
    const Target* src = stages.tgt[0];
    if (!rt || rt->gw < kFullScreenWidth || count != 4 || !k.blend || !src || src->depth || src->gw * 2 > rt->gw)
        return false;
    const auto& g = app::Get().graphics;
    if (k.src == kBlendSrcAlpha && k.dst == kBlendInvSrcAlpha && k.zen) {
        c.sblend[1] = g.motion_blur - 1;
        return true;
    }
    if (k.src == kBlendSrcAlpha && k.dst == kBlendOne) {
        // colour and alpha both scale the add: the square root of the setting on each
        c.sblend[2] = std::sqrt(src->screen_copy ? g.glow : g.headlights) - 1;
        return true;
    }
    return false;
}

// draws of a texture recognised by its content (TexRole), scaled by its graphics setting: the car neon. Blended by
// source alpha, the shader raises the alpha first and puts what is left over 1 into the colour (sblend.w), so the
// light the draw adds grows by the setting; blended any other way, the colour alone is scaled (sblend.z, with
// sblend.y keeping the alpha as it was).
bool D3D12Backend::ScaleTextureEffect(const PsoKey& k, const DrawTextures& stages, DrawConstants& c) {
    bool neon = false;
    for (int i = 0; i < 4; i++) neon |= stages.res[i] && stages.role[i] == kRoleNeon;
    if (!neon || !k.blend) return false;
    float scale = std::clamp(app::Get().graphics.neon, 0.0f, 4.0f);
    if (scale == 1) return false;
    if (k.src == kBlendSrcAlpha) {
        c.sblend[3] = scale - 1;
    } else if (scale > 0) {
        c.sblend[2] = scale - 1;
        c.sblend[1] = 1 / scale - 1;
    } else {
        c.sblend[2] = -1;
    }
    return true;
}

// a guest rect scaled to the target and clamped to it
D3D12_RECT ScaledRect(const Target& t, int x0, int y0, int x1, int y1) {
    auto r = app::resolution::MapRect({t.gw, t.gh}, {t.w, t.h}, {x0, y0, x1, y1});
    return {r.left, r.top, r.right, r.bottom};
}

// rs: the rects buffer, reused across targets
void D3D12Backend::ClearTarget(Target& t, const State& s, const ClearArgs& c, std::vector<D3D12_RECT>& rs) {
    rs.clear();
    if (c.count && c.rects) {
        for (uint32_t i = 0; i < c.count; i++) {
            int32_t r[4];
            std::memcpy(r, c.rects + size_t(i) * sizeof(r), sizeof(r));
            rs.push_back(ScaledRect(t, r[0], r[1], r[2], r[3]));
        }
    } else {
        const auto& v = s.viewport;
        rs.push_back(ScaledRect(t, int(v.x), int(v.y), int(v.x + v.w), int(v.y + v.h)));
    }
    if (t.depth) {
        Transition(t.res.Get(), t.state, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        D3D12_CLEAR_FLAGS cf = D3D12_CLEAR_FLAGS(((c.flags & kClearZ) ? D3D12_CLEAR_FLAG_DEPTH : 0) |
                                                 ((c.flags & kClearStencil) ? D3D12_CLEAR_FLAG_STENCIL : 0));
        float reversed = 1 - std::clamp(c.z, 0.0f, 1.0f);
        cl_->ClearDepthStencilView(Cpu(dsv_heap_.Get(), t.view, dsv_inc_), cf, reversed, UINT8(c.stencil),
                                   UINT(rs.size()), rs.data());
    } else {
        // pixels the CPU wrote before this clear go in first, a later upload would paint over the clear
        if (LatchCpuWrite(t)) UploadTarget(t, s.rt);
        Transition(t.res.Get(), t.state, D3D12_RESOURCE_STATE_RENDER_TARGET);
        float col[4];
        Color(c.color, col);
        for (int i = 0; t.range != 1 && i < 3; i++) col[i] /= t.range;
        // D3DCLEAR_TARGET_R/G/B/A are bits 4 to 7, D3D12's write mask has the same order
        uint8_t mask = uint8_t((c.flags & kClearTarget) >> 4);
        if (mask == D3D12_COLOR_WRITE_ENABLE_ALL) {
            cl_->ClearRenderTargetView(Cpu(rtv_heap_.Get(), t.view, rtv_inc_), col, UINT(rs.size()), rs.data());
        } else {
            ClearChannels(t, col, mask, rs);
        }
    }
    t.drawn = true;
}

// ClearChannels' pipeline for the target format and channel mask, kept in blit_psos_; null when it can't be made
ID3D12PipelineState* D3D12Backend::EnsureClearPipeline(DXGI_FORMAT rtv, uint8_t mask) {
    uint64_t seed = uint64_t(rtv) << 8 | mask;
    auto& pso = blit_psos_[Hash(kClearHlsl, std::strlen(kClearHlsl), seed)];
    if (pso) return pso.Get();
    uint64_t h;
    ID3DBlob* vs = Compile(kClearHlsl, "vs", "vs_5_0", h);
    ID3DBlob* ps = Compile(kClearHlsl, "psmain", "ps_5_0", h);
    if (!vs || !ps) return nullptr;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC d = FullScreenPipelineDesc(vs, ps, rtv);
    d.BlendState.RenderTarget[0].RenderTargetWriteMask = mask;
    if (FAILED(dev_->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&pso)))) return nullptr;
    return pso.Get();
}

// a clear of some colour channels: drawn over each rect writing only those
void D3D12Backend::ClearChannels(Target& t, const float col[4], uint8_t mask, const std::vector<D3D12_RECT>& rs) {
    ID3D12PipelineState* pso = EnsureClearPipeline(t.fmt, mask);
    if (!pso) {
        LOG_FIRST(4, "d3d12: channel clear pipeline failed, mask %X", mask);
        return;
    }
    SetBlitTarget(Cpu(rtv_heap_.Get(), t.view, rtv_inc_), t.w, t.h);
    Alloc cb = Upload(256, 256);
    std::memcpy(cb.cpu, col, 4 * sizeof(float));
    uint32_t slot = AllocSrvSlots(1);
    for (const D3D12_RECT& r : rs) {
        cl_->RSSetScissorRects(1, &r);
        binds_ = {};
        DrawFullScreen(pso, cb.gpu, slot);
    }
}

void D3D12Backend::Clear(const State& s, uint32_t count, const uint8_t* rects, uint32_t flags, uint32_t color, float z,
                         uint32_t stencil) {
    PerfScope perf_clear{g_perf.clear};
    if (cap_) {
        CapStep(s.rt);
        fprintf(cap_, "%04u clear rt %08X ds %08X flags %X color %08X z %.3f\n", cap_i_++, s.rt.data, s.zs.data, flags,
                color, z);
    }
    Target* rt = (flags & kClearTarget) ? EnsureTarget(s.rt, false) : nullptr;
    Target* ds = (flags & (kClearZ | kClearStencil)) ? EnsureTarget(s.zs, true) : nullptr;
    uint32_t gw;
    uint32_t gh;
    SurfaceSize(rt ? s.rt : s.zs, gw, gh);
    std::vector<D3D12_RECT> rs;
    ClearArgs c{.count = count, .rects = rects, .flags = flags, .color = color, .z = z, .stencil = stencil};
    for (Target* t : {rt, ds}) {
        if (t) ClearTarget(*t, s, c, rs);
    }
}

// CopyRects' guest lists: src_rects RECTs, dst_points POINTs, all int32
constexpr uint32_t kGuestRectBytes = 16;
constexpr uint32_t kRectLeft = 0;
constexpr uint32_t kRectTop = 4;
constexpr uint32_t kRectRight = 8;
constexpr uint32_t kRectBottom = 12;
constexpr uint32_t kGuestPointBytes = 8;
constexpr uint32_t kPointX = 0;
constexpr uint32_t kPointY = 4;

struct GuestRect {
    int32_t left;
    int32_t top;
    int32_t right;
    int32_t bottom;
};

struct GuestPoint {
    int32_t x;
    int32_t y;
};

GuestRect ReadGuestRect(uint32_t rects, uint32_t i) {
    uint32_t r = rects + i * kGuestRectBytes;
    GuestRect g;
    g.left = int32_t(kernel::R32(r + kRectLeft));
    g.top = int32_t(kernel::R32(r + kRectTop));
    g.right = int32_t(kernel::R32(r + kRectRight));
    g.bottom = int32_t(kernel::R32(r + kRectBottom));
    return g;
}

GuestPoint ReadGuestPoint(uint32_t points, uint32_t i) {
    uint32_t p = points + i * kGuestPointBytes;
    GuestPoint g;
    g.x = int32_t(kernel::R32(p + kPointX));
    g.y = int32_t(kernel::R32(p + kPointY));
    return g;
}

// a drawn copy's rect in guest pixels: the source corners and where it lands
struct DrawnRect {
    float x0 = 0;
    float y0 = 0;
    float x1 = 0;
    float y1 = 0;
    float dx = 0;
    float dy = 0;
};

// without dst_points the rect lands where it is
DrawnRect ReadDrawnRect(uint32_t src_rects, uint32_t dst_points, uint32_t i) {
    GuestRect g = ReadGuestRect(src_rects, i);
    DrawnRect r;
    r.x0 = float(g.left);
    r.y0 = float(g.top);
    r.x1 = float(g.right);
    r.y1 = float(g.bottom);
    r.dx = r.x0;
    r.dy = r.y0;
    if (dst_points) {
        GuestPoint p = ReadGuestPoint(dst_points, i);
        r.dx = float(p.x);
        r.dy = float(p.y);
    }
    return r;
}

// a guest coordinate read as unsigned, times the host scale
UINT ScaleUnsigned(int32_t v, float sc) { return UINT(uint32_t(v) * sc); }

// a drawn copy's taps round down a ratio a hair over a whole number
constexpr float kTapsSlack = 1e-3f;

// one Blit per rect: for formats that differ
void D3D12Backend::CopyByDraw(const Target& src_t, ID3D12Resource* src_res, D3D12_RESOURCE_STATES& src_state,
                              Target& dst_t, const CopyList& l) {
    Transition(src_res, src_state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    Transition(dst_t.res.Get(), dst_t.state, D3D12_RESOURCE_STATE_RENDER_TARGET);
    D3D12_CPU_DESCRIPTOR_HANDLE rh = Cpu(rtv_heap_.Get(), dst_t.view, rtv_inc_);
    cl_->OMSetRenderTargets(1, &rh, FALSE, nullptr);
    float src_sx = float(src_t.w) / src_t.gw, src_sy = float(src_t.h) / src_t.gh;
    float dst_sx = float(dst_t.w) / dst_t.gw, dst_sy = float(dst_t.h) / dst_t.gh;
    bool listed = l.count && l.src_rects;
    uint32_t n = listed ? l.count : 1;
    for (uint32_t i = 0; i < n; i++) {
        float whole_w = float(std::min(src_t.gw, dst_t.gw));
        float whole_h = float(std::min(src_t.gh, dst_t.gh));
        DrawnRect r{.x0 = 0, .y0 = 0, .x1 = whole_w, .y1 = whole_h};
        if (listed) r = ReadDrawnRect(l.src_rects, l.dst_points, i);
        if (r.x1 <= r.x0 || r.y1 <= r.y0) continue;
        D3D12_VIEWPORT vp{r.dx * dst_sx, r.dy * dst_sy, (r.x1 - r.x0) * dst_sx, (r.y1 - r.y0) * dst_sy,
                          0, 1};
        LONG right = std::min(LONG(vp.TopLeftX + vp.Width), LONG(dst_t.w));
        LONG bottom = std::min(LONG(vp.TopLeftY + vp.Height), LONG(dst_t.h));
        D3D12_RECT scissor{LONG(vp.TopLeftX), LONG(vp.TopLeftY), right, bottom};
        if (scissor.right <= scissor.left || scissor.bottom <= scissor.top) continue;
        cl_->RSSetViewports(1, &vp);
        cl_->RSSetScissorRects(1, &scissor);
        binds_ = {};
        float uv[4] = {r.x0 / src_t.gw, r.y0 / src_t.gh, (r.x1 - r.x0) / src_t.gw, (r.y1 - r.y0) / src_t.gh};
        uint32_t taps = uint32_t(std::clamp(std::ceil(std::max({src_sx / dst_sx, src_sy / dst_sy, 1.0f}) - kTapsSlack), 1.0f, 8.0f));
        float mul = 1 / dst_t.range;
        BlitOptions o{.alpha = true, .taps = taps, .mul = mul, .rtv = dst_t.fmt};
        Blit(src_res, src_t.fmt, uv, o);
    }
    dst_t.drawn = true;
    bound_rt_ = bound_ds_ = nullptr;
}

// one CopyTextureRegion per rect, clamped to both targets
void D3D12Backend::CopyByRegion(const Target& src_t, ID3D12Resource* src_res, D3D12_RESOURCE_STATES& src_state,
                                Target& dst_t, const CopyList& l) {
    Transition(src_res, src_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Transition(dst_t.res.Get(), dst_t.state, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION dl{dst_t.res.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    D3D12_TEXTURE_COPY_LOCATION sl{src_res, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    float sx = float(src_t.w) / src_t.gw, sy = float(src_t.h) / src_t.gh;
    float dx_scale = float(dst_t.w) / dst_t.gw, dy_scale = float(dst_t.h) / dst_t.gh;
    bool listed = l.count && l.src_rects;
    uint32_t n = listed ? l.count : 1;
    for (uint32_t i = 0; i < n; i++) {
        D3D12_BOX b{0, 0, 0, std::min(src_t.w, dst_t.w), std::min(src_t.h, dst_t.h), 1};
        uint32_t dx = 0;
        uint32_t dy = 0;
        if (listed) {
            GuestRect r = ReadGuestRect(l.src_rects, i);
            b.left = ScaleUnsigned(r.left, sx);
            b.top = ScaleUnsigned(r.top, sy);
            b.right = std::min(ScaleUnsigned(r.right, sx), src_t.w);
            b.bottom = std::min(ScaleUnsigned(r.bottom, sy), src_t.h);
            dx = b.left;
            dy = b.top;
            if (l.dst_points) {
                GuestPoint p = ReadGuestPoint(l.dst_points, i);
                dx = ScaleUnsigned(p.x, dx_scale);
                dy = ScaleUnsigned(p.y, dy_scale);
            }
        }
        if (b.right <= b.left || b.bottom <= b.top || dx >= dst_t.w || dy >= dst_t.h) continue;
        b.right = std::min<UINT>(b.right, b.left + dst_t.w - dx);
        b.bottom = std::min<UINT>(b.bottom, b.top + dst_t.h - dy);
        cl_->CopyTextureRegion(&dl, dx, dy, 0, &sl, &b);
    }
    dst_t.drawn = true;
    bound_rt_ = bound_ds_ = nullptr;
}

void D3D12Backend::Copy(const Surface& src, const Surface& dst, uint32_t count, uint32_t src_rects,
                        uint32_t dst_points) {
    PerfScope perf_copy{g_perf.copy};
    Target* src_t = FindTarget(src, false);
    if (cap_) {
        const char* from = "memory";
        if (src_t) from = src_t->drawn ? "target" : "idle target";
        fprintf(cap_, "%04u copy %08X %s to %08X\n", cap_i_++, src.data, from, dst.data);
    }
    // pixels the CPU made: the copy happens in memory, textures and targets pick it up from there
    if (!src_t || !src_t->drawn) {
        if (!CopySurfaceMemory(src, dst, count, src_rects, dst_points)) {
            LOG_FIRST(8, "d3d12: CopyRects %08X to %08X skipped", src.data, dst.data);
        } else if (Target* dst_t = FindTarget(dst, false)) {
            UploadTarget(*dst_t, dst);
        }
        return;
    }
    Resolve(*src_t);
    Target* dst_t = EnsureTarget(dst, false);
    if (!dst_t || src_t == dst_t) return;
    CopyList l{.count = count, .src_rects = src_rects, .dst_points = dst_points};
    if (src_t->fmt != dst_t->fmt ||
        !app::resolution::SameScale({src_t->gw, src_t->gh}, {src_t->w, src_t->h},
                                   {dst_t->gw, dst_t->gh}, {dst_t->w, dst_t->h})) {
        // formats differ: drawn across
        CopyByDraw(*src_t, src_t->res.Get(), src_t->state, *dst_t, l);
        return;
    }
    CopyByRegion(*src_t, src_t->res.Get(), src_t->state, *dst_t, l);
    if (cap_) {
        char name[32];
        snprintf(name, sizeof(name), "%04u_copy_%08X", cap_i_, dst_t->data);
        CapSave(*dst_t, name);
    }
}

void D3D12Backend::ResizeSwap() {
    HWND hwnd = static_cast<HWND>(app::Window());
    RECT rc;
    GetClientRect(hwnd, &rc);
    uint32_t w = std::max<LONG>(rc.right - rc.left, 1);
    uint32_t h = std::max<LONG>(rc.bottom - rc.top, 1);
    if (w == bb_w_ && h == bb_h_) return;
    Flush();
    for (auto& f : frames_) {
        if (f.fence) WaitFence(f.fence);
    }
    for (auto& b : bb_) b.Reset();
    HRESULT resized = swap_->ResizeBuffers(kFrames, w, h, DXGI_FORMAT_B8G8R8A8_UNORM, tearing_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
    if (FAILED(resized)) {
        // ResizeBuffers preserves its old buffers on failure. Reacquire their views and keep the settings UI usable.
        CreateBackBufferViews();
        app::ReportGraphicsFailure("The GPU could not resize the display buffers. Previous display settings were restored.");
        return;
    }
    CreateBackBufferViews();
    bb_w_ = w;
    bb_h_ = h;
    post_failed_ = false;
}

void D3D12Backend::InitPhotoCapture() {
    const char* hashes = diag::CaptureTextureHashes();
    if (!hashes || !*hashes) return;
    const auto config = photo_capture::Parse(hashes, diag::CaptureTextureFrames());
    if (!config || diag::CaptureDir().empty()) {
        LOGE("capture: photo diagnostic disabled; valid texture hashes/ordinals and XBE_CAPTURE_DIR are required");
        return;
    }
    photo_config_ = *config;
    photo_policy_ = photo_capture::Policy(*config);
    LOGR("capture: same-present photo diagnostic armed (%u hashes, at most %u attempts, 64 MiB/frame ceiling)",
         photo_config_.hash_count, photo_config_.ordinal_count);
}

void D3D12Backend::ObservePhotoDraw(const State& s, const Assembled& a, const PsoKey& k,
                                    const DrawShaders& sh, const DrawTextures& stages) {
    if (!bound_rt_ || !(k.wmask & 7) || !(a.index_count || a.draw_count) || !s.viewport.w || !s.viewport.h) return;
    bool relevant = false;
    for (int i = 0; i < 4; ++i)
        relevant |= stages.res[i] && photo_policy_.Wants(stages.photo_identity[i].original_hash);
    if (!relevant) return;
    auto [found, inserted] = photo_sampled_.try_emplace(sh.ph, 0);
    if (inserted) {
        auto mask = photo_capture::CompiledTextureStages(sh.ps_code);
        if (!mask) LOGE("capture: photo shader reflection failed; this shader cannot trigger a capture");
        found->second = mask.value_or(0);  // Fail closed: merely bound textures do not count.
    }
    for (uint32_t stage = 0; stage < 4; ++stage) {
        const auto hash = stages.photo_identity[stage].original_hash;
        if (!(found->second & (1u << stage)) || !stages.res[stage] || !photo_policy_.Wants(hash)) continue;
        TextureDesc guest;
        if (!Describe(s.textures[stage], guest)) continue;
        const auto host = stages.res[stage]->GetDesc();
        photo_policy_.ObserveIssuedTexture(hash);
        for (uint32_t i = 0; i < photo_config_.hash_count; ++i)
            if (photo_config_.hashes[i] == hash)
                photo_observed_[i] = {hash, stage, guest.data, bound_rt_->data, guest.width, guest.height,
                                      uint32_t(host.Width), host.Height, stages.photo_replaced[stage]};
    }
}

void D3D12Backend::CapturePhoto(const photo_capture::Decision& decision, uint32_t backbuffer) {
    CaptureStamp stamp{frame_, presents_,
        uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count()),
        uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count()), GetCurrentProcessId()};
    std::error_code error;
    const auto base = std::filesystem::weakly_canonical(app::ExeDir() / diag::CaptureDir() / "photo", error);
    if (error) { LOGE("capture: photo directory resolution failed: %s", error.message().c_str()); return; }
    const auto directory = CreateUniqueCaptureDirectory(base, stamp);
    if (directory.error) { LOGE("capture: new photo directory failed: %s", directory.error.message().c_str()); return; }
    std::vector<uint8_t> pixels;
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_PRESENT;  // DrawFrame completed this transition.
    const bool read = photo_capture::ReadPresented(bb_w_, bb_h_, pixels,
        [&] { Transition(bb_[backbuffer].Get(), state, D3D12_RESOURCE_STATE_COPY_SOURCE); },
        [&](auto& out) { return ReadBack(bb_[backbuffer].Get(), bb_w_, bb_h_, out); },
        [&] { Transition(bb_[backbuffer].Get(), state, D3D12_RESOURCE_STATE_PRESENT); });
    const bool saved = read && photo_capture::SaveBmpNew(directory.path / "present.bmp", bb_w_, bb_h_, pixels);
    FILE* metadata = OpenCaptureFileNew(directory.path / "photo.json");
    bool metadata_ok = false;
    if (metadata) {
        const auto& settings = app::Get();
        const auto& graphics = settings.graphics;
        bool wrote = std::fprintf(metadata,
            "{\n  \"format\": \"mc3-photo-capture-v1\",\n"
            "  \"scope\": \"same completed backbuffer post-DrawFrame/pre-Present; no Shrink\",\n"
            "  \"frame\": %llu, \"completed_presents\": %llu, \"matching_ordinal\": %u,\n"
            "  \"wall_unix_ns\": %llu, \"monotonic_ns\": %llu, \"process_id\": %u,\n"
            "  \"width\": %u, \"height\": %u, \"backbuffer_index\": %u, \"format_name\": \"BGRA8\",\n"
            "  \"byte_ceiling\": %llu, \"readback_succeeded\": %s, \"bmp_saved\": %s,\n"
            "  \"observed_mask\": %u, \"render_percent\": %u, \"scene_width\": %u, \"scene_height\": %u,\n"
            "  \"post\": %s, \"aa\": %u, \"sharpen\": %.9g, \"texture_pack\": %s, \"texture_detail\": %u,\n"
            "  \"context_limit\": \"EXE/source/settings/file SHA-256 require external receipt; shader access does not prove visible pixels; swapchain Present result not recorded\",\n"
            "  \"textures\": [\n",
            static_cast<unsigned long long>(stamp.frame), static_cast<unsigned long long>(stamp.presents), decision.matched_ordinal,
            static_cast<unsigned long long>(stamp.wall_unix_ns), static_cast<unsigned long long>(stamp.monotonic_ns), stamp.process_id,
            bb_w_, bb_h_, backbuffer, static_cast<unsigned long long>(photo_capture::kMaxFrameBytes),
            read ? "true" : "false", saved ? "true" : "false", decision.observed_mask, settings.render_percent,
            graphics.scene_width, graphics.scene_height, graphics.post ? "true" : "false", graphics.aa,
            double(graphics.sharpen), graphics.texture_pack ? "true" : "false", graphics.texture_detail) >= 0;
        for (uint32_t i = 0; i < photo_config_.hash_count; ++i) {
            const auto& o = photo_observed_[i];
            wrote &= std::fprintf(metadata,
                "    {\"original_hash\": \"%016llX\", \"stage\": %u, \"guest_data\": %u, \"draw_target_data\": %u, "
                "\"guest_size\": [%u,%u], \"host_size\": [%u,%u], \"replacement_used\": %s}%s\n",
                static_cast<unsigned long long>(o.original_hash), o.stage, o.guest_data, o.target_data,
                o.guest_w, o.guest_h, o.host_w, o.host_h, o.replaced ? "true" : "false",
                i + 1 < photo_config_.hash_count ? "," : "") >= 0;
        }
        wrote &= std::fprintf(metadata, "  ]\n}\n") >= 0;
        metadata_ok = std::fclose(metadata) == 0 && wrote;
    }
    if (saved && metadata_ok) LOGR("capture: photo ordinal %u saved to %s", decision.matched_ordinal,
                                   hostpath::Utf8(directory.path).c_str());
    else LOGE("capture: photo attempt %u incomplete (read %u, BMP %u, metadata %u); inspect %s", decision.matched_ordinal,
              unsigned(read), unsigned(saved), unsigned(metadata_ok), hostpath::Utf8(directory.path).c_str());
}

void D3D12Backend::FinishCapture(const Surface& front) {
    CapStep(Surface{});
    if (Target* f = FindTarget(front, false)) CapSave(*f, "final");
    for (auto& t : cap_tex_) {
        char name[32];
        snprintf(name, sizeof(name), "tex_%08X", t.data);
        D3D12_RESOURCE_STATES st = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        CapSave({.res = t.res.Get(), .state = &st, .fmt = t.fmt, .w = t.w, .h = t.h}, name);
    }
    fclose(cap_);
    cap_ = nullptr;
    cap_tex_.clear();
    LOGR("d3d12: %s frame captured to %s", cap_auto_ ? "automatic" : "requested", hostpath::Utf8(cap_dir_).c_str());
    capture::g_status.Finished(hostpath::Utf8(cap_dir_), cap_auto_);
}

void D3D12Backend::StartCapture() {
    std::error_code ec;
    auto base = std::filesystem::weakly_canonical(app::ExeDir() / CaptureBase(diag::CaptureDir()), ec);
    if (ec) {
        LOGE("capture: cannot resolve output directory: %s", ec.message().c_str());
        capture::g_status.Failed("cannot resolve the captures folder: " + ec.message());
        return;
    }
    CaptureStamp stamp{frame_, presents_,
        uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count()),
        uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count()), GetCurrentProcessId()};
    auto directory = CreateUniqueCaptureDirectory(base, stamp);
    if (directory.error || !WriteCaptureMetadata(directory, stamp, ec)) {
        const auto error = directory.error ? directory.error : ec;
        LOGE("capture: cannot create new evidence session: %s", error.message().c_str());
        capture::g_status.Failed("cannot create a capture folder: " + error.message());
        return;
    }
    FILE* stream = OpenCaptureFileNew(directory.path / "frame.txt");
    if (!stream) {
        LOGE("capture: cannot create new frame log (errno %d)", errno);
        capture::g_status.Failed("cannot create frame.txt (errno " + std::to_string(errno) + ")");
        return;
    }
    cap_dir_ = std::move(directory.path);
    cap_ = stream;
    cap_i_ = cap_rt_ = 0;
    cap_seen_.clear();
    cap_sh_.clear();
}

// ramp_ holds the red, green and blue ramps, 256 entries each
constexpr int kRampRed = 0;
constexpr int kRampGreen = 256;
constexpr int kRampBlue = 512;

void D3D12Backend::UploadGammaRamp() {
    constexpr uint32_t kRampBytes = 256 * 4;  // one BGRA texel per ramp entry
    ramp_dirty_ = false;
    Alloc a = Upload(kRampBytes + D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
    bool identity = true;
    for (int i = 0; i < 256; i++) {
        uint8_t* p = a.cpu + i * 4;
        p[0] = ramp_[kRampBlue + i];
        p[1] = ramp_[kRampGreen + i];
        p[2] = ramp_[kRampRed + i];
        p[3] = 255;
        identity &= ramp_[kRampRed + i] == i && ramp_[kRampGreen + i] == i && ramp_[kRampBlue + i] == i;
    }
    gamma_ = !identity;
    Transition(lut_.Get(), lut_state_, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION dl{lut_.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    D3D12_TEXTURE_COPY_LOCATION sl{a.res, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    sl.PlacedFootprint.Offset = a.off;
    sl.PlacedFootprint.Footprint = {DXGI_FORMAT_B8G8R8A8_UNORM, 256, 1, 1, kRampBytes};
    cl_->CopyTextureRegion(&dl, 0, 0, 0, &sl, nullptr);
}

bool D3D12Backend::EnsurePostTargets(uint32_t w, uint32_t h) {
    if (post_failed_) return false;
    if (post_[0] && post_w_ == w && post_h_ == h) return true;
    for (int i = 0; i < 2; i++) {
        if (post_[i]) Release(std::move(post_[i]));
        if (post_rtv_[i] == ~0u) {
            if (free_rtv_.empty()) return false;
            post_rtv_[i] = free_rtv_.back();
            free_rtv_.pop_back();
        }
        D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width = w;
        rd.Height = h;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.Format = kBgra;
        rd.SampleDesc.Count = 1;
        rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_CLEAR_VALUE cv{kBgra, {0, 0, 0, 1}};
        post_state_[i] = D3D12_RESOURCE_STATE_RENDER_TARGET;
        if (FAILED(dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, post_state_[i], &cv,
                                                 IID_PPV_ARGS(&post_[i])))) {
            LOGE("d3d12: picture chain targets %ux%u failed", w, h);
            post_[0].Reset();
            post_failed_ = true;
            app::ReportGraphicsFailure("The GPU could not allocate the picture effects. Previous display settings were restored. Try a lower scene resolution.");
            return false;
        }
        dev_->CreateRenderTargetView(post_[i].Get(), nullptr, Cpu(rtv_heap_.Get(), post_rtv_[i], rtv_inc_));
    }
    post_w_ = w;
    post_h_ = h;
    return true;
}

void D3D12Backend::PostPass(const char* entry, int dst, ID3D12Resource* s0, DXGI_FORMAT f0, ID3D12Resource* s1,
                            DXGI_FORMAT f1, const float* p, uint32_t n) {
    ID3D12PipelineState* pso = EnsurePassPipeline(kPostHlsl, entry, kBgra, -1);
    if (!pso) return;
    uint32_t slot = AllocSrvSlots(4);
    ID3D12Resource* src[2] = {s0, s1};
    DXGI_FORMAT fmt[2] = {f0, f1};
    for (int i = 0; i < 4; i++) {
        D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
        sv.Format = i < 2 && src[i] ? fmt[i] : kBgra;
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sv.Texture2D.MipLevels = 1;
        dev_->CreateShaderResourceView(i < 2 ? src[i] : nullptr, &sv, Cpu(srv_heap_.Get(), slot + i, srv_inc_));
    }
    Alloc cb = Upload(256, 256);
    std::memset(cb.cpu, 0, 64);
    std::memcpy(cb.cpu, p, std::min<uint32_t>(n, 16) * sizeof(float));
    Transition(post_[dst].Get(), post_state_[dst], D3D12_RESOURCE_STATE_RENDER_TARGET);
    D3D12_CPU_DESCRIPTOR_HANDLE rh = Cpu(rtv_heap_.Get(), post_rtv_[dst], rtv_inc_);
    cl_->OMSetRenderTargets(1, &rh, FALSE, nullptr);
    SetFullViewport(post_w_, post_h_);
    DrawFullScreen(pso, cb.gpu, slot);
    bound_rt_ = bound_ds_ = nullptr;
}

bool D3D12Backend::PostProcess(Target& t, D3D12_CPU_DESCRIPTOR_HANDLE bb, const D3D12_VIEWPORT& vp) {
    const app::Settings::Graphics& g = app::Get().graphics;
    if (!g.post) return false;
    uint32_t w = uint32_t(vp.Width + 0.5f), h = uint32_t(vp.Height + 0.5f);
    if (!w || !h || !EnsurePostTargets(w, h)) return false;
    Transition(t.res.Get(), t.state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    // supersampled frames are box filtered down: one tap per source texel under each output pixel
    float taps = std::clamp(std::round(float(t.w) / float(w)), 1.0f, 4.0f);
    float grade[16] = {1.0f / w, 1.0f / h, taps, gamma_ ? 1.0f : 0.0f,
                       g.brightness, g.contrast, g.saturation, g.vibrance,
                       g.gamma};
    PostPass("grade", 0, t.res.Get(), t.fmt, lut_.Get(), kBgra, grade, 16);
    int cur = 0;
    auto step = [&](const char* entry, float amount) {
        Transition(post_[cur].Get(), post_state_[cur], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        float p[4] = {1.0f / w, 1.0f / h, amount, 0};
        PostPass(entry, 1 - cur, post_[cur].Get(), kBgra, nullptr, kBgra, p, 4);
        cur = 1 - cur;
    };
    if (g.aa == 1 && g.aa_strength > 0) step("fxaa", g.aa_strength);
    if (g.sharpen > 0) step("cas", g.sharpen);
    // into the back buffer as the plain blit would place the frame
    Transition(post_[cur].Get(), post_state_[cur], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    cl_->OMSetRenderTargets(1, &bb, FALSE, nullptr);
    D3D12_RECT scissor{0, 0, LONG(bb_w_), LONG(bb_h_)};
    cl_->RSSetViewports(1, &vp);
    cl_->RSSetScissorRects(1, &scissor);
    float uv[4] = {0, 0, 1, 1};
    Blit(post_[cur].Get(), kBgra, uv, {.gamma = false});
    return true;
}

void D3D12Backend::BlitFrame(Target& t, D3D12_CPU_DESCRIPTOR_HANDLE bb) {
    // 16:9 when the console reports a widescreen display, else 4:3
    bool wide = app::Widescreen();
    float aspect = wide ? 16.0f / 9.0f : 4.0f / 3.0f;
    float w = float(bb_w_);
    float h = float(bb_h_);
    float vw = std::min(w, h * aspect);
    float vh = vw / aspect;
    float uv[4] = {0, 0, 1, 1};
    D3D12_VIEWPORT vp{(w - vw) / 2, (h - vh) / 2, vw, vh, 0, 1};
    if (PostProcess(t, bb, vp)) return;
    D3D12_RECT scissor{0, 0, LONG(bb_w_), LONG(bb_h_)};
    cl_->RSSetViewports(1, &vp);
    cl_->RSSetScissorRects(1, &scissor);
    binds_ = {};
    Transition(t.res.Get(), t.state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    Blit(t.res.Get(), t.fmt, uv, {.gamma = gamma_});
}

void D3D12Backend::RenderUi() {
    std::lock_guard lock(app::UiLock());
    ImGui_ImplDX12_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    app::DrawSettingsMenu(float(fps_));
    if (app::kGame.ui) app::kGame.ui(fps_);
    ImGui::Render();
    ID3D12DescriptorHeap* uh[] = {ui_heap_.Get()};
    cl_->SetDescriptorHeaps(1, uh);
    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), cl_.Get());
}

void D3D12Backend::DrawFrame(Target* t, uint32_t bi) {
    D3D12_RESOURCE_STATES bs = D3D12_RESOURCE_STATE_PRESENT;
    Transition(bb_[bi].Get(), bs, D3D12_RESOURCE_STATE_RENDER_TARGET);
    D3D12_CPU_DESCRIPTOR_HANDLE rh = Cpu(rtv_heap_.Get(), kRtvCount + bi, rtv_inc_);
    float black[4] = {0, 0, 0, 1};
    cl_->ClearRenderTargetView(rh, black, 0, nullptr);
    cl_->OMSetRenderTargets(1, &rh, FALSE, nullptr);
    bound_rt_ = bound_ds_ = nullptr;
    if (t) BlitFrame(*t, rh);
    RenderUi();
    Transition(bb_[bi].Get(), bs, D3D12_RESOURCE_STATE_PRESENT);
}

void D3D12Backend::ReleaseOldTextures() {
    constexpr uint64_t kSweepEvery = 60;    // presents between sweeps
    constexpr size_t kTexturesKept = 2048;  // no sweep below this many
    constexpr uint64_t kIdleFrames = 600;
    // textures unused for ten seconds go, levels stream theirs in and out
    if (presents_ % kSweepEvery != 0 || textures_.size() <= kTexturesKept) return;
    for (auto it = textures_.begin(); it != textures_.end();) {
        if (it->second.frame + kIdleFrames < presents_) {
            CurrentFrame().garbage.push_back(it->second.res);
            if (it->second.pbr_nrm) CurrentFrame().garbage.push_back(it->second.pbr_nrm);
            if (it->second.pbr_orm) CurrentFrame().garbage.push_back(it->second.pbr_orm);
            it = textures_.erase(it);
        } else {
            ++it;
        }
    }
}

void D3D12Backend::ResetFrame() {
    Frame& f = CurrentFrame();
    const uint64_t completed = fence_->GetCompletedValue();
    if (completed == UINT64_MAX) {
        DrainDebugMessages("removed device during frame reset");
        ReportDebugDred("removed device during frame reset");
        CheckGpu(dev_.Get(), DXGI_ERROR_DEVICE_REMOVED, "resetting the frame allocator");
    }
    if (f.fence && completed < f.fence) {
        auto perf_wait = PerfClock::now();
        WaitFence(f.fence);
        g_perf.wait += PerfMs(perf_wait);
    }
    CompletePbrFrame(f);
    CheckGpu(dev_.Get(), f.alloc->Reset(), "resetting the frame allocator");
    f.garbage.clear();
    f.pbr_retained.clear();
    f.used = 0;
    f.srv = 0;
    CheckGpu(dev_.Get(), cl_->Reset(f.alloc.Get(), nullptr), "resetting the command list");
    Begin();
}

void D3D12Backend::CountFps() {
    fps_n_++;
    auto now = std::chrono::steady_clock::now();
    double el = std::chrono::duration<double>(now - fps_t_).count();
    if (el >= 0.5) {
        fps_ = fps_n_ / el;
        fps_n_ = 0;
        fps_t_ = now;
    }
}

void LogPerf() {
    static PerfClock::time_point perf_start = PerfClock::now();
    static uint64_t perf_frames = 0;
    perf_frames++;
    double el = PerfMs(perf_start);
    if (el >= 1000.0) {
        Perf& p = g_perf;
        double n = double(perf_frames);
        double host = p.draw + p.clear + p.copy + p.present + p.sync;
        LOGI("perf: %.1f fps %.2f ms/frame | host %.2f (draw %.2f: assemble %.2f bind %.2f [check %.2f, upload %.2f] "
             "compile %.2f; clear %.2f copy %.2f sync %.2f present %.2f [swap %.2f wait %.2f]) other %.2f",
             n * 1000.0 / el, el / n, host / n, p.draw / n, p.assemble / n, p.bind / n, p.sig / n, p.texup / n,
             p.compile / n, p.clear / n, p.copy / n, p.sync / n, p.present / n, p.swap / n, p.wait / n,
             (el - host) / n);
        LOGI("perf: title thread per frame: recording %.2f ms, waiting %.2f ms (%.1f waits: read-back %.2f "
             "resource %.2f fence %.2f flush %.2f copy %.2f, next frame %.2f) | read-backs waited for: %.1f lock, "
             "%.1f NOOVERWRITE",
             g_perf_capture_ns.exchange(0) / n / 1e6, g_perf_wait_ns.exchange(0) / n / 1e6,
             g_perf_waits.exchange(0) / n, g_perf_wait_kind_ns[0].exchange(0) / n / 1e6,
             g_perf_wait_kind_ns[1].exchange(0) / n / 1e6, g_perf_wait_kind_ns[2].exchange(0) / n / 1e6,
             g_perf_wait_kind_ns[3].exchange(0) / n / 1e6, g_perf_wait_kind_ns[4].exchange(0) / n / 1e6,
             g_perf_wait_kind_ns[5].exchange(0) / n / 1e6, g_perf_sync_block.exchange(0) / n,
             g_perf_sync_late.exchange(0) / n);
        LOGI("perf: per frame %.0f draws (%.0f raw) %.0fk verts %.1f MB upload %.1f overflow | %.0f checks %.2f MB "
             "hashed | %.1f tex uploads %.2f MB, %.1f new tex, %.1f new targets, %.1f target uploads, %.1f shrinks, "
             "%.1f read-backs %.1f late | %llu shaders %llu psos this second",
             p.draws / n, p.raw / n, p.verts / n / 1000.0, p.upload / n / 1048576.0, p.overflow / n, p.sigs / n,
             p.sig_bytes / n / 1048576.0, p.texups / n, p.texup_bytes / n / 1048576.0, p.newtex / n,
             p.targets / n, p.rtups / n, p.shrinks / n, p.syncs / n, p.lates / n,
             (unsigned long long)p.shaders, (unsigned long long)p.psos);
        g_perf = Perf{};
        perf_start = PerfClock::now();
        perf_frames = 0;
    }
}

void LogPbrPerf() {
    static PerfClock::time_point pbr_start = PerfClock::now();
    PbrPerf& q = g_pbr_perf;
    q.frames++;
    if (PerfMs(pbr_start) >= 10000.0) {
        const double n = double(q.frames);
        if (q.attempts)
            LOGR("perf: pbr per frame %.2f ms (mesh %.2f plan %.2f record %.2f other %.2f) for %.0f material draws, %.0f drawn",
                 q.total / n, q.mesh / n, q.plan / n, q.record / n, (q.total - q.mesh - q.plan - q.record) / n,
                 q.attempts / n, q.drawn / n);
        q = PbrPerf{};
        pbr_start = PerfClock::now();
    }
}

void D3D12Backend::Present(const Surface& front) {
    PerfScope perf_present{g_perf.present};
    if (cap_) {
        FinishCapture(front);
    } else {
        // F4 and XBE_CAPTURE_FRAME, the menu's delayed request, then the automatic mode (none while F1 is open)
        const double now = capture::Now();
        const bool menu = app::MenuOpen();
        const auto& c = app::Get().capture;
        const bool asked = g_capture.exchange(false) || diag::CaptureFrame(frame_ + 1) || capture::g_delayed.Take(now, menu);
        const bool automatic = auto_capture_.Due(now, c.automatic, c.interval, capture::g_status.Automatic(), c.limit, menu);
        if (asked || automatic) {
            cap_auto_ = !asked;
            if (automatic) auto_capture_.Taken(now);
            StartCapture();
        }
    }
    const auto photo = photo_policy_.FinishPresent();
    ResizeSwap();
    Target* t = EnsureTarget(front, false);
    if (t) app::ReportRenderResolution({t->w, t->h});
    if (t && LatchCpuWrite(*t)) UploadTarget(*t, front);
    if (t) Resolve(*t);
    if (ramp_dirty_) UploadGammaRamp();
    Transition(lut_.Get(), lut_state_, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    uint32_t bi = swap_->GetCurrentBackBufferIndex();
    DrawFrame(t, bi);
    if (photo.capture) CapturePhoto(photo, bi);
    photo_observed_ = {};
    Submit(false);
    present_fence_[presents_ % (kFrames + 1)].store(fence_value_);
    DeliverReports();
    bool vsync = app::Get().vsync;
    auto perf_swap = PerfClock::now();
    const HRESULT presented = swap_->Present(vsync ? 1 : 0, !vsync && tearing_ ? DXGI_PRESENT_ALLOW_TEARING : 0);
    const double present_ms = PerfMs(perf_swap);
    g_perf.swap += present_ms;
    if (debug_gpu_ && debug_budget_.Present()) {
        LOGR("d3d12 debug: Present frame %llu backbuffer %u result 0x%08lX elapsed %.3f ms fence %llu/%llu "
             "slot %llu SRVs %u samplers %u",
             static_cast<unsigned long long>(frame_), bi, static_cast<unsigned long>(presented), present_ms,
             static_cast<unsigned long long>(fence_->GetCompletedValue()), static_cast<unsigned long long>(fence_value_),
             static_cast<unsigned long long>(frame_ % kFrames), CurrentFrame().srv, smp_used_);
        DrainDebugMessages("initial Present");
        logx::Flush();
    }
    if (FAILED(presented)) { DrainDebugMessages("Present failure"); ReportDebugDred("Present failure"); }
    // Positive DXGI status values, including DXGI_STATUS_OCCLUDED, remain successful statuses.
    CheckGpu(dev_.Get(), presented, "presenting the swap chain");

    presents_++;
    presents_seen_.store(presents_);
    frame_++;
    ReleaseOldTextures();
    ResetFrame();
    CountFps();
    if (diag::Perf()) LogPerf();
    LogPbrPerf();
    UpdatePbrSettings();
    if (presents_ == 1 || presents_ % 30 == 0) PublishPbrTelemetry();
    ApplySettings();
}

void D3D12Backend::CompletePbrFrame(Frame& frame) {
    if (frame.pbr_owner == pbr_library_.library && frame.pbr_pending_draws.size() == pbr_observed_.size()) {
        for (size_t i = 0; i < frame.pbr_pending_draws.size(); ++i)
            pbr_observed_[i].rendered_draws += frame.pbr_pending_draws[i];
    }
    frame.pbr_pending_draws.clear();
    frame.pbr_owner.reset();
}

bool D3D12Backend::TryPbrDraw(const State& s,const Assembled& a,const DrawShaders& sh,const PixelKey& pk,
                            const PsoKey& key,const DrawTextures& textures,GuestExtent size,
                            ID3D12PipelineState* legacy_pso) {
    namespace pc = pbr::controls;
    namespace pp = pbr::pipeline;
    if (!pbr_settings_ || !pbr_settings_->settings().enabled || !pbr_library_.library) return false;
    const auto& library = *pbr_library_.library;
    const auto& settings = pbr_settings_->settings();
    // Candidates by the content hash of each bound stage's accepted colour, not a scan of every material.
    size_t candidates[16];
    size_t candidate_count = 0;
    for (uint32_t stage = 0; stage < 4; ++stage) {
        const Tex* bound = textures.accepted_color[stage];
        if (!bound || !bound->photo_identity.original_hash) continue;
        pbr::ForTexture(library, bound->photo_identity.original_hash, [&](uint32_t row) {
            if (library.materials[row].assignment.stage == stage && candidate_count < std::size(candidates))
                candidates[candidate_count++] = row;
        });
    }
    for (size_t candidate = 0; candidate < candidate_count; ++candidate) {
        const size_t row_index = candidates[candidate];
        const auto& material = library.materials[row_index];
        const auto& assignment = material.assignment;
        if (assignment.stage >= 4 || (assignment.vertex_shader != pbr::kAnyShader && assignment.vertex_shader != sh.vkey) ||
            (assignment.pixel_shader != pbr::kAnyShader && assignment.pixel_shader != sh.pkey)) continue;
        Tex* color = textures.accepted_color[assignment.stage];
        if (!color || color->photo_identity.original_hash != assignment.original_texture) continue;
        // "any" UV set: the draw's own coordinate index. The mesh is built for that one.
        pbr::Assignment mesh_assignment = assignment;
        if (assignment.uv_set == pbr::kAnyUv) {
            const uint32_t index = s.tss[assignment.stage][kTssTexCoordIndex];
            if (index >= 4) continue;
            mesh_assignment.uv_set = index;
        }
        auto& observed = pbr_observed_[row_index];
        ++observed.observed_draws;
        ++g_pbr_perf.attempts;
        // always timed, as mesh/plan/record are: PerfScope times only under XBE_PERF
        struct PbrTotal {
            PerfClock::time_point t = PerfClock::now();
            ~PbrTotal() { g_pbr_perf.total += PerfMs(t); }
        } perf_pbr;
        observed.reason = pc::Reason::UnsupportedDraw;
        // The catalog reason is coarse; the log names the exact check, once per change per material.
        const auto trace = [&](const std::string& why) {
            if (pbr_trace_.size() < pbr_observed_.size()) pbr_trace_.resize(pbr_observed_.size());
            // every distinct reason once per material: draws sharing a material may alternate between reasons
            auto& seen = pbr_trace_[row_index];
            if (seen.find("|" + why + "|") != std::string::npos) return;
            seen += "|" + why + "|";
            LOGR("pbr: material %zu texture %016llX: %s", row_index, (unsigned long long)assignment.original_texture,
                 why.c_str());
        };
        const auto& info = textures.info[assignment.stage];
        // Cutouts (trees, fences, sign edges): a GREATER/GEQUAL alpha test keeps its coverage through the same alpha,
        // optionally blended SRC_ALPHA/INV_SRC_ALPHA over what is behind; ONE/ZERO blending is opaque. Other tests and
        // blends (additive glows, decals) stay legacy.
        const uint32_t alpha_func=SimpleState(s,kMAlphaFunc),alpha_ref=SimpleState(s,kMAlphaRef)&0xFF;
        const bool alpha_test=SimpleState(s,kMAlphaTestEnable)&&Cmp(alpha_func)!=D3D12_COMPARISON_FUNC_ALWAYS;
        const bool cutout_test=alpha_test&&(Cmp(alpha_func)==D3D12_COMPARISON_FUNC_GREATER||
                                            Cmp(alpha_func)==D3D12_COMPARISON_FUNC_GREATER_EQUAL);
        const bool kept_blend=!key.blend||(key.op==kBlendOpAdd&&((key.src==kBlendSrcAlpha&&key.dst==kBlendInvSrcAlpha)||
                                                                   (key.src==kBlendOne&&key.dst==0)));
        // the legacy test passes round(a*255) > ref (>= ref): clip(a - cutoff) at the half step matches it exactly
        const float cutoff=!cutout_test?0.0f:std::clamp((float(alpha_ref)+
            (Cmp(alpha_func)==D3D12_COMPARISON_FUNC_GREATER?0.5f:-0.5f))/255.0f,0.0f,1.0f);
        // The facades' lit night windows: stage 1 drawn as the window layer over the stage-0 surface.
        const bool window_layer=pbr::WindowLayerSurface(s,pk,assignment.stage);
        // Original coverage, pass effects, fog and programmable deformation cannot be silently dropped.
        // Captures deliberately retain the original path until the evidence format also represents PBR draws.
        const char* gate =
            !bound_rt_ || bound_rt_->range != 1 ? "render target" : ext_ ? "renderer extension" : cap_ ? "capturing" :
            photo_policy_.Armed() ? "photo capture armed" : sh.prog ? "vertex program" : sh.quad ? "screen quad" :
            !sh.fixed_transform ? "not a fixed transform" : sh.car_paint ? "car paint" :
            a.pretransformed ? "pretransformed" : !(pbr::SimpleAlbedoSurface(s,pk,assignment.stage)||window_layer) ? "texture combiner" :
            a.topology != Topology::Triangles ? "not triangles" : !kept_blend ? "blended" : !key.zen ? "depth test off" :
            !key.zwrite ? "depth write off" :
            // an enabled ALWAYS test never discards (the world's state), exactly as the legacy alphatest()
            alpha_test && !cutout_test ? "alpha test" :
            pk.alphakill ? "alpha kill" : assignment.cutout ? "cutout" :
            assignment.surface != pbr::SurfaceClass::World ? "not a world surface" :
            textures.rt[assignment.stage] ? "texture is a render target" : info.kind != kTex2D ? "not a 2D texture" :
            info.rect ? "rect texture" : info.shadow ? "shadow texture" : color->uploads > 4 ? "dynamic texture" :
            textures.role[assignment.stage] == kRoleNeon ? "neon" : nullptr;
        if (gate) {
            std::string why = std::string("draw gate: ") + gate;
            if (gate == std::string_view("texture combiner")) {
                // the exact key the combiner check saw: stage 0/1 colour and alpha ops/args/result, stage 2 op
                char detail[384];
                const auto& f0 = pk.ff[0]; const auto& f1 = pk.ff[1];
                const auto& f2 = pk.ff[2];
                snprintf(detail, sizeof(detail), " (s0 %u %u %u|%u %u %u r%u, s1 %u %u %u|%u %u %u r%u, s2 %u %u %u|%u %u %u r%u, "
                         "tci %u/%u addr %u%u/%u%u, blend %u, "
                         "tex %u/%u/%u/%u flags %u, flat %u two %u proj %u quad %u prog %u kill %u)",
                         f0[0], f0[1], f0[2], f0[4], f0[5], f0[6], f0[8], f1[0], f1[1], f1[2], f1[4], f1[5], f1[6], f1[8],
                         f2[0], f2[1], f2[2], f2[4], f2[5], f2[6], f2[8], s.tss[0][kTssTexCoordIndex], s.tss[1][kTssTexCoordIndex],
                         s.tss[0][kTssAddressU], s.tss[0][kTssAddressV], s.tss[1][kTssAddressU], s.tss[1][kTssAddressV], key.blend,
                         pk.tex[0].kind, pk.tex[1].kind, pk.tex[2].kind, pk.tex[3].kind, pk.tex[0].flags,
                         pk.flat, pk.two_sided, pk.proj, pk.quad, pk.programmable, pk.alphakill);
                why += detail;
            } else if (gate == std::string_view("blended") || gate == std::string_view("alpha test")) {
                char detail[128];
                snprintf(detail, sizeof(detail), " (blend %u src %u dst %u op %u, alpha func %u ref %u, zwrite %u)",
                         key.blend, key.src, key.dst, key.op, SimpleState(s, kMAlphaFunc), SimpleState(s, kMAlphaRef) & 0xFF,
                         key.zwrite);
                why += detail;
            }
            trace(why);
            continue;
        }
        const auto* override = pbr_settings_->Find(assignment,library.manifest_sha256);
        if (override && !override->enabled) { observed.reason=pc::Reason::MaterialDisabled; continue; }
        auto parameters=override&&!override->use_global_parameters?override->parameters:settings.parameters;
        pc::Sanitize(parameters);
        // A height effect needs this material's height map: without one it draws with none, as the menu says.
        // True displacement needs culling/depth/budget proofs this backend never establishes, so it is parallax.
        if (parameters.height_mode!=pc::HeightMode::None&&!material.has_height) parameters.height_mode=pc::HeightMode::None;
        if (parameters.height_mode==pc::HeightMode::Displacement) parameters.height_mode=pc::HeightMode::Parallax;
        pbr::fixed_mesh::RasterTarget target{bound_rt_->w,bound_rt_->h,size.w,size.h};
        const uint64_t index_bytes=uint64_t(a.index_count)*(a.index16?2:4);
        if (index_bytes > pp::kMaxMeshIndices*4ull || (index_bytes && !a.index_src)) continue;
        // Fixed-function API semantics establish registers0/2/3/9+uv as object position/authored normal/
        // diffuse/UV. World streams have no register 2: those shade with the triangle's own plane. LIGHTING
        // off forwards register 3 to oD0, which the baked mode carries. The bridge independently checks the
        // actual declaration/FVF, layout, decoding and same-draw spans.
        const bool lit=s.rs[kRsLighting]!=0,authored_normals=((a.regs>>2)&1)!=0;
        // only the baked mode writes the original's alpha (texture times cascade); a lit draw writes the texture's
        if ((cutout_test||key.blend) && lit) { trace("lit draw with alpha coverage or blending"); continue; }
        pbr::fixed_mesh::FixedProof fixed;
        fixed.kind=pbr::fixed_mesh::DrawKind::ObjectSpaceFixedFunction;
        fixed.authored_normals=authored_normals;fixed.face_normals=!authored_normals;fixed.assigned_uv=true;
        fixed.vertex_diffuse=!lit;
        // SimpleAlbedoSurface admitted stage 1 only as a CURRENT*TFACTOR tint; the baked mode folds the factor in.
        if (window_layer && lit) { trace("lit draw with a window layer"); continue; }
        if (window_layer) {
            const uint32_t layer_uv=s.tss[1][kTssTexCoordIndex];
            Tex* layer=textures.accepted_color[1];
            if (layer_uv>=4 || !layer || !layer->res) { trace("window layer without a usable stage-1 texture"); continue; }
            fixed.layer_uv=int32_t(layer_uv);
        }
        const bool factor_stage=!window_layer&&pk.ff[1][0]>1;
        if (factor_stage && lit) { trace("lit draw with a texture factor stage"); continue; }
        if (factor_stage) {
            const uint32_t f=s.rs[kRsTextureFactor];
            const bool factor_alpha=pk.ff[1][4]==4; // TextureFactorStage: MODULATE CURRENT*TFACTOR, else keeps CURRENT
            fixed.diffuse_factor={float((f>>16)&255)/255.0f,float((f>>8)&255)/255.0f,float(f&255)/255.0f,
                                  factor_alpha?float(f>>24)/255.0f:1.0f};
        }
        // SimpleAlbedoSurface: stage-0 alpha is the texture's, or texture*diffuse
        fixed.alpha_from_diffuse=pk.ff[0][4]==4;
        observed.reason=pc::Reason::MissingGeometry;
        auto perf_mesh_t=PerfClock::now();
        // Static scenery repeats its exact bytes every frame: build each mesh once, then refresh it.
        uint64_t mesh_key=MixHash(&row_index,sizeof(row_index),sh.vkey^(uint64_t(mesh_assignment.uv_set)<<56));
        {
            const uint64_t words[]={a.vertex_count,a.index_count,a.draw_count,uint64_t(a.index16),uint64_t(int64_t(a.base_vertex)),
                uint64_t(a.strip),uint64_t(a.topology),uint64_t(a.regs),a.decode,a.buffer_count,uint64_t(a.pretransformed),
                s.vs_handle,uint64_t(fixed.authored_normals),uint64_t(fixed.vertex_diffuse),uint64_t(fixed.alpha_from_diffuse),
                std::bit_cast<uint32_t>(fixed.diffuse_factor[0]),std::bit_cast<uint32_t>(fixed.diffuse_factor[1]),
                std::bit_cast<uint32_t>(fixed.diffuse_factor[2]),std::bit_cast<uint32_t>(fixed.diffuse_factor[3]),
                uint64_t(int64_t(fixed.layer_uv))};
            mesh_key=MixHash(words,sizeof(words),mesh_key);
            mesh_key=MixHash(a.elements.data(),sizeof(a.elements),mesh_key);
            for (uint32_t i=0;i<a.buffer_count&&i<a.buffers.size();++i) {
                const auto& b=a.buffers[i];const uint32_t layout[2]={b.bytes,b.stride};
                mesh_key=MixHash(layout,sizeof(layout),mesh_key);
                if (b.src) mesh_key=MixHash(b.src,b.bytes,mesh_key);
            }
            if (index_bytes) mesh_key=MixHash(a.index_src,size_t(index_bytes),mesh_key);
            if (s.vs) {
                mesh_key=MixHash(s.vs->inputs.data(),sizeof(s.vs->inputs),mesh_key);
                mesh_key=MixHash(s.vs->stream_stride.data(),sizeof(s.vs->stream_stride),mesh_key);
            }
            for (const auto& stream:s.streams){const uint32_t stride=stream.stride;mesh_key=MixHash(&stride,sizeof(stride),mesh_key);}
        }
        auto cached=pbr_mesh_cache_.find(mesh_key);
        pbr::fixed_mesh::Status mesh_status;std::string mesh_detail;
        if (cached!=pbr_mesh_cache_.end()&&cached->second.status!=pbr::fixed_mesh::Status::Ready) {
            mesh_status=cached->second.status;mesh_detail=cached->second.detail;
        } else if (cached!=pbr_mesh_cache_.end()) {
            mesh_status=pbr::fixed_mesh::Refresh(s,a,mesh_assignment,fixed,target,cached->second.mesh);
            if (mesh_status!=pbr::fixed_mesh::Status::Ready) {pbr_mesh_cache_.erase(cached);cached=pbr_mesh_cache_.end();}
        } else {
            PbrMeshEntry entry;
            mesh_status=entry.status=pbr::fixed_mesh::Build(s,a,mesh_assignment,fixed,target,
                {static_cast<const uint8_t*>(a.index_src),size_t(index_bytes)},entry.mesh,{},&entry.detail);
            mesh_detail=entry.detail;
            // Failures that depend only on the bytes are kept; state/transform ones are retried every draw.
            using MS=pbr::fixed_mesh::Status;
            const bool keep=mesh_status==MS::Ready||mesh_status==MS::InvalidMesh||mesh_status==MS::InvalidIndex||
                            mesh_status==MS::InvalidBuffer||mesh_status==MS::UnsupportedFormat||mesh_status==MS::MissingInput;
            if (keep) {
                if (pbr_mesh_cache_.size()>=kPbrMeshCacheEntries) pbr_mesh_cache_.clear();
                cached=pbr_mesh_cache_.emplace(mesh_key,std::move(entry)).first;
            }
        }
        g_pbr_perf.mesh+=PerfMs(perf_mesh_t);
        if (const auto st=mesh_status; st!=pbr::fixed_mesh::Status::Ready) {
            trace(std::string("mesh: ")+pbr::fixed_mesh::StatusText(st)+(mesh_detail.empty()?"":" ("+mesh_detail+")"));continue;
        }
        const auto& mesh=cached->second.mesh;
        pp::SceneConstants scene;
        observed.reason=pc::Reason::MissingLinearLighting;
        if (!lit) {
            pbr::scene::BakedProof baked;
            baked.immutable_draw_proven=true; // This State/assembly/stream snapshot, consumed above in one call.
            baked.fixed_world_source_proven=sh.fixed_transform&&!sh.prog;
            baked.vertex_diffuse_stream_proven=true; // Build bound and decoded register 3.
            baked.vertex_diffuse_forwarded_proven=true; // FixedVertexHlsl LIGHTING off: oD0 = v3.
            baked.vertex_specular_zero_proven=((a.regs>>4)&1)==0; // ... and oD1 = v4, absent = 0.
            baked.albedo_diffuse_combiner_proven=true; // SimpleAlbedoSurface above; Build rechecks the cascade.
            baked.vertex_diffuse_alpha_one_proven=mesh.vertex_diffuse_alpha_one;
            baked.texture_factor_applied=factor_stage; // The packed mesh colours already carry the factor.
            baked.combiner_alpha_packed=true; // ... and the cascade's alpha factors, written times albedo alpha.
            baked.rigid_view_proven=mesh.height_units_proven;
            baked.perspective_camera_proven=true; // Build rechecks the SAME actual projection numerically.
            baked.opaque_no_cutout_proven=true;
            baked.coverage_kept=cutout_test||key.blend; // the clip and blend state below are the draw's own
            baked.window_layer_kept=window_layer; // stage 1 is added by the shader below (options[3], game_layer)
            baked.coefficient_colors_linear=true; // Declared policy; the shader decodes like the albedo.
            baked.output_policy_proven=bound_rt_->fmt==kBgra;
            baked.fog_reproduced=true; // Same fogfactor/fog colour/order as the legacy fixed pipeline.
            if (const auto st=pbr::scene::BuildBakedVertexDiffuse(s,baked,pp::OutputPolicy::ToneMappedSrgb,scene);
                st!=pbr::scene::Status::Ready) {
                std::string why=std::string("baked scene: ")+pbr::scene::StatusText(st);
                if (st==pbr::scene::Status::UnsupportedBakedCombiner) {
                    char detail[192];const auto& t0=s.tss[0];const auto& t1=s.tss[1];
                    snprintf(detail,sizeof(detail)," (s0 alpha %u %u %u, s1 %u %u %u|%u %u %u r%u, s2 %u arg %u tex1 %d tex2 %d, tfactor %08X, vertex alpha one %d)",
                             t0[kTssAlphaOp],t0[kTssAlphaArg1],t0[kTssAlphaArg2],t1[kTssColorOp],t1[kTssColorArg1],t1[kTssColorArg2],
                             t1[kTssAlphaOp],t1[kTssAlphaArg1],t1[kTssAlphaArg2],t1[kTssResultArg],s.tss[2][kTssColorOp],
                             s.tss[2][kTssColorArg1],s.textures[1]!=0,s.textures[2]!=0,s.rs[kRsTextureFactor],int(mesh.vertex_diffuse_alpha_one));
                    why+=detail;
                }
                trace(why);continue;
            }
        } else {
            const auto lights=s.pbr_light_proof.Snapshot(s.light_enable,true);
            pbr::scene::Proof scene_proof;
            scene_proof.light_writes_after_success=lights.light_writes_after_success;
            scene_proof.light_index_coverage=lights.light_index_coverage;
            scene_proof.stateblock_valid=lights.stateblock_valid&&s.pbr_active_light_records_verified;
            scene_proof.fixed_function_lighting_proven=sh.fixed_transform&&s.rs[kRsLighting];
            scene_proof.rigid_view_proven=mesh.height_units_proven;
            scene_proof.perspective_camera_proven=true; // Build rechecks the SAME actual projection numerically.
            scene_proof.opaque_no_cutout_proven=true; // Exact original states checked above.
            // Explicit normalized-linear compatibility interpretation of actual D3DLIGHT8 coefficients.
            // It is not calibrated lumens/metres, inferred image lighting, or a claim of a full HDR framebuffer.
            scene_proof.api_colors_linear=true;
            scene_proof.output_policy_proven=bound_rt_->fmt==kBgra;
            scene_proof.fog_reproduced=true;
            if (const auto st=pbr::scene::Build(s,scene_proof,pp::OutputPolicy::ToneMappedSrgb,scene);
                st!=pbr::scene::Status::Ready) {trace(std::string("lit scene: ")+pbr::scene::StatusText(st));continue;}
        }
        pbr::SurfaceEvidence surface;
        surface.assignment=assignment;surface.color_width=color->pbr_color.width;surface.color_height=color->pbr_color.height;
        surface.current_alpha_sha256=color->pbr_color.alpha_sha256;
        surface.texture_2d=true;surface.geometric_position=true;surface.geometric_normal=true;
        surface.tangent_basis=true;surface.linear_lighting=true;
        observed.reason=pc::Reason::UnsupportedDraw;
        // Initial slice preserves target alpha at EVERY accepted mip/filter sample. Level-zero alpha
        // equality alone cannot prove arbitrary authored mip coverage; those materials stay legacy.
        // ... unless no texture alpha can reach the target: blending is off, the alpha test is ALWAYS (gates above)
        // and the baked cascade multiplies the written alpha by a zero TFACTOR alpha (the world's state).
        const bool alpha_unused=!lit&&fixed.diffuse_factor[3]==0;
        // A game albedo is the original texture itself: its coverage is the original's at every mip.
        if (!material.game_albedo&&!color->pbr_color.constant_alpha&&!alpha_unused) {trace("texture alpha is not constant at every mip");continue;}
        observed.reason=pc::Reason::AlphaMismatch;
        if (pbr::Select(library,surface).reason!=pbr::SelectionReason::Ready) {trace("selection: size or alpha differs from the pack");continue;}
        pp::MaterialConstants factors;
        if (const auto st=pp::BuildMaterial(parameters,material.normal_convention,material.has_height,material.has_emission,cutout_test,cutoff,factors);
            st!=pp::Status::Ready) {trace(std::string("material constants: ")+pp::StatusText(st));continue;}
        pp::Evidence evidence;
        // This is an intended draw plan; GPU success/readiness is never inferred from these CPU facts.
        evidence.enabled=evidence.approved_assignment=true;
        evidence.texture=evidence.expected_texture=assignment.original_texture;
        evidence.vertex_shader=evidence.expected_vertex_shader=assignment.vertex_shader;
        evidence.pixel_shader=evidence.expected_pixel_shader=assignment.pixel_shader;
        evidence.fixed_transform=evidence.triangle_list=evidence.opaque=true;
        evidence.packed_mesh_validated=evidence.original_normal_uv_proven=evidence.affine_bridge_proven=true;
        evidence.view_projection_proven=evidence.linear_lights_proven=evidence.output_policy_proven=true;
        evidence.all_five_bindings=true; // Device preflights and binds all five exact/neutral resources atomically.
        evidence.height_map=material.has_height;evidence.height_units_proven=mesh.height_units_proven;
        // No culling/depth/multipass/edge proofs are implied by a valid normal/UV triangle mesh.
        // True displacement stays ineligible until those separate source-backed obligations are met.
        evidence.shared_edges_and_uv_continuity_proven=mesh.shared_edges_and_uv_continuity_proven;
        evidence.displacement_bounds_and_culling_proven=mesh.displacement_bounds_and_culling_proven;
        evidence.displacement_depth_and_multipass_proven=mesh.displacement_depth_and_multipass_proven;
        evidence.displacement_budget_proven=mesh.displacement_budget_proven;
        pbr::device::Input input;
        input.library=pbr_library_.library;input.key=observed.key;input.surface=std::move(surface);input.evidence=evidence;
        input.vertices=mesh.vertices;input.indices=mesh.indices;input.geometry=mesh.geometry;input.material=factors;input.scene=scene;
        if (material.game_albedo) {input.game_albedo=color->res.Get();input.geometry.options[2]=1;}
        if (window_layer) {
            // stage 1's texture times TFACTOR, as the cascade multiplies it (display values, added after encoding)
            const uint32_t f=s.rs[kRsTextureFactor];
            input.material.emission_color={float((f>>16)&255)/255.0f,float((f>>8)&255)/255.0f,float(f&255)/255.0f,0};
            input.game_layer=textures.accepted_color[1]->res.Get();input.geometry.options[3]=1;
        }
        if (g_reversed_depth) {
            DrawConstants depth{};
            FillDepthConstants(s,sh,key,depth);
            input.geometry.depth={depth.zparams[0],depth.zparams[1],depth.zparams[2],1};
        }
        FillPipelineDesc(key,sh.vs_code,sh.ps_code,input.original_opaque_state);
        const auto& graphics=app::Get();
        input.actual_sampler=StageSampler(s,int(assignment.stage),textures,UnitTexcoords(a),graphics.aniso,graphics.graphics.texture_detail);
        input.actual_sampler.MaxAnisotropy=std::max(1u,input.actual_sampler.MaxAnisotropy);
        input.actual_sampler.ComparisonFunc=D3D12_COMPARISON_FUNC_ALWAYS;
        input.coverage_policy_proven=true;input.actual_alpha_cutoff=cutoff;input.draw_alpha_test=cutout_test;
        pbr::device::Plan plan;
        observed.reason=parameters.height_mode==pc::HeightMode::Displacement?pc::Reason::DisplacementUnavailable:pc::Reason::RendererBindingsUnavailable;
        auto perf_plan_t=PerfClock::now();
        const auto plan_status=pbr::device::PlanDraw(input,{},plan);
        g_pbr_perf.plan+=PerfMs(perf_plan_t);
        if (const auto st=plan_status; st!=pbr::device::Status::Planned) {
            trace(std::string("plan: ")+pbr::device::StatusText(st));continue;
        }
        Frame& frame=CurrentFrame();
        if (!frame.pbr_owner) {frame.pbr_pending_draws.assign(pbr_observed_.size(),0);frame.pbr_owner=input.library;}
        if (frame.pbr_owner!=input.library) continue;
        pbr::device::Context context;
        context.device=dev_.Get();context.list=cl_.Get();context.queue_identity=queue_.Get();
        context.allocate_upload=[this](uint64_t bytes,uint64_t alignment){
            auto a=Upload(bytes,alignment);return pbr::device::UploadAllocation{a.cpu,a.gpu,a.res,a.off,bytes};
        };
        context.retain=[&frame](std::span<IUnknown* const> objects){
            frame.pbr_retained.reserve(frame.pbr_retained.size()+objects.size());
            for(IUnknown* object:objects){if(!object)return false;frame.pbr_retained.emplace_back(object);}return true;
        };
        SetViewportScissor(s,size.w);SetBlendFactorStencilRef(s);
        const uint32_t query=BeginCountedDraw(s);
        auto perf_record_t=PerfClock::now();
        const auto result=pbr_device_.RecordDraw(context,input,plan);
        g_pbr_perf.record+=PerfMs(perf_record_t);
        EndCountedDraw(query);
        binds_ = {}; // the dedicated path sets its own pipeline, root bindings and topology
        if (result.legacy_bindings_invalidated) {
            ID3D12DescriptorHeap* heaps[]={srv_heap_.Get(),smp_heap_.Get()};
            cl_->SetDescriptorHeaps(2,heaps);cl_->SetGraphicsRootSignature(root_.Get());cl_->SetPipelineState(legacy_pso);
            // On fallback the following legacy UploadConstants/texture/view/geometry calls rebind every root/IA slot.
            // On success the next Draw does the same. No legacy binding cache is trusted across this change.
        }
        if(result.draw_call_recorded&&result.status!=pbr::device::Status::Recorded) {
            CheckGpu(dev_.Get(),FAILED(result.error)?result.error:E_FAIL,"recording the dedicated PBR draw");
        }
        if(result.status!=pbr::device::Status::Recorded||!result.draw_call_recorded) {
            trace(std::string("record: ")+pbr::device::StatusText(result.status));continue;
        }
        trace("drawing through the dedicated path");
        ++g_pbr_perf.drawn;
        ++frame.pbr_pending_draws[row_index];
        observed.reason=pc::Reason::Ready;
        pbr_capabilities_={.pipeline=true,.geometry=true,.view=true,.lighting=true,.linear_output=true,.material_bindings=true,
                           .parallax=true,.height_units=mesh.height_units_proven};
        return true;
    }
    return false;
}

void D3D12Backend::UpdatePbrSettings() {
    namespace pc = pbr::controls;
    {
        std::lock_guard lock(app::UiLock());
        const auto& current = app::Get().pbr;
        if (!pbr_settings_ || !pbr::SameSettings(pbr_settings_->settings(), current))
            pbr_settings_ = std::make_unique<pc::PreparedSettings>(current);
    }
    const auto& settings = pbr_settings_->settings();
    std::filesystem::path root;
    if (settings.enabled && pc::ValidPackPath(settings.pack_path) && pc::ValidDigest(settings.approved_manifest_sha256)) {
        root = hostpath::FromUtf8(settings.pack_path);
        if (root.is_relative()) root = app::ExeDir() / root;
    }
    // One CPU-only loader, no filename scans/automatic approval or parallel GPU workers.
    pbr_library_cache_.Request(settings.enabled, root, settings.approved_manifest_sha256);
    auto next = pbr_library_cache_.Snapshot();
    if (next.library != pbr_library_.library) {
        if (next.library) {
            // Changing the immutable material owner must never recycle descriptors/maps still in flight.
            Flush();
            pbr_device_.ResetGpuIdle();
            ++pbr_proof_epoch_;
        }
        pbr_observed_.clear();
        pbr_trace_.clear();
        pbr_mesh_cache_.clear();
        pbr_capabilities_ = {};
        if (next.library) for (const auto& m : next.library->materials) {
            pc::CatalogMaterial row;
            row.key = pc::ExactKey(m.assignment, next.library->manifest_sha256);
            row.has_height = m.has_height; row.has_emission = m.has_emission;
            row.reason = pc::Reason::RendererBindingsUnavailable;
            pbr_observed_.push_back(std::move(row));
        }
    }
    pbr_library_ = std::move(next);
    pbr::observation::RequestGuestProof(settings.enabled && pbr_library_.library != nullptr);
}

void D3D12Backend::PublishPbrTelemetry() {
    namespace pc = pbr::controls;
    pc::CatalogSnapshot snap;
    snap.revision = presents_;
    snap.message = pbr_library_.message;
    switch (pbr_library_.status) {
    case pbr::CacheStatus::Disabled: snap.status = pc::RuntimeStatus::Disabled; break;
    case pbr::CacheStatus::WaitingForApproval: snap.status = pc::RuntimeStatus::WaitingForPack; break;
    case pbr::CacheStatus::Loading: snap.status = pc::RuntimeStatus::Loading; break;
    case pbr::CacheStatus::Rejected: snap.status = pc::RuntimeStatus::PackRejected; break;
    case pbr::CacheStatus::Ready:
        snap.status = pc::RuntimeStatus::BindingsUnavailable;
        snap.message = "Approved materials loaded; unsupported or unproved draws retain original rendering.";
        if (pbr_library_.library) snap.manifest_sha256 = pbr_library_.library->manifest_sha256;
        snap.materials = pbr_observed_;
        snap.capabilities = pbr_capabilities_;
        for (const auto& row : snap.materials) if (row.rendered_draws && row.reason == pc::Reason::Ready) {
            snap.status = pc::RuntimeStatus::Ready; break;
        }
        break;
    }
    pc::PublishCatalog(std::move(snap));
}
// the slot is not reused before the caller's wait: the title runs at most one present ahead
void D3D12Backend::WaitGpuFrame(uint64_t n) {
    uint64_t fence = present_fence_[n % (kFrames + 1)].load();
    if (fence > fence_->GetCompletedValue()) fence_->SetEventOnCompletion(fence, nullptr);
}

void D3D12Backend::ApplySettings() {
    if (texturedumper::Update()) ReleasePackTextures();
    std::lock_guard lock(app::UiLock());
    app::Mode scene = app::RenderResolution();
    float want = app::EnhancedRenderScale(guest_h_);
    bool remake = ext_ && ext_->Settings();
    if (scene != scene_ || std::fabs(want - scale_) > 1e-4f || remake) {
        Flush();
        for (auto& [key, tg] : targets_) (tg.depth ? free_dsv_ : free_rtv_).push_back(tg.view);
        targets_.clear();
        scale_ = want;
        scene_ = scene;
        allocation_failed_ = false;
        post_failed_ = false;
        app::ReportRenderResolution({});
        if (ext_) ext_->Reset(*this);
        bound_rt_ = bound_ds_ = nullptr;
        LOGR("d3d12: native scene selection %ux%u (UI %ux%u)", scene_.width, scene_.height, bb_w_, bb_h_);
    }
}

// the target resolved, then shrunk with o when it is not guest sized BGRA; res null on failure
D3D12Backend::CopySource D3D12Backend::PrepareCopySource(Target& t, const ShrinkOptions& o) {
    Resolve(t);
    CopySource out{t.res.Get(), nullptr};
    if (t.w != t.gw || t.h != t.gh || t.fmt != kBgra) {
        out.down = Shrink(t.res.Get(), t.state, t.fmt, t.gw, t.gh, o);
        out.res = out.down.Get();
    } else {
        Transition(t.res.Get(), t.state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    }
    return out;
}

// long unused: the title freed it and other data is at this address now
bool D3D12Backend::TargetStale(uint32_t data) const {
    auto it = target_used_.find(data);
    uint64_t used = it == target_used_.end() ? 0 : it->second;
    return frame_ - used > kStaleFrames;
}

// the CPU reads a surface the GPU drew: bring the pixels back
void D3D12Backend::Sync(uint32_t data, uint32_t) {
    auto it = targets_.find(TargetMapKey(data, false));
    if (it == targets_.end() || !it->second.drawn || TargetStale(data)) return;
    PerfScope perf_sync{g_perf.sync};
    g_perf.syncs++;
    if (diag::Perf()) {
        LOG_FIRST(16, "perf: read-back of %08X (%ux%u guest, %ux%u host)", data, it->second.gw, it->second.gh,
                  it->second.w, it->second.h);
    }
    Target& t = it->second;
    // src.down, when there is one, stays alive until ReadBack is done
    CopySource src = PrepareCopySource(t, {});
    if (!src.res) return;
    std::vector<uint8_t> px;
    if (!ReadBack(src.res, t.gw, t.gh, px)) return;
    // color targets are 32 bit linear in guest memory, the pitch rounds to 64 bytes
    uint32_t pitch = GuestPitch(t.gw);
    uint8_t* guest = static_cast<uint8_t*>(kernel::HostPtr(data));
    for (uint32_t y = 0; y < t.gh; y++) std::memcpy(guest + size_t(y) * pitch, &px[size_t(y) * t.gw * 4], t.gw * 4);
    t.read = std::move(px);
    t.drawn = false;
    t.sig = MemSig(t.data, t.bytes);
}

// A NOOVERWRITE lock: the console reads whatever the GPU has finished, so a surface locked every frame gets the newest
// finished copy (a frame or two old) and the CPU never waits on the GPU; one locked now and then still waits in Sync
void D3D12Backend::SyncLate(uint32_t data) {
    if (!DeliverLate(data)) Sync(data, 0);
    RecordLate(data);
}

bool D3D12Backend::DeliverLate(uint32_t data) {
    std::lock_guard lock(late_lock_);
    auto it = late_.find(data);
    if (it == late_.end()) return false;
    Late& l = it->second;
    uint64_t done = fence_->GetCompletedValue();
    uint64_t now = presents_seen_.load();
    int best = -1;
    for (int i = 0; i < kFrames; i++) {
        bool finished = l.fence[i] && l.fence[i] <= done;
        bool recent = now - l.frame[i] <= uint64_t(kFrames) + 1;
        bool newest = best < 0 || l.fence[i] > l.fence[best];
        if (finished && recent && newest) best = i;
    }
    if (best < 0) {
        if (diag::Perf()) {
            LOG_FIRST(16, "perf: late %08X nothing finished: done %llu now %llu, slots %llu/%llu %llu/%llu %llu/%llu",
                      data, (unsigned long long)done, (unsigned long long)now, (unsigned long long)l.fence[0],
                      (unsigned long long)l.frame[0], (unsigned long long)l.fence[1], (unsigned long long)l.frame[1],
                      (unsigned long long)l.fence[2], (unsigned long long)l.frame[2]);
        }
        return false;
    }
    if (l.fence[best] > l.delivered) {
        uint8_t* p = nullptr;
        // the last row is not padded to the pitch: a range to the end of the pitch fails to map
        D3D12_RANGE rr{0, size_t(l.row_pitch) * (l.gh - 1) + l.gw * 4};
        if (FAILED(l.rb[best]->Map(0, &rr, reinterpret_cast<void**>(&p)))) return false;
        uint32_t pitch = GuestPitch(l.gw);
        uint8_t* dst = static_cast<uint8_t*>(kernel::HostPtr(data));
        for (uint32_t y = 0; y < l.gh; y++) std::memcpy(dst + size_t(y) * pitch, p + size_t(y) * l.row_pitch, l.gw * 4);
        D3D12_RANGE none{0, 0};
        l.rb[best]->Unmap(0, &none);
        l.delivered = l.fence[best];
    }
    return true;
}

// a slot set for another size: its buffers go with the frame
void D3D12Backend::ResetLate(Late& l, const Target& t) {
    auto& garbage = CurrentFrame().garbage;
    for (auto& r : l.rb) {
        if (r) garbage.push_back(r);
    }
    if (l.down) garbage.push_back(l.down);
    l = Late{};
    l.gw = t.gw;
    l.gh = t.gh;
}

// the target as a guest sized BGRA copy source, shrunk into l.down when it is not one already
ID3D12Resource* D3D12Backend::PrepareLateSource(Target& t, Late& l) {
    ShrinkOptions reuse{.into = l.down.Get(), .into_state = l.down_state};
    CopySource src = PrepareCopySource(t, reuse);
    if (src.down) {
        l.down = src.down;
        l.down_state = D3D12_RESOURCE_STATE_COPY_SOURCE;
    }
    return src.res;
}

// src into the readback buffer of the slot, made on first use
void D3D12Backend::RecordLateCopy(Late& l, uint32_t slot, ID3D12Resource* src) {
    D3D12_RESOURCE_DESC rd = src->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;
    UINT rows;
    UINT64 row_size;
    UINT64 bytes;
    dev_->GetCopyableFootprints(&rd, 0, 1, 0, &fp, &rows, &row_size, &bytes);
    if (!l.rb[slot]) {
        D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_READBACK};
        D3D12_RESOURCE_DESC bd = BufferDesc(bytes);
        if (FAILED(dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST,
                                                 nullptr, IID_PPV_ARGS(&l.rb[slot])))) {
            return;
        }
    }
    D3D12_TEXTURE_COPY_LOCATION dl{l.rb[slot].Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    dl.PlacedFootprint = fp;
    D3D12_TEXTURE_COPY_LOCATION sl{src, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    cl_->CopyTextureRegion(&dl, 0, 0, 0, &sl, nullptr);
    l.row_pitch = fp.Footprint.RowPitch;
    l.fence[slot] = fence_value_ + 1;  // the next Submit carries it
    l.frame[slot] = presents_;
    l.next = (slot + 1) % kFrames;
}

void D3D12Backend::RecordLate(uint32_t data) {
    auto it = targets_.find(TargetMapKey(data, false));
    if (it == targets_.end() || TargetStale(data)) return;
    Target& t = it->second;
    PerfScope perf_sync{g_perf.sync};
    g_perf.lates++;
    // what DeliverLate or Sync left in guest memory is no CPU write to upload back
    t.sig = MemSig(t.data, t.bytes);
    t.read.clear();
    std::lock_guard lock(late_lock_);
    Late& l = late_[data];
    if (l.gw != t.gw || l.gh != t.gh) {
        if (diag::Perf()) LOG_FIRST(16, "perf: late %08X ring reset, %ux%u -> %ux%u", data, l.gw, l.gh, t.gw, t.gh);
        ResetLate(l, t);
    }
    // a copy for a later lock, unless the GPU is a whole ring behind
    uint32_t slot = l.next;
    if (l.fence[slot] > fence_->GetCompletedValue()) return;
    ID3D12Resource* src = PrepareLateSource(t, l);
    if (!src) return;
    RecordLateCopy(l, slot, src);
}

// filters src into a new w x h target, left as a copy source
ComPtr<ID3D12Resource> D3D12Backend::Shrink(ID3D12Resource* src, D3D12_RESOURCE_STATES& st, DXGI_FORMAT fmt,
                                            uint32_t w, uint32_t h, const ShrinkOptions& o) {
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC td = Texture2DDesc(DXGI_FORMAT_B8G8R8A8_UNORM, w, h);
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE cv{};
    cv.Format = td.Format;
    ComPtr<ID3D12Resource> down = o.into;
    D3D12_RESOURCE_STATES down_state = o.into ? o.into_state : D3D12_RESOURCE_STATE_RENDER_TARGET;
    g_perf.shrinks++;
    if (free_rtv_.empty()) return nullptr;
    if (down) {
        Transition(down.Get(), down_state, D3D12_RESOURCE_STATE_RENDER_TARGET);
    } else if (FAILED(dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td,
                                                    D3D12_RESOURCE_STATE_RENDER_TARGET, &cv, IID_PPV_ARGS(&down)))) {
        return nullptr;
    }
    // borrows the next free view without taking it, only for this recording
    D3D12_CPU_DESCRIPTOR_HANDLE rh = Cpu(rtv_heap_.Get(), free_rtv_.back(), rtv_inc_);
    dev_->CreateRenderTargetView(down.Get(), nullptr, rh);
    Transition(src, st, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    SetBlitTarget(rh, w, h);
    float uv[4] = {0, 0, 1, 1};
    Blit(src, fmt, uv, {.alpha = true, .taps = o.taps, .mul = o.mul});
    Transition(down.Get(), down_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    return down;
}

// a BGRA copy source to the CPU, rows packed; waits for the GPU
bool D3D12Backend::ReadBack(ID3D12Resource* res, uint32_t w, uint32_t h, std::vector<uint8_t>& out) {
    D3D12_RESOURCE_DESC rd = res->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;
    UINT rows;
    UINT64 row_size;
    UINT64 bytes;
    dev_->GetCopyableFootprints(&rd, 0, 1, 0, &fp, &rows, &row_size, &bytes);
    ComPtr<ID3D12Resource> rb;
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_READBACK};
    D3D12_RESOURCE_DESC bd = BufferDesc(bytes);
    if (FAILED(dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                             IID_PPV_ARGS(&rb)))) {
        return false;
    }
    D3D12_TEXTURE_COPY_LOCATION dl{rb.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    dl.PlacedFootprint = fp;
    D3D12_TEXTURE_COPY_LOCATION sl{res, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    cl_->CopyTextureRegion(&dl, 0, 0, 0, &sl, nullptr);
    Flush();
    uint8_t* p = nullptr;
    const HRESULT mapped = rb->Map(0, nullptr, reinterpret_cast<void**>(&p));
    if (FAILED(mapped) || !p) {
        if (SUCCEEDED(mapped)) rb->Unmap(0, nullptr);
        LOGE("capture: readback Map failed or returned null (0x%08lX)", static_cast<unsigned long>(mapped));
        return false;
    }
    out.resize(size_t(w) * h * 4);
    for (uint32_t y = 0; y < h; y++) std::memcpy(&out[size_t(y) * w * 4], p + size_t(y) * fp.Footprint.RowPitch, w * 4);
    rb->Unmap(0, nullptr);
    return true;
}

// BMP file and info header: byte offsets of the fields CapSave writes
constexpr int kBmpHeaderBytes = 54;
constexpr int kBmpFileSize = 2;
constexpr int kBmpPixelOffset = 10;
constexpr int kBmpInfoSize = 14;
constexpr int kBmpWidth = 18;
constexpr int kBmpHeight = 22;  // negative: rows top down
constexpr int kBmpPlanes = 26;
constexpr int kBmpBitCount = 28;
constexpr int kBmpImageSize = 34;
constexpr uint32_t kBmpInfoBytes = 40;

void D3D12Backend::CapSave(const CapImage& img, const char* name) {
    ComPtr<ID3D12Resource> down = Shrink(img.res, *img.state, img.fmt, img.w, img.h, {.mul = img.mul});
    std::vector<uint8_t> px;
    if (!down || !ReadBack(down.Get(), img.w, img.h, px)) return;
    FILE* f = _wfopen((cap_dir_ / (std::string(name) + ".bmp")).c_str(), L"wb");
    if (!f) return;
    uint8_t hd[kBmpHeaderBytes] = {'B', 'M'};  // BITMAPFILEHEADER and BITMAPINFOHEADER, top down 32 bit
    auto put = [&](int o, uint32_t v) { std::memcpy(hd + o, &v, 4); };
    put(kBmpFileSize, kBmpHeaderBytes + uint32_t(px.size()));
    put(kBmpPixelOffset, kBmpHeaderBytes);
    put(kBmpInfoSize, kBmpInfoBytes);
    put(kBmpWidth, img.w);
    put(kBmpHeight, uint32_t(-int32_t(img.h)));
    hd[kBmpPlanes] = 1;
    hd[kBmpBitCount] = 32;
    put(kBmpImageSize, uint32_t(px.size()));
    fwrite(hd, 1, kBmpHeaderBytes, f);
    fwrite(px.data(), 1, px.size(), f);
    fclose(f);
}

void D3D12Backend::CapSave(Target& t, const char* name) {
    DXGI_FORMAT fmt = t.depth ? kDepthRead : t.fmt;
    float mul = t.depth ? 1 : t.range;
    CapSave({.res = t.res.Get(), .state = &t.state, .fmt = fmt, .w = t.gw, .h = t.gh, .mul = mul}, name);
}

// the frame moves to another target: save the one it leaves
void D3D12Backend::CapStep(const Surface& rt) {
    uint32_t d = rt.header ? mem::VirtualFromPhysical(rt.data) : 0;
    if (d == cap_rt_) return;
    auto it = targets_.find(TargetMapKey(cap_rt_, false));
    if (cap_rt_ && it != targets_.end()) {
        char name[32];
        snprintf(name, sizeof(name), "%04u_%08X", cap_i_, cap_rt_);
        CapSave(it->second, name);
    }
    auto z = targets_.find(TargetMapKey(cap_rt_, true));
    if (cap_rt_ && z != targets_.end()) {
        char name[32];
        snprintf(name, sizeof(name), "%04u_%08X_z", cap_i_, cap_rt_);
        CapSave(z->second, name);
    }
    cap_rt_ = d;
}

void D3D12Backend::Capture(Target& t, const char* tag, float mul) {
    if (!cap_) return;
    char name[32];
    snprintf(name, sizeof(name), "%04u_%s_%08X", cap_i_, tag, t.data);
    CapSave({.res = t.res.Get(), .state = &t.state, .fmt = t.fmt, .w = t.gw, .h = t.gh, .mul = mul}, name);
}

void D3D12Backend::SetGamma(const uint8_t* ramp) {
    std::memcpy(ramp_, ramp, sizeof(ramp_));
    ramp_dirty_ = true;
}

}  // namespace

std::unique_ptr<Backend> CreateBackend() { return std::make_unique<D3D12Backend>(); }

}  // namespace gpu
