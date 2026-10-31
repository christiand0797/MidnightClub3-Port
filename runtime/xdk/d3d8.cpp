// Direct3D 8 (XDK 4831 and 5849) entry points the title calls, implemented on the host renderer.
// Resource creation, locking and the XG helpers stay recompiled: they only manage memory.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <map>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include "app/app.h"
#include "core/diag.h"
#include "gpu/backend.h"
#include "gpu/shaders.h"
#include "gpu/simple_dirty.h"
#include "gpu/texture.h"
#include "gpu/vertex.h"
#include "gpu/pbr_light_snapshot.h"
#include "gpu/pbr_observation.h"
#include "xdk/d3d8.h"
#include "xdk/hle.h"
#include "xdk/layout.h"

using namespace kernel;

namespace dispatch {
bool InCode(uint32_t va);  // cpu/dispatch.cpp
}

// configs written before the SDK knew the name
#ifndef SYM_D3DDevice_BeginPush
#define SYM_D3DDevice_BeginPush FFFFFE00
#define HAS_D3DDevice_BeginPush 0
#endif
#ifndef SYM_D3DDevice_EndPush
#define SYM_D3DDevice_EndPush FFFFFE01
#define HAS_D3DDevice_EndPush 0
#endif
#ifndef HAS_D3D_FvfShader
#define HAS_D3D_FvfShader 0
#endif
#ifndef SYM_D3DDevice_SetPalette
#define SYM_D3DDevice_SetPalette FFFFFE02
#define HAS_D3DDevice_SetPalette 0
#endif
#ifndef SYM_D3DDevice_GetVertexShaderFunction
#define SYM_D3DDevice_GetVertexShaderFunction FFFFFE03
#define HAS_D3DDevice_GetVertexShaderFunction 0
#endif
#ifndef SYM_D3DDevice_GetTile
#define SYM_D3DDevice_GetTile FFFFFE04
#define HAS_D3DDevice_GetTile 0
#endif
#ifndef SYM_D3DDevice_SetRenderTargetFast
#define SYM_D3DDevice_SetRenderTargetFast FFFFFE05
#define HAS_D3DDevice_SetRenderTargetFast 0
#endif
#ifndef SYM_D3DDevice_LoadVertexShaderProgram
#define SYM_D3DDevice_LoadVertexShaderProgram FFFFFE06
#define HAS_D3DDevice_LoadVertexShaderProgram 0
#endif
#ifndef SYM_D3DDevice_GetRasterStatus
#define SYM_D3DDevice_GetRasterStatus FFFFFE07
#define HAS_D3DDevice_GetRasterStatus 0
#endif
#ifndef SYM_D3DDevice_Reset
#define SYM_D3DDevice_Reset FFFFFE08
#define HAS_D3DDevice_Reset 0
#endif
#ifndef SYM_D3DDevice_RunPushBuffer
#define SYM_D3DDevice_RunPushBuffer FFFFFE09
#define HAS_D3DDevice_RunPushBuffer 0
#endif
#ifndef SYM_D3D_BlockOnNonSurfaceResource
#define SYM_D3D_BlockOnNonSurfaceResource FFFFFE0A
#define HAS_D3D_BlockOnNonSurfaceResource 0
#endif
#ifndef SYM_D3DDevice_DrawIndexedVerticesUP
#define SYM_D3DDevice_DrawIndexedVerticesUP FFFFFE0B
#endif
#ifndef HAS_D3D_VertexShaderConstants
#define HAS_D3D_VertexShaderConstants 0
#endif

namespace d3d {

namespace {

// A/B switch: 0 draws on the title's thread, as before
#ifndef XBE_RENDER_THREAD
#define XBE_RENDER_THREAD 1
#endif
constexpr bool kRenderThread = XBE_RENDER_THREAD;

// D3D globals of this build
constexpr uint32_t kDevice = SYM(D3D_Device);        // the static CDevice, g_pDevice points at it
constexpr uint32_t kDevicePtr = SYM(D3D_pDevice);
constexpr uint32_t kIndexData = SYM(D3D_IndexData);  // index data base the inline draws read
constexpr uint32_t kDirtyFlags = SYM(D3D_DirtyFlags);
constexpr uint32_t kTextureState = SYM(D3D_DeferredTextureState);  // [4][32]
constexpr uint32_t kRenderState = SYM(D3D_RenderState);            // [166]
constexpr uint32_t kStreams = SYM(D3D_Streams);                    // [16] stride, offset, buffer
constexpr uint32_t kSimpleEncode = SYM(D3D_SimpleRenderStateEncode);

// CDevice fields mirrored so the recompiled getters and resource code see what the XDK would
constexpr uint32_t kDevPush = XDK_CDevice_Push, kDevThreshold = XDK_CDevice_PushThreshold,
                   kDevBaseVertex = XDK_CDevice_BaseVertex, kDevIndexBuffer = XDK_CDevice_IndexBuffer,
                   kDevTextures = XDK_CDevice_Textures, kDevRenderTarget = XDK_CDevice_RenderTarget,
                   kDevDepth = XDK_CDevice_Depth, kDevSwapCount = XDK_CDevice_SwapCount,
                   kDevBackBuffer = XDK_CDevice_BackBuffer, kDevFrontBuffer = XDK_CDevice_FrontBuffer,
                   kDevZScale = XDK_CDevice_ZScale, kDevSuperX = XDK_CDevice_SuperSampleX,
                   kDevSuperY = XDK_CDevice_SuperSampleY, kDevTransforms = XDK_CDevice_Transforms,
                   kDevPalettes = XDK_CDevice_Palettes;
constexpr uint32_t kStageStates = XDK_TextureState_Stage, kStreamSize = XDK_Stream_Size;

gpu::State g_state;
std::unique_ptr<gpu::Backend> g_backend;
std::recursive_mutex g_lock;

// every write to g_state.simple goes through here or marks it
void SetSimple(uint32_t method, uint32_t v) {
    g_state.simple[method >> 2] = v;
    gpu::MarkSimple(method >> 2);
}

std::unordered_map<uint32_t, std::shared_ptr<gpu::VertexShader>> g_vs;
std::unordered_map<uint32_t, std::shared_ptr<gpu::PixelShader>> g_ps;
std::array<uint32_t, 3> g_buffers{};  // front, back surfaces
uint32_t g_buffer_count = 2;
uint32_t g_depth = 0;
uint32_t g_scratch_push = 0, g_scratch_size = 0;
uint32_t g_walk = 0;  // first scratch command not read yet

uint32_t Dev(uint32_t off) { return R32(kDevice + off); }
void SetDev(uint32_t off, uint32_t v) { W32(kDevice + off, v); }

// The device's own references: a count in bits 19 to 22 of the resource's first dword. A surface's first one
// also holds its parent. The library's functions do it when the image has them, inlined ones are done here.
void AddRef(X86Context& ctx, uint32_t res) {
    if (!res) return;
#if HAS_D3D_InternalAddRef
    GUEST_CALL(D3D_InternalAddRef, ctx, {res});
#else
    (void)ctx;
    uint32_t common = R32(res);
    if (!(common & 0x780000) && (common & 0x70000) == 0x50000)
        if (uint32_t parent = R32(res + 0x14)) W32(parent, R32(parent) + 0x80000);
    W32(res, R32(res) + 0x80000);
#endif
}
void Release(X86Context& ctx, uint32_t res) {
    if (!res) return;
#if HAS_D3D_InternalRelease
    GUEST_CALL(D3D_InternalRelease, ctx, {res});
#else
    (void)ctx;
    // the library frees a resource nothing holds any more; without its function the memory stays
    auto drop = [](uint32_t r) {
        W32(r, R32(r) - 0x80000);
        if (!(R32(r) & 0x78FFFF)) LOG_FIRST(8, "d3d: resource %08X is not freed, the image has no D3D_InternalRelease", r);
    };
    uint32_t common = R32(res);
    if ((common & 0x780000) == 0x80000 && (common & 0x70000) == 0x50000)
        if (uint32_t parent = R32(res + 0x14)) drop(parent);
    drop(res);
#endif
}

void Walk(uint32_t end);

// Commands the title pushed itself that Walk has not read run before a direct state change, so an older raw write
// (an inlined constant set) never wins over a newer one through the API
void FlushPush() {
    if (g_walk && Dev(kDevPush) != g_walk) {
        std::lock_guard<std::recursive_mutex> lock(g_lock);
        Walk(Dev(kDevPush));
    }
}

// Hardware commands a caller still pushes go to a scratch buffer, rewound at every request once Walk has read it
uint32_t ScratchPush(uint32_t bytes = 0) {
    Walk(Dev(kDevPush));
    uint32_t need = std::max<uint32_t>(0x10000, (bytes + 0x1FFF) & ~0xFFFu);
    if (need > g_scratch_size) {
        if (g_scratch_push) mem::FreeContiguous(g_scratch_push);
        g_scratch_push = mem::AllocContiguous(need, 0, 0xFFFFFFFF, 0x1000);
        g_scratch_size = need;
    }
    SetDev(kDevPush, g_scratch_push);
    SetDev(kDevThreshold, g_scratch_push + g_scratch_size - 0x1000);
    g_walk = g_scratch_push;
    return g_scratch_push;
}

gpu::Surface SurfaceOf(uint32_t header) {
    gpu::Surface s;
    s.header = header;
    if (header) {
        s.data = R32(header + 4);
        s.format = R32(header + 12);
        s.size = R32(header + 16);
    }
    return s;
}

uint32_t NewSurface(uint32_t w, uint32_t h, uint32_t fmt, uint32_t bpp) {
    uint32_t pitch = (w * bpp + 63) & ~63u;
    uint32_t data = mem::AllocContiguous(pitch * h, 0, 0xFFFFFFFF, 0x1000);
    uint32_t hdr = mem::AllocSystem(24);
    W32(hdr + 0, 0x000D0001);  // surface, one reference, one internal reference held by the device
    W32(hdr + 4, mem::PhysicalFromVirtual(data));
    W32(hdr + 8, 0);
    W32(hdr + 12, 0x00010021 | (fmt << 8));  // dma A, 2D, one level
    W32(hdr + 16, (w - 1) | (h - 1) << 12 | (pitch / 64 - 1) << 24);
    W32(hdr + 20, 0);
    return hdr;
}

void SyncDeferred() {
    // deferred states live in the title's memory, the inline SetRenderState and SetTextureStageState write them
    for (uint32_t i = gpu::kRsDeferredFirst; i < gpu::kRsComplexFirst; i++) g_state.rs[i] = R32(kRenderState + i * 4);
    for (int s = 0; s < 4; s++)
        for (uint32_t i = 0; i < gpu::kTssBumpEnvMat00; i++) g_state.tss[s][i] = R32(kTextureState + (s * kStageStates + i) * 4);
}

float ZScale(const gpu::Surface& zs) {
    uint32_t f = zs.header ? (zs.format >> 8) & 0xFF : 0x2E;
    return f == 0x2C || f == 0x30 ? 65535.0f : 16777215.0f;
}

bool g_own_constants = false;  // D3DSCM_NORESERVEDCONSTANTS: c-38 and c-37 are the title's

// c-38 and c-37 hold the viewport scale and offset, as D3D_GetViewportOffsetAndScale computes them
void UpdateViewportConstants() {
    auto& v = g_state.viewport;
    g_state.zscale = ZScale(g_state.zs);
    float hw = v.w * 0.5f, hh = v.h * 0.5f;
    if (!g_own_constants) {
        g_state.vsc[58] = {hw, -hh, (v.max_z - v.min_z) * g_state.zscale, 0};
        g_state.vsc[59] = {v.x + g_state.ss_offset_x + hw, v.y + g_state.ss_offset_y + hh, v.min_z * g_state.zscale, 0};
    }
    // the viewport as the library's SetViewport stores it, for inlined readers
    if (XDK_CDevice_Viewport != xdk::kUnknown) {
        uint32_t p = kDevice + XDK_CDevice_Viewport;
        W32(p, v.x);
        W32(p + 4, v.y);
        W32(p + 8, v.w);
        W32(p + 12, v.h);
        std::memcpy(HostPtr(p + 16), &v.min_z, 4);
        std::memcpy(HostPtr(p + 20), &v.max_z, 4);
    }
}

void SetComplex(uint32_t rs, uint32_t v) {
    g_state.rs[rs] = v;
    W32(kRenderState + rs * 4, v);
}

// The NV2A's transform program memory: 136 instructions of 4 dwords. The XDK's SetVertexShader loads a shader's
// program at slot 0 and starts it there; LoadVertexShader and LoadVertexShaderProgram load at any slot, and
// SelectVertexShader starts the program at a slot with a declaration's register layout. The renderer gets what the
// hardware would run: the slots from the start to the instruction with the final flag, as a VertexShader object.
// See docs/notes/d3d8_shader_slots.md.
constexpr uint32_t kProgramSlots = 136;
std::array<std::array<uint32_t, 4>, kProgramSlots> g_slots{};
std::array<bool, kProgramSlots> g_slot_written{};
uint32_t g_slot_gen = 1;  // moves on at every slot write

// the program running from the slots, while the bound shader is one
struct Selection {
    bool on = false;
    uint32_t start = 0;
    uint32_t handle = 0;                       // the declaration: a shader handle, or an FVF when decl is null
    std::shared_ptr<gpu::VertexShader> decl;   // the shader the layout comes from
    uint32_t gen = 0;                          // g_slot_gen the bound program was read at
} g_sel;

// the render thread tells shaders apart by pointer, so an object made for a program and declaration is kept and
// reused; it keeps the declaration's object alive so its address is never another's
struct SlotShader {
    std::shared_ptr<gpu::VertexShader> decl, vs;
};
using SlotKey = std::pair<std::pair<const gpu::VertexShader*, uint32_t>, std::vector<uint32_t>>;
std::map<SlotKey, SlotShader> g_slot_shaders;

void ResetProgramSlots() {
    g_slot_written.fill(false);
    g_slot_gen++;
    g_sel = Selection{};
}

// count instructions of 4 dwords into the slots from addr
void WriteSlots(uint32_t addr, const uint32_t* words, uint32_t count) {
    if (addr >= kProgramSlots) {
        LOG_FIRST(8, "d3d: vertex program load at slot %u is past the %u slots, dropped", addr, kProgramSlots);
        return;
    }
    if (count > kProgramSlots - addr) {
        LOG_FIRST(8, "d3d: vertex program of %u instructions at slot %u is cut at the last slot", count, addr);
        count = kProgramSlots - addr;
    }
    for (uint32_t i = 0; i < count; i++) {
        std::memcpy(g_slots[addr + i].data(), words + i * 4, 16);
        g_slot_written[addr + i] = true;
    }
    if (count) g_slot_gen++;
}

// binds the selected program: the slots from the start up to the final flag (bit 0 of dword 3), or up to a slot
// nothing was loaded into
void BindSelected() {
    g_sel.gen = g_slot_gen;
    std::vector<uint32_t> program;
    for (uint32_t i = g_sel.start; i < kProgramSlots && g_slot_written[i]; i++) {
        program.insert(program.end(), g_slots[i].begin(), g_slots[i].end());
        if (g_slots[i][3] & 1) break;
    }
    if (program.empty() || (g_sel.decl && program == g_sel.decl->program)) {
        if (program.empty())
            LOG_FIRST(8, "d3d: vertex program selected at slot %u, where nothing was loaded; the declaration's shader is kept",
                      g_sel.start);
        g_state.vs = g_sel.decl;
        g_state.vs_handle = g_sel.handle;
        return;
    }
    SlotKey key{{g_sel.decl.get(), g_sel.decl ? 0u : g_sel.handle}, std::move(program)};
    auto it = g_slot_shaders.find(key);
    if (it == g_slot_shaders.end()) {
        if (g_slot_shaders.size() >= 1024) g_slot_shaders.clear();  // bound objects stay alive through g_state
        auto vs = std::make_shared<gpu::VertexShader>();
        if (g_sel.decl) {
            vs->decl = g_sel.decl->decl;
            vs->inputs = g_sel.decl->inputs;
            vs->stream_stride = g_sel.decl->stream_stride;
        } else {
            gpu::FvfInputs(g_sel.handle, *vs);  // a program over an FVF's layout
        }
        vs->program = key.second;
        vs->program_type = 0x2078;
        vs->handle = g_sel.decl ? g_sel.handle : g_sel.handle | 1;
        LOG_FIRST(32, "d3d: vertex program from slot %u, %zu instructions, declaration %08X", g_sel.start,
                  vs->program.size() / 4, g_sel.handle);
        it = g_slot_shaders.emplace(std::move(key), SlotShader{g_sel.decl, vs}).first;
    }
    g_state.vs = it->second.vs;
    g_state.vs_handle = it->second.vs->handle;  // bit 0 set: the renderer runs the program on the layout of vs
}

// before a draw: slots written since the selection change what the selection runs
void SyncProgram() {
    if (g_sel.on && g_sel.gen != g_slot_gen) BindSelected();
}

// SelectVertexShader(handle, start): handle 0 keeps the declaration in use
void SelectProgram(uint32_t handle, uint32_t start) {
    if (handle) {
        auto it = g_vs.find(handle);
        if (it != g_vs.end()) {
            g_sel.handle = handle;
            g_sel.decl = it->second;
            // constants declared with the shader load when it is selected, as in SetVertexShader
            for (auto& [reg, v] : it->second->constants)
                if (reg < 192) g_state.vsc[reg] = v;
        } else {
            LOG_FIRST(8, "d3d: SelectVertexShader with unknown handle %08X keeps the declaration in use", handle);
            handle = 0;
        }
    }
    if (!handle && !g_sel.on) {
        g_sel.handle = g_state.vs_handle;
        g_sel.decl = g_state.vs_handle & 1 ? g_state.vs : nullptr;
    }
    g_sel.on = true;
    g_sel.start = start;
    BindSelected();
}

// Transform methods the title pushes itself rather than through the API (the XDK's inlined constant sets, as the
// ped skinning's index scale c9; program uploads and starts). SET_TRANSFORM_CONSTANT_LOAD and
// SET_TRANSFORM_PROGRAM_LOAD pick the register or slot, each 4 SET_TRANSFORM_CONSTANT or SET_TRANSFORM_PROGRAM
// dwords fill it and move to the next, SET_TRANSFORM_PROGRAM_START starts the program at a slot with the
// declaration in use. True when m was one of them.
uint32_t g_const_load = 0, g_const_part = 0, g_prog_load = 0;

bool ApplyTransformMethod(uint32_t m, uint32_t v) {
    constexpr uint32_t kProgram = 0x0B00, kConstant = 0x0B80, kExecutionMode = 0x1E94, kWriteEnable = 0x1E98,
                       kProgramLoad = 0x1E9C, kProgramStart = 0x1EA0, kConstantLoad = 0x1EA4;
    if (m >= kConstant && m < kConstant + 0x80) {  // 32 dwords
        float f;
        std::memcpy(&f, &v, 4);
        const uint32_t part = (m - kConstant) / 4 & 3;
        if (g_const_load < 192) g_state.vsc[g_const_load][part] = f;
        g_const_part = (part + 1) & 3;
        if (part == 3) g_const_load++;
        return true;
    }
    if (m >= kProgram && m < kProgram + 0x80) {  // 32 dwords, the dword of the instruction by the method
        uint32_t part = (m - kProgram) / 4 & 3;
        if (g_prog_load < kProgramSlots) {
            g_slots[g_prog_load][part] = v;
            g_slot_written[g_prog_load] = true;
            g_slot_gen++;
        } else {
            LOG_FIRST(4, "d3d: pushed vertex program write at slot %u is past the slots", g_prog_load);
        }
        if (part == 3) g_prog_load++;
        return true;
    }
    switch (m) {
        case kConstantLoad:
            g_const_load = v;
            g_const_part = 0;
            return true;
        case kProgramLoad:
            g_prog_load = v;
            return true;
        case kProgramStart:
            SelectProgram(0, v);
            return true;
        case kExecutionMode:  // bits 0-1: 0 fixed function, 2 program; START follows a switch to a program
            if ((v & 3) != 2) LOG_FIRST(4, "d3d: pushed transform execution mode %X is ignored", v);
            return true;
        case kWriteEnable:
            return true;
        default:
            return false;
    }
}

// The library's default render and texture stage states; its pushes land in the scratch buffer
void ApplyDefaultStates(X86Context& ctx) {
#if HAS_D3D_InitDefaultState
    GUEST_CALL(D3D_InitDefaultState, ctx, {});
#else
    (void)ctx;
    LOG_FIRST(1, "d3d: the image has no D3D_InitDefaultState, default states were not applied");
#endif
}

// The frame buffers a D3DPRESENT_PARAMETERS asks for become the device's targets, the viewport covers them. The
// renderer is made at the first ones' size.
void SetFrameBuffers(const char* caller, uint32_t pp) {
    uint32_t w = R32(pp), h = R32(pp + 4), fmt = R32(pp + 8), count = std::max<uint32_t>(R32(pp + 12), 1);
    uint32_t auto_depth = R32(pp + 0x20), depth_fmt = R32(pp + 0x24), interval = R32(pp + 0x30);
    if (!w) w = 640;
    if (!h) h = 480;
    LOGI("d3d: %s %ux%u format %X buffers %u depth %u format %X interval %X flags %X", caller, w, h, fmt, count,
         auto_depth, depth_fmt, interval, R32(pp + 0x28));
    uint32_t color_fmt = gpu::LinearColorFormat(fmt);
    uint32_t zf = auto_depth ? gpu::LinearDepthFormat(depth_fmt) : 0;
    // surfaces of the same shape are kept: a title wraps their memory in textures of its own across a Reset
    static std::array<uint32_t, 5> made{};
    std::array<uint32_t, 5> asked{w, h, color_fmt, std::min<uint32_t>(count + 1, 3), zf};
    if (asked != made) {
        made = asked;
        g_buffer_count = asked[3];
        for (uint32_t i = 0; i < g_buffer_count; i++) g_buffers[i] = NewSurface(w, h, color_fmt, 4);
        g_depth = zf ? NewSurface(w, h, zf, zf == 0x30 || zf == 0x31 ? 2 : 4) : 0;  // 0x30 LIN_D16, 0x31 LIN_F16
    }
    if (!g_backend) {
        g_backend = gpu::CreateBackend();
        if (!g_backend->Init(w, h)) {
            LOGE("d3d: renderer init failed");
            // no owner window: this runs on the title's thread and the window may already be fullscreen over the box
            MessageBoxW(nullptr,
                        L"Direct3D 12 could not be started. Make sure your graphics card supports it and its driver "
                        L"is up to date.",
                        app::kGame.short_title, MB_ICONERROR | MB_TOPMOST);
            kernel::Shutdown(1);
        }
        if (kRenderThread) g_backend = gpu::CreateThreadedBackend(std::move(g_backend));
    }
    g_state.viewport = {0, 0, w, h, 0.0f, 1.0f};
    SetDev(kDevRenderTarget, g_buffers[1]);
    SetDev(kDevDepth, g_depth);
    SetDev(kDevBackBuffer, g_buffers[1]);
    SetDev(kDevFrontBuffer, g_buffers[1]);
    float one = 1.0f, zs = g_depth && (R32(g_depth + 12) >> 8 & 0xFF) == 0x30 ? 65535.0f : 16777215.0f;
    std::memcpy(HostPtr(kDevice + kDevSuperX), &one, 4);
    std::memcpy(HostPtr(kDevice + kDevSuperY), &one, 4);
    std::memcpy(HostPtr(kDevice + kDevZScale), &zs, 4);
    // fields the library's init sets that inlined library code and the default states read
    if (XDK_CDevice_AutoDepth != xdk::kUnknown) SetDev(XDK_CDevice_AutoDepth, g_depth);
    if (XDK_CDevice_BackBufferScale != xdk::kUnknown) {
        std::memcpy(HostPtr(kDevice + XDK_CDevice_BackBufferScale), &one, 4);
        std::memcpy(HostPtr(kDevice + XDK_CDevice_BackBufferScale + 4), &one, 4);
    }
    g_state.rt = SurfaceOf(g_buffers[1]);
    g_state.zs = SurfaceOf(g_depth);
    UpdateViewportConstants();
}

constexpr bool kVBlank = XDK_CDevice_VBlankCount != xdk::kUnknown && XDK_CDevice_VBlankEvent != xdk::kUnknown &&
                         XDK_CDevice_VBlankCallback != xdk::kUnknown;

// what the library's vertical blank interrupt leaves for the title: count, event, callback
void VerticalBlank(X86Context& ctx) {
    using clock = std::chrono::steady_clock;
    uint32_t info = mem::AllocSystem(12), shown = 0;
    auto next = clock::now();
    for (;;) {
        next = std::max(next + std::chrono::microseconds(16667), clock::now());
        std::this_thread::sleep_until(next);
        uint32_t count = Dev(XDK_CDevice_VBlankCount) + 1, swaps = Dev(kDevSwapCount);
        SetDev(XDK_CDevice_VBlankCount, count);
        ObjectRef event = FromPointer(kDevice + XDK_CDevice_VBlankEvent);
        SetEvent(*event);
        if (uint32_t callback = Dev(XDK_CDevice_VBlankCallback)) {
            // blank count, swaps shown, 1 when this blank showed one
            W32(info, count);
            W32(info + 4, swaps);
            W32(info + 8, swaps != shown);
            CallGuest(ctx, callback, {info});
        }
        shown = swaps;
        // the title clears the event in its own memory before a wait, which the host object does not see
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        ResetEvent(*event);
    }
}

}  // namespace

gpu::State& State() { return g_state; }
std::recursive_mutex& Lock() { return g_lock; }
void FlushPendingCommands() {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    FlushPush();
}

}  // namespace d3d

using namespace d3d;

// device

HLE_STDCALL(Direct3D_CreateDevice, 6) {
    XDK_REQUIRE("d3d", XDK_DATA(D3D_Device), XDK_DATA(D3D_pDevice), XDK_DATA(D3D_DirtyFlags),
                XDK_DATA(D3D_DeferredTextureState), XDK_DATA(D3D_RenderState), XDK_DATA(D3D_Streams),
                XDK_DATA(D3D_SimpleRenderStateEncode));
    XDK_REQUIRE("d3d", XDK_KEY(CDevice_Push), XDK_KEY(CDevice_PushThreshold), XDK_KEY(CDevice_BaseVertex),
                XDK_KEY(CDevice_IndexBuffer), XDK_KEY(CDevice_ZScale), XDK_KEY(CDevice_SuperSampleX),
                XDK_KEY(CDevice_SuperSampleY), XDK_KEY(CDevice_Transforms), XDK_KEY(CDevice_Textures),
                XDK_KEY(CDevice_RenderTarget), XDK_KEY(CDevice_Depth), XDK_KEY(CDevice_BackBuffer),
                XDK_KEY(CDevice_FrontBuffer), XDK_KEY(CDevice_SwapCount), XDK_KEY(CDevice_Size),
                XDK_KEY(DirtyFlags_Texture), XDK_KEY(TextureState_Stage), XDK_KEY(Stream_Size),
                XDK_KEY(Stream_Stride), XDK_KEY(Stream_Buffer));
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    std::memset(HostPtr(kDevice), 0, XDK_CDevice_Size);
    W32(kDevicePtr, kDevice);
    ScratchPush();
    g_state = gpu::State{};
    g_state.pbr_light_proof.DeviceReset(); // This host device creation just established the empty light shadow/device.
    ResetProgramSlots();
    gpu::MarkAllSimple();
    gpu::g_simple_tracked = true;
    SetFrameBuffers("CreateDevice", a[4]);
    // 60 Hz, widescreen
    if (XDK_CDevice_DisplayFlags != xdk::kUnknown) SetDev(XDK_CDevice_DisplayFlags, 0x400000 | (app::Widescreen() ? 0x10000 : 0));
    // the miniport's hardware init leaves an identity ramp in slot 0, which GetGammaRamp reads until one is set
    if (XDK_CDevice_GammaRamps != xdk::kUnknown) {
        uint8_t* ramp = static_cast<uint8_t*>(HostPtr(kDevice + XDK_CDevice_GammaRamps));  // red, green, blue
        for (uint32_t i = 0; i < 256; i++) ramp[i] = ramp[256 + i] = ramp[512 + i] = uint8_t(i);
    }
#if HAS_D3D_FvfShader
    if (XDK_CDevice_VertexShader != xdk::kUnknown) SetDev(XDK_CDevice_VertexShader, SYM(D3D_FvfShader));
#endif
    if (kVBlank) {
        // notification event, signalled
        SetDev(XDK_CDevice_VBlankEvent, 0x00040000);
        SetDev(XDK_CDevice_VBlankEvent + 4, 1);
        static bool started = false;
        if (!started) StartHostThread("vertical blank", VerticalBlank);
        started = true;
    }
    ApplyDefaultStates(ctx);
    if (a[5]) W32(a[5], kDevice);
    return 0;
}

HLE_STDCALL(D3DDevice_Release, 0) { return 1; }

// synchronisation, the renderer never runs behind

HLE_STDCALL(D3D_BlockOnResource, 1) {
    uint32_t res = a[0];
    if (res && g_backend) {
        std::lock_guard<std::recursive_mutex> lock(g_lock);
        uint32_t type = R32(res) & 0x70000;  // 0x40000 texture, 0x50000 surface
        if (type == 0x40000 || type == 0x50000) {
            uint32_t data = mem::VirtualFromPhysical(R32(res + 4));
            // perf diagnostic: who blocks on which surface, once each
            static std::unordered_set<uint32_t> seen;
            if (diag::Perf() && seen.insert(data).second) {
                uint32_t code[4] = {}, n = 0;
                for (uint32_t i = 0; i < 32 && n < 4; i++) {
                    uint32_t v = R32(ctx.esp + i * 4);
                    if (dispatch::InCode(v)) code[n++] = v;
                }
                LOGI("d3d: BlockOnResource on surface %08X, stack %08X %08X %08X %08X", data, code[0], code[1], code[2], code[3]);
            }
            g_backend->Sync(data, 0);
        } else {
            g_backend->WaitResource(res);
        }
    }
    gpu::MarkDirty(res);
    return 0;
}
// lock(resource, face, level, locked, rect, flags): D3DLOCK_NOOVERWRITE skips the block above, yet a
// title may read what the GPU drew
HLE_WRAP(Lock2DSurface, 6) {
    uint32_t res = a[0];
    if ((a[5] & 0x20) && res && g_backend) {  // D3DLOCK_NOOVERWRITE
        std::lock_guard<std::recursive_mutex> lock(g_lock);
        g_backend->SyncLate(mem::VirtualFromPhysical(R32(res + 4)));
    }
}
HLE_STDCALL(D3DResource_BlockUntilNotBusy, 1) {
    if (a[0] && g_backend) {
        std::lock_guard<std::recursive_mutex> lock(g_lock);
        g_backend->WaitResource(a[0]);
    }
    gpu::MarkDirty(a[0]);
    return 0;
}
// (resource): Lock2 of a vertex buffer or palette. The library waits on GPU time stamps, which the host does not keep
// (BlockOnTime returns at once), so a title refilling a buffer would overwrite draws not yet taken.
HLE_STDCALL(D3D_BlockOnNonSurfaceResource, 1) {
    if (a[0] && g_backend) {
        std::lock_guard<std::recursive_mutex> lock(g_lock);
        g_backend->WaitResource(a[0]);
    }
    gpu::MarkDirty(a[0]);
    return 0;
}
HLE_STDCALL(D3DResource_IsBusy, 1) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    uint32_t res = a[0];
    if (!res) return 0;
    // set on the device, a surface also through its texture
    uint32_t common = R32(res), parent = (common & 0x70000) == 0x50000 ? R32(res + 0x14) : 0;
    if ((common & 0x780000) || (parent && (R32(parent) & 0x780000))) return 1;  // internal reference count
    return g_backend && g_backend->Busy(res) ? 1 : 0;
}
HLE_STDCALL(D3D_BlockOnTime, 2) { return 0; }
HLE_STDCALL(D3DDevice_BlockOnFence, 1) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    if (g_backend) g_backend->WaitFence(a[0]);
    return 0;
}
HLE_STDCALL(D3DDevice_InsertFence, 0) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    return g_backend ? g_backend->Fence() : 0;
}
HLE_STDCALL(D3DDevice_IsFencePending, 1) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    return g_backend && g_backend->FencePending(a[0]) ? 1 : 0;
}
HLE_STDCALL(D3D_SetFence, 1) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    return g_backend ? g_backend->Fence() : 0;
}
HLE_STDCALL(D3D_KickOffAndWaitForIdle, 0) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    if (g_backend) g_backend->Flush();
    return 0;
}
HLE_FASTCALL(CDevice_KickOff, 0) { return 0; }

HLE_STDCALL(D3DDevice_MakeSpace, 0) {
    LOG_FIRST(16, "d3d: MakeSpace from %08X, some caller still pushes hardware commands", R32(ctx.esp));
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    return ScratchPush();
}
HLE_STDCALL(D3D_MakeRequestedSpace, 2) {
    LOG_FIRST(16, "d3d: MakeRequestedSpace from %08X", R32(ctx.esp));
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    return ScratchPush();
}

HLE_STDCALL(D3DDevice_BlockUntilVerticalBlank, 0) {
    std::this_thread::sleep_for(std::chrono::milliseconds(16));
    return 0;
}
// (status): D3DRASTER_STATUS {InVBlank, ScanLine} on a 60 Hz frame clock. The library reads the GPU's scanline,
// which counts the front buffer's lines, then the blank: 45 lines per 480 shown, as on NTSC.
HLE_STDCALL(D3DDevice_GetRasterStatus, 1) {
    constexpr int64_t kFrameMicroseconds = 16667;
    constexpr uint32_t kBlankLines = 45;
    constexpr uint32_t kShownLines = 480;
    uint32_t shown = g_buffers[0] ? (R32(g_buffers[0] + 0x10) >> 12 & 0xFFF) + 1 : kShownLines;
    uint32_t lines = shown + shown * kBlankLines / kShownLines;
    auto now = std::chrono::steady_clock::now().time_since_epoch();
    int64_t into_frame = std::chrono::duration_cast<std::chrono::microseconds>(now).count() % kFrameMicroseconds;
    uint32_t line = uint32_t(into_frame * lines / kFrameMicroseconds);
    W32(a[0], line >= shown);
    W32(a[0] + 4, line < shown ? line : 0);
    return 0;
}
HLE_STDCALL(D3DDevice_SetFlickerFilter, 1) { return 0; }
HLE_STDCALL(D3DDevice_SetSoftDisplayFilter, 1) { return 0; }
HLE_STDCALL(D3DDevice_EnableOverlay, 1) { return 0; }
// the front buffer, read back at the guest size, is kept for the next instance after a quick reboot, which may
// fade from it at its first frames
HLE_STDCALL(D3DDevice_PersistDisplay, 0) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    uint32_t front = g_buffers[0], size = R32(front + 0x10);
    uint32_t pitch = ((size >> 24 & 0xFF) + 1) * 64, height = (size >> 12 & 0xFFF) + 1, bytes = pitch * height;
    uint32_t data = mem::VirtualFromPhysical(R32(front + 4));
    if (g_backend) g_backend->Sync(data, bytes);
    kernel::SavePersistedDisplay(R32(front), R32(front + 0xC), size, HostPtr(data), bytes);
    LOGI("d3d: PersistDisplay, %u bytes", bytes);
    return 0;
}
HLE_STDCALL(D3DDevice_FlushVertexCache, 0) { return 0; }

// The library keeps the ramp in one of two device slots for GetGammaRamp: both get it, whichever one its index names
HLE_STDCALL(D3DDevice_SetGammaRamp, 2) {
    uint8_t ramp[768];
    std::memcpy(ramp, HostPtr(a[1]), 768);
    if (XDK_CDevice_GammaRamps != xdk::kUnknown)
        for (uint32_t slot = 0; slot < 2; slot++)
            std::memcpy(HostPtr(kDevice + XDK_CDevice_GammaRamps + slot * sizeof(ramp)), ramp, sizeof(ramp));
    if (g_backend) g_backend->SetGamma(ramp);
    return 0;
}

// targets and presentation

namespace {
// the device's targets become rt and zs, the references move from the old ones
void ApplyRenderTarget(X86Context& ctx, uint32_t rt, uint32_t zs) {
    AddRef(ctx, rt);
    uint32_t old_rt = Dev(kDevRenderTarget);
    uint32_t old_zs = Dev(kDevDepth);
    SetDev(kDevRenderTarget, rt);
    SetDev(kDevDepth, zs);
    AddRef(ctx, zs);
    Release(ctx, old_rt);
    Release(ctx, old_zs);
    g_state.rt = SurfaceOf(rt);
    g_state.zs = SurfaceOf(zs);
}
}  // namespace

HLE_STDCALL(D3DDevice_SetRenderTarget, 2) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    uint32_t rt = a[0] ? a[0] : Dev(kDevRenderTarget);
    ApplyRenderTarget(ctx, rt, a[1]);
    // SetRenderTarget resets the viewport to the whole target
    uint32_t w, h;
    gpu::SurfaceSize(g_state.rt, w, h);
    g_state.viewport = {0, 0, w, h, 0.0f, 1.0f};
    UpdateViewportConstants();
    return 0;
}

// (rt, zs, flags): the surfaces alone, the viewport stays as it was
HLE_STDCALL(D3DDevice_SetRenderTargetFast, 3) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    ApplyRenderTarget(ctx, a[0], a[1]);
    return 0;
}

HLE_STDCALL(D3DDevice_GetBackBuffer2, 1) {
    int32_t i = int32_t(a[0]);
    uint32_t s = i < 0 ? g_buffers[0] : g_buffers[std::min<uint32_t>(uint32_t(i) + 1, g_buffer_count - 1)];
    W32(s, R32(s) + 1);
    return s;
}

HLE_STDCALL(D3DDevice_GetRenderTarget2, 0) {
    uint32_t s = Dev(kDevRenderTarget);
    if (s) W32(s, R32(s) + 1);
    return s;
}

HLE_STDCALL(D3DDevice_GetDepthStencilSurface2, 0) {
    uint32_t s = Dev(kDevDepth);
    if (s) W32(s, R32(s) + 1);
    return s;
}

namespace {
std::atomic<uint32_t> g_presents{0};

// XBE_WATCH="4B2D70:f,534774:x,53476D:b": guest values logged once a second at a present, f float, x dword, b byte
void LogWatches() {
    struct Watch {
        uint32_t va;
        char type;
    };
    static const std::vector<Watch> watches = [] {
        std::vector<Watch> out;
        const char* v = std::getenv("XBE_WATCH");
        while (v && *v) {
            char* end = nullptr;
            uint32_t va = uint32_t(std::strtoul(v, &end, 16));
            if (end == v) break;
            char type = 'x';
            if (*end == ':' && end[1]) {
                type = end[1];
                end += 2;
            }
            out.push_back({va, type});
            if (*end && *end != ',') break;
            v = *end ? end + 1 : end;
        }
        if (!out.empty()) LOGR("watch: %zu guest values, logged once a second", out.size());
        return out;
    }();
    if (watches.empty()) return;
    using clock = std::chrono::steady_clock;
    static clock::time_point next = clock::now();
    static uint32_t presents_at_last = 0;
    if (clock::now() < next) return;
    next = clock::now() + std::chrono::seconds(1);
    std::string line;
    char buf[48];
    for (const Watch& w : watches) {
        if (w.type == 'f') {
            float f;
            uint32_t u = R32(w.va);
            std::memcpy(&f, &u, 4);
            std::snprintf(buf, sizeof(buf), " %X=%.6g", w.va, f);
        } else if (w.type == 'b') {
            std::snprintf(buf, sizeof(buf), " %X=%02X", w.va, kernel::G<uint8_t>(w.va));
        } else {
            std::snprintf(buf, sizeof(buf), " %X=%08X", w.va, R32(w.va));
        }
        line += buf;
    }
    uint32_t presents = g_presents.load();
    LOGR("watch: presents %u%s", presents - presents_at_last, line.c_str());
    presents_at_last = presents;
}
}  // namespace

HLE_STDCALL(D3DDevice_Swap, 1) {
    app::SplashFirstFrame();
    // handed over from the previous instance: the window opens on the second frame, the first picture with
    // anything drawn over it right after
    if (app::HandingOver() && g_presents >= 2) {
        {
            std::lock_guard<std::recursive_mutex> lock(g_lock);
            if (g_backend) g_backend->Flush();
        }
        app::FinishHandover();
    }
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    g_presents++;
    // the kept display is shown until the title presents its own
    kernel::SetSavedDataAddress(0);
    // the ramp an inlined SetGammaRamp left for the vertical blank that shows this swap
    if (g_backend && XDK_CDevice_GammaPending != xdk::kUnknown && XDK_CDevice_GammaRamps != xdk::kUnknown) {
        uint32_t i = Dev(kDevSwapCount) & 1;
        if (Dev(XDK_CDevice_GammaPending + i * 4) == 1) {
            SetDev(XDK_CDevice_GammaPending + i * 4, 0);
            g_backend->SetGamma(static_cast<const uint8_t*>(HostPtr(kDevice + XDK_CDevice_GammaRamps + i * 0x300)));
        }
    }
    Walk(Dev(kDevPush));  // reports asked for at the end of the frame
    LogWatches();
    if (g_backend) g_backend->Present(SurfaceOf(g_buffers[1]));
    app::Pace();
    // like the XDK, the surfaces stay put and their memory rotates: titles keep back buffer pointers and copy its data
    uint32_t last = g_buffers[g_buffer_count - 1], data = R32(last + 4), lk = R32(last + 8);
    for (uint32_t i = g_buffer_count - 1; i > 0; i--) {
        W32(g_buffers[i] + 4, R32(g_buffers[i - 1] + 4));
        W32(g_buffers[i] + 8, R32(g_buffers[i - 1] + 8));
    }
    W32(g_buffers[0] + 4, data);
    W32(g_buffers[0] + 8, lk);
    g_state.rt = SurfaceOf(Dev(kDevRenderTarget));
    uint32_t n = Dev(kDevSwapCount) + 1;
    SetDev(kDevSwapCount, n);
    return n;
}

HLE_STDCALL(D3DDevice_Clear, 6) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    const uint8_t* rects = a[0] && a[1] ? static_cast<const uint8_t*>(HostPtr(a[1])) : nullptr;
    if (g_backend) g_backend->Clear(g_state, a[0], rects, a[2], a[3], ArgF(a, 4), a[5]);
    return 0;
}

// (pp): the frame buffers pp asks for, the targets on them and cleared, as the library's Reset leaves the device.
// The device's references move back to the frame buffers first, so kept ones hold theirs as CreateDevice made them.
HLE_STDCALL(D3DDevice_Reset, 1) {
    constexpr uint32_t kClearTargetAndZ = 3;
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    ApplyRenderTarget(ctx, g_buffers[1], g_depth);
    SetFrameBuffers("Reset", a[0]);
    if (g_backend) g_backend->Clear(g_state, 0, nullptr, kClearTargetAndZ, 0, 1.0f, 0);
    return 0;
}

HLE_STDCALL(D3DDevice_CopyRects, 5) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    if (g_backend) g_backend->Copy(SurfaceOf(a[0]), SurfaceOf(a[3]), a[2], a[1], a[4]);
    return 0;
}

HLE_STDCALL(D3DDevice_SetViewport, 1) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    uint32_t v = a[0];
    g_state.viewport = {R32(v), R32(v + 4), R32(v + 8), R32(v + 12), GF(v + 16), GF(v + 20)};
    g_state.scissor_count = 0;
    UpdateViewportConstants();
    return 0;
}

HLE_STDCALL(D3DDevice_SetScissors, 3) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    g_state.scissor_count = std::min<uint32_t>(a[0], 8);
    for (uint32_t i = 0; i < g_state.scissor_count * 4; i++) g_state.scissors[i] = R32(a[2] + i * 4);
    return 0;
}

HLE_STDCALL(D3DDevice_SetScreenSpaceOffset, 2) {
    g_state.ss_offset_x = ArgF(a, 0);
    g_state.ss_offset_y = ArgF(a, 1);
    UpdateViewportConstants();
    return 0;
}

// render states

HLE_FASTCALL(D3DDevice_SetRenderState_Simple, 0) {
    uint32_t method = a[0] & 0x1FFC, value = a[1];
    SetSimple(method, value);
    return 0;
}

HLE_STDCALL(D3DDevice_SetRenderStateNotInline, 2) {
    uint32_t rs = a[0], v = a[1];
    if (rs < gpu::kRsDeferredFirst) {
        uint32_t method = R32(kSimpleEncode + rs * 4) & 0x1FFC;
        SetSimple(method, v);
        W32(kRenderState + rs * 4, v);  // titles read their states back from the library's table
    } else if (rs < gpu::kRsMax) {
        SetComplex(rs, v);
    }
    return 0;
}

#define COMPLEX_STATE(name, rs)                       \
    HLE_STDCALL(D3DDevice_SetRenderState_##name, 1) { \
        SetComplex(rs, a[0]);                         \
        return 0;                                     \
    }
COMPLEX_STATE(PSTextureModes, gpu::kRsPsTextureModes)
COMPLEX_STATE(VertexBlend, gpu::kRsVertexBlend)
COMPLEX_STATE(FogColor, gpu::kRsFogColor)
COMPLEX_STATE(FillMode, gpu::kRsFillMode)
COMPLEX_STATE(BackFillMode, gpu::kRsBackFillMode)
COMPLEX_STATE(TwoSidedLighting, gpu::kRsTwoSidedLighting)
COMPLEX_STATE(NormalizeNormals, gpu::kRsNormalizeNormals)
COMPLEX_STATE(ZEnable, gpu::kRsZEnable)
COMPLEX_STATE(StencilEnable, gpu::kRsStencilEnable)
COMPLEX_STATE(StencilFail, gpu::kRsStencilFail)
COMPLEX_STATE(FrontFace, gpu::kRsFrontFace)
COMPLEX_STATE(CullMode, gpu::kRsCullMode)
COMPLEX_STATE(TextureFactor, gpu::kRsTextureFactor)
COMPLEX_STATE(LogicOp, gpu::kRsLogicOp)
COMPLEX_STATE(EdgeAntiAlias, gpu::kRsEdgeAntiAlias)
COMPLEX_STATE(MultiSampleAntiAlias, gpu::kRsMultiSampleAntiAlias)
COMPLEX_STATE(MultiSampleMask, gpu::kRsMultiSampleMask)
COMPLEX_STATE(MultiSampleMode, gpu::kRsMultiSampleMode)
COMPLEX_STATE(MultiSampleRenderTargetMode, gpu::kRsMultiSampleRenderTargetMode)
COMPLEX_STATE(ShadowFunc, gpu::kRsShadowFunc)
COMPLEX_STATE(LineWidth, gpu::kRsLineWidth)
COMPLEX_STATE(SampleAlpha, gpu::kRsSampleAlpha)
COMPLEX_STATE(Dxt1NoiseEnable, gpu::kRsDxt1NoiseEnable)
COMPLEX_STATE(YuvEnable, gpu::kRsYuvEnable)
COMPLEX_STATE(OcclusionCullEnable, gpu::kRsOcclusionCullEnable)
COMPLEX_STATE(StencilCullEnable, gpu::kRsStencilCullEnable)
COMPLEX_STATE(RopZCmpAlwaysRead, gpu::kRsRopZCmpAlwaysRead)
COMPLEX_STATE(RopZRead, gpu::kRsRopZRead)
COMPLEX_STATE(DoNotCullUncompressed, gpu::kRsDoNotCullUncompressed)

HLE_STDCALL(D3DDevice_SetTextureState_TexCoordIndex, 2) {
    g_state.tss[a[0] & 3][gpu::kTssTexCoordIndex] = a[1];
    W32(kTextureState + ((a[0] & 3) * kStageStates + gpu::kTssTexCoordIndex) * 4, a[1]);
    return 0;
}
HLE_STDCALL(D3DDevice_SetTextureState_BumpEnv, 3) {
    if (a[1] >= gpu::kTssBumpEnvMat00 && a[1] <= gpu::kTssBumpEnvLOffset) g_state.tss[a[0] & 3][a[1]] = a[2];
    return 0;
}
HLE_STDCALL(D3DDevice_SetTextureState_BorderColor, 2) {
    g_state.tss[a[0] & 3][gpu::kTssBorderColor] = a[1];
    return 0;
}
HLE_STDCALL(D3DDevice_SetTextureState_ColorKeyColor, 2) {
    g_state.tss[a[0] & 3][gpu::kTssColorKeyColor] = a[1];
    return 0;
}
HLE_STDCALL(D3DDevice_SetTextureStageStateNotInline, 3) {
    uint32_t stage = a[0] & 3, type = a[1], v = a[2];
    if (type < gpu::kTssCount) {
        g_state.tss[stage][type] = v;
        W32(kTextureState + (stage * kStageStates + type) * 4, v);
    }
    return 0;
}

// the XDK's transform types: view 0, projection 1, texture 2-5, world 6-9; the renderer keeps Direct3D 8's numbers
// (view 2, projection 3, texture 16, world 256). The device copy is what GetTransform reads.
HLE_STDCALL(D3DDevice_SetTransform, 2) {
    uint32_t t = a[0];
    if (t >= 10) return 0;
    std::memcpy(HostPtr(kDevice + kDevTransforms + t * 64), HostPtr(a[1]), 64);
    uint32_t pc = t < 2 ? t + 2 : t < 6 ? t + 14 : t + 250;
    std::memcpy(g_state.transforms[pc].data(), HostPtr(a[1]), 64);
    return 0;
}

HLE_STDCALL(D3DDevice_SetMaterial, 1) {
    std::memcpy(g_state.material.data(), HostPtr(a[0]), 17 * 4);
    return 0;
}

// Preserve the legacy before-original shadow. PBR also observes the REAL original outcome afterward;
// a failed original allocation or implicit LightEnable default must never certify physical scene input.
HLE_ABI_(hle_abi_D3DDevice_SetLight, hle_args_D3DDevice_SetLight, "D3DDevice_SetLight", ::hle::Map{},
         !hle_abi_D3DDevice_SetLight::map.mapped || hle_abi_D3DDevice_SetLight::map.n == 2)
X86_EXTERN_FUNC(GUEST_ORIG(D3DDevice_SetLight));
X86_FUNC(GUEST(D3DDevice_SetLight)) {
    const hle_args_D3DDevice_SetLight args{ctx};
    const uint32_t i = args[0], l = args[1];
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    bool copied = false;
    if (i < 8 && l) {
        std::memcpy(g_state.lights[i].data(), HostPtr(l), 26 * 4);
        copied = true;
    }
    g_state.pbr_active_light_records_verified = false;
    GUEST_ORIG(D3DDevice_SetLight)(ctx, base);
    g_state.pbr_light_proof.SetLightResult(i, copied, ctx.eax);
}
HLE_ABI_(hle_abi_D3DDevice_LightEnable, hle_args_D3DDevice_LightEnable, "D3DDevice_LightEnable", ::hle::Map{},
         !hle_abi_D3DDevice_LightEnable::map.mapped || hle_abi_D3DDevice_LightEnable::map.n == 2)
X86_EXTERN_FUNC(GUEST_ORIG(D3DDevice_LightEnable));
X86_FUNC(GUEST(D3DDevice_LightEnable)) {
    const hle_args_D3DDevice_LightEnable args{ctx};
    const uint32_t i = args[0], on = args[1];
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    if (i < 8) {
        g_state.light_enable = on ? g_state.light_enable | 1u << i : g_state.light_enable & ~(1u << i);
    }
    g_state.pbr_active_light_records_verified = false;
    g_state.pbr_light_proof.BeginEnable(i);
    GUEST_ORIG(D3DDevice_LightEnable)(ctx, base);
    g_state.pbr_light_proof.EnableResult(i, on, ctx.eax);
}

// resources in use

HLE_STDCALL(D3DDevice_SetTexture, 2) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    uint32_t stage = a[0] & 3, tex = a[1];
    uint32_t old = Dev(kDevTextures + stage * 4);
    // bound again: the AddRef and Release would cancel out
    bool same = tex == old;
    if (!same) AddRef(ctx, tex);
    SetDev(kDevTextures + stage * 4, tex);
    if (!same) Release(ctx, old);
    g_state.textures[stage] = tex;
    W32(kDirtyFlags, R32(kDirtyFlags) | XDK_DirtyFlags_Texture);
    return 0;
}

// SetPalette(stage, palette): the colours a P8 texture of the stage looks up
HLE_STDCALL(D3DDevice_SetPalette, 2) {
    XDK_REQUIRE("d3d", XDK_KEY(CDevice_Palettes));
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    uint32_t stage = a[0] & 3, pal = a[1];
    uint32_t old = Dev(kDevPalettes + stage * 4);
    AddRef(ctx, pal);
    SetDev(kDevPalettes + stage * 4, pal);
    Release(ctx, old);
    g_state.palettes[stage] = pal;
    return 0;
}

// SwitchTexture(method, data, format), fastcall: a swizzled texture of the bound one's shape replaces it. It is
// bound through a header of the stage's own holding the bound texture's fields with the new data and format.
HLE_FASTCALL(D3DDevice_SwitchTexture, 1) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    static uint32_t headers[4];
    // texture methods from 0x1B00, 0x40 per stage
    uint32_t stage = (((a[0] & 0x1FFC) - 0x1B00) >> 6) & 3, cur = g_state.textures[stage];
    uint32_t& h = headers[stage];
    if (!h) h = mem::AllocSystem(20);
    if (cur && cur != h) std::memcpy(HostPtr(h), HostPtr(cur), 20);
    W32(h + 4, a[1]);
    W32(h + 12, a[2]);
    g_state.textures[stage] = h;
    return 0;
}

HLE_STDCALL(D3DDevice_SetStreamSource, 3) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    uint32_t n = a[0] & 15, vb = a[1], stride = a[2];
    uint32_t old = R32(kStreams + n * kStreamSize + XDK_Stream_Buffer);
    bool same = vb == old;
    if (!same) AddRef(ctx, vb);
    W32(kStreams + n * kStreamSize + XDK_Stream_Stride, stride);
    W32(kStreams + n * kStreamSize + XDK_Stream_Buffer, vb);
    if (!same) Release(ctx, old);
    g_state.streams[n] = {vb, stride};
    return 0;
}

HLE_STDCALL(D3DDevice_SetIndices, 2) {
    XDK_REQUIRE("d3d", XDK_DATA(D3D_IndexData));
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    uint32_t ib = a[0];
    uint32_t old = Dev(kDevIndexBuffer);
    bool same = ib == old;
    if (!same) AddRef(ctx, ib);
    W32(kIndexData, ib ? R32(ib + 4) : 0);
    SetDev(kDevIndexBuffer, ib);
    SetDev(kDevBaseVertex, a[1]);
    if (!same) Release(ctx, old);
    g_state.index_buffer = ib;
    g_state.index_data = ib ? R32(ib + 4) : 0;
    g_state.base_vertex = a[1];
    return 0;
}

// shaders

HLE_STDCALL(D3DDevice_CreateVertexShader, 4) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    uint32_t decl = a[0], func = a[1], out = a[2];
    auto vs = std::make_shared<gpu::VertexShader>();
    if (decl) {
        for (uint32_t i = 0; i < 1024; i++) {
            uint32_t t = R32(decl + i * 4);
            vs->decl.push_back(t);
            if (t == 0xFFFFFFFF) break;
        }
    }
    if (func) {
        uint32_t hdr = R32(func);
        vs->program_type = hdr & 0xFFFF;
        uint32_t n = hdr >> 16;
        for (uint32_t i = 0; i < n * 4; i++) vs->program.push_back(R32(func + 4 + i * 4));
    }
    gpu::ParseDeclaration(*vs);
    uint32_t rec = mem::AllocSystem(16, 16);
    vs->handle = rec | 1;
    g_vs[vs->handle] = vs;
    W32(out, vs->handle);
    LOG_FIRST(64, "d3d: vertex shader %08X, %zu decl tokens, %zu instructions, type %X", vs->handle, vs->decl.size(),
              vs->program.size() / 4, vs->program_type);
    return 0;
}

HLE_STDCALL(D3DDevice_DeleteVertexShader, 1) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    auto it = g_vs.find(a[0]);
    if (it != g_vs.end()) {
        mem::FreeSystem(a[0] & ~1u);
        g_vs.erase(it);
    }
    return 0;
}

// (handle, data, size in and out): the program as it was given, the size alone without data or room
HLE_STDCALL(D3DDevice_GetVertexShaderFunction, 3) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    auto it = g_vs.find(a[0]);
    uint32_t words = it == g_vs.end() ? 0 : uint32_t(it->second->program.size());
    uint32_t need = words ? 4 + words * 4 : 0, room = R32(a[2]);
    W32(a[2], need);
    if (need > room) return a[1] ? 0x88760867 : 0;  // D3DERR_MOREDATA
    if (!a[1] || !need) return 0;
    W32(a[1], it->second->program_type | words / 4 << 16);
    for (uint32_t i = 0; i < words; i++) W32(a[1] + 4 + i * 4, it->second->program[i]);
    return 0;
}

HLE_STDCALL(D3DDevice_SetVertexShader, 1) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Walk(Dev(kDevPush));  // commands pushed before run with the shader before
    uint32_t h = a[0];
    g_state.vs_handle = h;
    g_state.vs = nullptr;
    g_sel = Selection{};
    if (h & 1) {
        auto it = g_vs.find(h);
        if (it != g_vs.end()) {
            g_state.vs = it->second;
            // constants declared with the shader load when it is selected
            for (auto& [reg, v] : it->second->constants)
                if (reg < 192) g_state.vsc[reg] = v;
            // like the XDK's (LoadVertexShader and SelectVertexShader at slot 0): the program goes to slot 0 and
            // runs from there, so a later load over it changes what the following draws run
            const auto& program = it->second->program;
            if (!program.empty()) {
                WriteSlots(0, program.data(), uint32_t(program.size() / 4));
                g_sel.on = true;
                g_sel.start = 0;
                g_sel.handle = h;
                g_sel.decl = it->second;
                g_sel.gen = g_slot_gen;
            }
        } else {
            LOG_FIRST(8, "d3d: SetVertexShader with unknown handle %08X", h);
        }
    }
    return 0;
}

namespace {
// Hardware register numbers already include the D3D +96 bias. The non-Fast one/four setters also update the
// XDK's guest-side constant shadow, which the original state-block capture code reads. Fast setters deliberately
// leave that shadow alone: the original non-Fast bulk wrapper performs its own conditional shadow copy.
void SetVertexConstants(uint32_t reg, uint32_t src, uint32_t dwords, bool shadow) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    FlushPush();
    if (reg >= 192) return;
    if (!dwords) {
        g_const_load = reg;
        g_const_part = 0;
        return;  // The original empty bulk setter still emits SET_TRANSFORM_CONSTANT_LOAD.
    }
    if (!src) return;
    dwords = std::min(dwords, (192u - reg) * 4u);
    const size_t bytes = size_t(dwords) * 4;
    if (uint64_t(src) + bytes > (uint64_t(1) << 32)) return;
    std::memcpy(g_state.vsc[reg].data(), HostPtr(src), bytes);
#if HAS_D3D_VertexShaderConstants
    if (shadow) std::memmove(HostPtr(SYM(D3D_VertexShaderConstants) + reg * 16), HostPtr(src), bytes);
#else
    (void)shadow;  // Older image configs retain their existing behavior until the shadow symbol is supplied.
#endif
    // A later raw constant payload can continue without another LOAD; match the cursor left by the XDK's pushes.
    g_const_load = reg + dwords / 4;
    g_const_part = dwords % 4;
}
}  // namespace

// Older inline constants are consumed before each direct update, in their original order.
HLE_FASTCALL(D3DDevice_SetVertexShaderConstant1Fast, 0) {
    SetVertexConstants(a[0], a[1], 4, false);
    return 0;
}

HLE_FASTCALL(D3DDevice_SetVertexShaderConstant1, 0) {
    SetVertexConstants(a[0], a[1], 4, true);
    return 0;
}

HLE_FASTCALL(D3DDevice_SetVertexShaderConstant4, 0) {
    SetVertexConstants(a[0], a[1], 16, true);
    return 0;
}

HLE_FASTCALL(D3DDevice_SetVertexShaderConstantNotInlineFast, 1) {
    SetVertexConstants(a[0], a[1], a[2], false);
    return 0;
}

// LoadVertexShader(handle, address): the shader's program into the program slots from address, not run yet
HLE_STDCALL(D3DDevice_LoadVertexShader, 2) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Walk(Dev(kDevPush));
    auto it = g_vs.find(a[0]);
    if (it == g_vs.end() || it->second->program.empty()) {
        LOG_FIRST(8, "d3d: LoadVertexShader %08X at slot %u: %s", a[0], a[1],
                  it == g_vs.end() ? "unknown handle" : "the shader has no program");
        return 0;
    }
    const auto& program = it->second->program;
    LOG_FIRST(16, "d3d: LoadVertexShader %08X, %zu instructions at slot %u", a[0], program.size() / 4, a[1]);
    WriteSlots(a[1], program.data(), uint32_t(program.size() / 4));
    return 0;
}

// LoadVertexShaderProgram(function, address): microcode as CreateVertexShader takes it (a header of type and
// instruction count, then 4 dwords an instruction) into the slots from address
HLE_STDCALL(D3DDevice_LoadVertexShaderProgram, 2) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Walk(Dev(kDevPush));
    uint32_t func = a[0];
    if (!func) return 0;
    uint32_t hdr = R32(func), count = std::min(hdr >> 16, kProgramSlots);
    std::vector<uint32_t> words(size_t(count) * 4);
    for (uint32_t i = 0; i < count * 4; i++) words[i] = R32(func + 4 + i * 4);
    LOG_FIRST(16, "d3d: LoadVertexShaderProgram type %X, %u instructions at slot %u", hdr & 0xFFFF, hdr >> 16, a[1]);
    WriteSlots(a[1], words.data(), count);
    return 0;
}

// SelectVertexShader(handle, address): the program at slot address runs with the declaration of handle, or with
// the declaration in use when handle is 0
HLE_STDCALL(D3DDevice_SelectVertexShader, 2) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Walk(Dev(kDevPush));
    LOG_FIRST(16, "d3d: SelectVertexShader %08X at slot %u", a[0], a[1]);
    SelectProgram(a[0], a[1]);
    return 0;
}
HLE_STDCALL(D3DDevice_SetShaderConstantMode, 1) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    g_own_constants = a[0] & 0x10;  // D3DSCM_NORESERVEDCONSTANTS
    UpdateViewportConstants();
    return 0;
}

// the XDK's pixel shader object (0x415A90): reference count, owned, the definition's address and its copy. Guest
// code reads it: the state-block capture adds a reference (0x416A63) and its release goes through DeletePixelShader.
constexpr uint32_t kPsRefCount = 0, kPsOwned = 4, kPsDefinition = 8, kPsCopy = 0xC;
constexpr uint32_t kPsObjectSize = kPsCopy + sizeof(gpu::PixelShader::def);

HLE_STDCALL(D3DDevice_CreatePixelShader, 2) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    auto ps = std::make_shared<gpu::PixelShader>();
    std::memcpy(ps->def, HostPtr(a[0]), sizeof(ps->def));
    uint32_t rec = mem::AllocSystem(kPsObjectSize, 16);
    W32(rec + kPsRefCount, 1);
    W32(rec + kPsOwned, 1);
    W32(rec + kPsDefinition, rec + kPsCopy);
    std::memcpy(HostPtr(rec + kPsCopy), ps->def, sizeof(ps->def));
    ps->handle = rec;
    g_ps[rec] = ps;
    W32(a[1], rec);
    return 0;
}

// a release: the object goes with its last reference, as the XDK's
HLE_STDCALL(D3DDevice_DeletePixelShader, 1) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    if (!g_ps.count(a[0])) return 0;
    const uint32_t refs = R32(a[0] + kPsRefCount) - 1;
    W32(a[0] + kPsRefCount, refs);
    if (!refs && g_ps.erase(a[0])) mem::FreeSystem(a[0]);
    return 0;
}

HLE_STDCALL(D3DDevice_SetPixelShader, 1) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    // the device keeps the current one like the XDK's: state-block capture (0x4169D0) copies it, and D3DX records
    // every pass's PixelShader into a state block; left 0, each replay set no shader
    if (XDK_CDevice_PixelShader != xdk::kUnknown) SetDev(XDK_CDevice_PixelShader, a[0]);
    g_state.ps = nullptr;
    if (!a[0]) return 0;
    auto it = g_ps.find(a[0]);
    if (it != g_ps.end()) {
        g_state.ps = it->second;
    } else if (XDK_PixelShader_Definition != xdk::kUnknown) {
        // made by library code the title inlined: the object points at its definition, handles are reused
        static std::unordered_map<uint32_t, std::shared_ptr<gpu::PixelShader>> made;
        const void* def = HostPtr(R32(a[0] + XDK_PixelShader_Definition));
        auto& ps = made[a[0]];
        if (!ps || std::memcmp(ps->def, def, sizeof(ps->def))) {
            ps = std::make_shared<gpu::PixelShader>();
            ps->handle = a[0];
            std::memcpy(ps->def, def, sizeof(ps->def));
        }
        g_state.ps = ps;
    } else {
        LOG_FIRST(8, "d3d: SetPixelShader with unknown handle %08X", a[0]);
        return 0;
    }
    // the definition loads the combiner state like the XDK pushes it
    const uint32_t* d = g_state.ps->def;
    for (int i = 0; i < 8; i++) {
        SetSimple(gpu::kMPsAlphaInputs + i * 4, d[0 + i]);
        SetSimple(gpu::kMPsConstant0 + i * 4, d[10 + i]);
        SetSimple(gpu::kMPsConstant1 + i * 4, d[18 + i]);
        SetSimple(gpu::kMPsAlphaOutputs + i * 4, d[26 + i]);
        SetSimple(gpu::kMPsRgbInputs + i * 4, d[34 + i]);
        SetSimple(gpu::kMPsRgbOutputs + i * 4, d[45 + i]);
    }
    SetSimple(gpu::kMPsFinalAbcd, d[8]);
    SetSimple(gpu::kMPsFinalEfgh, d[9]);
    SetSimple(gpu::kMPsCompareMode, d[42]);
    SetSimple(gpu::kMPsFinalConstant0, d[43]);
    SetSimple(gpu::kMPsFinalConstant1, d[44]);
    SetSimple(gpu::kMPsCombinerCount, d[53]);
    SetSimple(gpu::kMPsDotMapping, d[55]);
    SetSimple(gpu::kMPsInputTexture, d[56]);
    SetComplex(gpu::kRsPsTextureModes, d[54]);
    return 0;
}

HLE_STDCALL(D3DDevice_SetPixelShaderConstant, 3) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    uint32_t reg = a[0], src = a[1], n = a[2];
    for (uint32_t i = 0; i < n && reg + i < 16; i++) std::memcpy(g_state.psc[reg + i].data(), HostPtr(src + i * 16), 16);
    gpu::ApplyPixelConstants(g_state, reg, n);
    gpu::MarkAllSimple();  // it writes the combiner constants into simple
    return 0;
}

// drawing

// This observation is tied to the exact 5849 image whose original SetLight/LightEnable instructions
// establish these fields. Other images remain unsupported; no generic guessed device layout is read.
bool PbrCommitted(uint32_t address, uint32_t bytes) {
    const uint64_t end = uint64_t(address) + bytes;
    if (!bytes || address < 0x10000 || end > (uint64_t(1) << 32)) return false;
    for (uint64_t page = address & ~uint64_t(mem::kPage - 1); page < end; page += mem::kPage)
        if (!mem::IsCommitted(uint32_t(page))) return false;
    return true;
}
void ObservePbrActiveLights() {
    g_state.pbr_active_light_records_verified = false;
    if (!gpu::pbr::observation::GuestProofRequested()) return;
    const auto proof = g_state.pbr_light_proof.Snapshot(g_state.light_enable, true);
    if (!proof.light_writes_after_success || !proof.light_index_coverage || !proof.stateblock_valid) return;
    if (kDevice != 0x422D80 || kDevicePtr != 0x422D78 ||
        SYM(D3DDevice_SetLight) != 0x411FB0 || SYM(D3DDevice_LightEnable) != 0x412270 ||
        !PbrCommitted(kDevicePtr, 4) || R32(kDevicePtr) != kDevice || !PbrCommitted(kDevice + 0x7A4, 0x2C)) return;
    // A light-table match alone cannot establish material/transform equivalence after an original
    // stateblock replay. SetTransform mirrors these exact device matrices; SetMaterial historically
    // only updates the host shadow, so a mismatch deliberately stays on the legacy path.
    if (XDK_CDevice_Transforms != 0xC60 || !PbrCommitted(kDevice + 0xC60, 10 * 64) ||
        !PbrCommitted(kDevice + 0xF00, 17 * 4)) return;
    if (std::memcmp(HostPtr(kDevice + 0xF00), g_state.material.data(), 17 * 4)) return;
    for (const auto [original, host] : {std::pair{0u,2u},std::pair{1u,3u},std::pair{6u,256u}})
        if (std::memcmp(HostPtr(kDevice + 0xC60 + original * 64), g_state.transforms[host].data(), 64)) return;
    gpu::pbr::light_snapshot::Table table;
    table.base = R32(kDevice + 0x7A4); table.capacity = R32(kDevice + 0x7A8);
    table.packed_slot_types = R32(kDevice + 0x7CC);
    for (unsigned slot = 0; slot < 8; ++slot) table.active[slot] = R32(kDevice + 0x7AC + slot * 4);
    g_state.pbr_active_light_records_verified = gpu::pbr::light_snapshot::Matches(
        table, g_state.lights, g_state.light_enable, PbrCommitted, [](uint32_t p) { return R32(p); });
}

HLE_STDCALL(D3DDevice_DrawVertices, 3) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Walk(Dev(kDevPush));  // a visibility test begun since the last draw counts this one
    SyncProgram();
    SyncDeferred();
    ObservePbrActiveLights();
    if (g_backend) g_backend->Draw(g_state, a[0], a[1], a[2], nullptr);
    return 0;
}

HLE_STDCALL(D3DDevice_DrawIndexedVertices, 3) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Walk(Dev(kDevPush));
    SyncProgram();
    SyncDeferred();
    ObservePbrActiveLights();
    const uint8_t* indices = a[2] ? static_cast<const uint8_t*>(HostPtr(a[2])) : nullptr;
    if (g_backend) g_backend->Draw(g_state, a[0], 0, a[1], indices);
    return 0;
}

// DrawVerticesUP(prim, count, vertices, stride): the vertices are copied to a ring the render thread reads, stream 0
// points there through a vertex buffer header for the draw. A half of the ring is written again once the draws
// that read it are done.
namespace {
constexpr uint32_t kUpRing = 4u << 20, kUpHalf = kUpRing / 2;
uint32_t g_up_ring = 0, g_up_pos = 0, g_up_header = 0, g_up_fence[2] = {};

uint32_t UpVertices(const void* src, uint32_t bytes) {
    if (!g_up_ring) {
        g_up_ring = mem::AllocContiguous(kUpRing, 0, 0xFFFFFFFF, 0x1000);
        g_up_header = mem::AllocSystem(20);
        std::memset(HostPtr(g_up_header), 0, 20);
        W32(g_up_header, 1);  // a vertex buffer, one reference
    }
    bytes = std::min(bytes, kUpHalf);
    uint32_t room = (bytes + 63) & ~63u;
    uint32_t pos = g_up_pos + room > kUpRing ? 0 : g_up_pos;
    uint32_t from = g_up_pos ? (g_up_pos - 1) / kUpHalf : 1, to = (pos + room - 1) / kUpHalf;
    if (to != from) {
        g_up_fence[from] = g_backend->Fence();
        g_backend->WaitFence(g_up_fence[to]);
    }
    std::memcpy(HostPtr(g_up_ring + pos), src, bytes);
    g_up_pos = pos + room;
    W32(g_up_header + 4, mem::PhysicalFromVirtual(g_up_ring + pos));
    return g_up_header;
}

void DrawUp(uint32_t prim, uint32_t count, const void* vertices, uint32_t stride) {
    if (!g_backend || !count || !stride) return;
    SyncDeferred();
    ObservePbrActiveLights();
    auto stream = g_state.streams[0];
    g_state.streams[0] = {UpVertices(vertices, count * stride), stride};
    g_backend->Draw(g_state, prim, 0, count, nullptr);
    g_state.streams[0] = stream;
}

// bytes per vertex of the bound format: stream 0 of the FVF or of the shader's declaration
uint32_t VertexBytes() {
    if (g_state.vs_handle & 1) return g_state.vs ? g_state.vs->stream_stride[0] : 0;
    gpu::VertexShader fvf;
    gpu::FvfInputs(g_state.vs_handle, fvf);
    return fvf.stream_stride[0];
}

uint32_t g_push_begin = 0;
}  // namespace

// Begin/End immediate mode. Titles inline Begin and the vertex data calls, so their commands are read from the scratch buffer.
namespace d3d {
namespace {
uint32_t g_im_prim = 0, g_im_mask = 0;  // open primitive, slots it wrote
// 16 slots of x y z w: the NV2A's inline attribute values, which also stand in for registers a draw's vertices lack
float g_im_attr[64] = {0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 1, 1, 1, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1,
                       0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1};
std::vector<float> g_im_vertices;       // every slot per vertex

// visibility tests: depth passing pixels are counted while enabled, a report gets the count since the clear,
// at a physical address in the first 16 MB
void ApplyVisibilityMethod(uint32_t m, uint32_t v) {
    constexpr uint32_t kReportAddressMask = 0xFFFFFF;
    if (m == gpu::kMCountEnable) SetSimple(gpu::kMCountEnable, v);
    if (!g_backend) return;
    if (m == gpu::kMClearReport) g_backend->ClearReport();
    if (m == gpu::kMGetReport) g_backend->GetReport(mem::VirtualFromPhysical(v & kReportAddressMask));
}

// pretransformed (XYZRHW) vertices: packed the way the bound FVF lays them out, with the slots the primitive wrote
// that the FVF lacks added (colours as D3DCOLOR, texture coordinates of the FVF's sizes), so the renderer takes
// them as screen space vertices like any XYZRHW draw. A slot the FVF has that the primitive did not write carries
// the inline value it had at each vertex.
void DrawPretransformed(uint32_t prim, uint32_t count, uint32_t handle) {
    static std::vector<uint8_t> packed;
    uint32_t fvf = handle;
    if (g_im_mask >> 3 & 1) fvf |= 0x40;  // D3DFVF_DIFFUSE
    if (g_im_mask >> 4 & 1) fvf |= 0x80;  // D3DFVF_SPECULAR
    if (g_im_mask >> 6 & 1) fvf |= 0x20;  // D3DFVF_PSIZE
    uint32_t tex = fvf >> 8 & 15;
    for (uint32_t t = 0; t < 4; t++)
        if (g_im_mask >> (9 + t) & 1) tex = std::max(tex, t + 1);
    fvf = (fvf & ~0xF00u) | tex << 8;
    gpu::VertexShader layout;
    gpu::FvfInputs(fvf, layout);
    uint32_t stride = layout.stream_stride[0];
    if (!stride) return;
    packed.assign(size_t(count) * stride, 0);
    auto unorm8 = [](float f) { return uint32_t((f > 0 ? (f < 1 ? f : 1) : 0) * 255.0f + 0.5f); };  // NaN is 0
    for (uint32_t v = 0; v < count; v++)
        for (uint32_t r = 0; r < 16; r++) {
            const auto& in = layout.inputs[r];
            if (in.stream != 0) continue;
            const float* src = &g_im_vertices[size_t(v) * 64 + r * 4];
            uint8_t* dst = packed.data() + size_t(v) * stride + in.offset;
            if (in.type == 0x40) {  // D3DCOLOR: bytes B G R A of the register's r g b a
                uint32_t c = unorm8(src[2]) | unorm8(src[1]) << 8 | unorm8(src[0]) << 16 | unorm8(src[3]) << 24;
                std::memcpy(dst, &c, 4);
            } else if ((in.type & 15) == 2) {  // FLOAT1 to FLOAT4
                std::memcpy(dst, src, std::min<uint32_t>(in.type >> 4 & 15, 4) * 4);
            }
        }
    g_state.vs_handle = fvf;
    DrawUp(prim, count, packed.data(), stride);
    g_state.vs_handle = handle;
}

// draws the open primitive: the slots it wrote are FLOAT4 inputs of the bound program, or of the fixed pipeline
void EndPrimitive() {
    struct Made {
        std::shared_ptr<gpu::VertexShader> from, vs;
    };
    // the render thread tells shaders apart by pointer: one object per program and slot set, the program kept alive
    static std::map<std::pair<gpu::VertexShader*, uint32_t>, Made> made;
    static std::vector<float> packed;
    uint32_t prim = g_im_prim, count = uint32_t(g_im_vertices.size() / 64);
    g_im_prim = 0;
    if (!prim || !count) return;
    SyncProgram();
    // registers the vertices lack read the inline values as they are at this end, not at the end of the walk
    std::memcpy(g_state.vattr.data(), g_im_attr, sizeof(g_im_attr));
    uint32_t handle = g_state.vs_handle;
    if (!(handle & 1) && (handle & 0xE) == 4) {  // D3DFVF_XYZRHW
        DrawPretransformed(prim, count, handle);
        return;
    }
    auto bound = g_state.vs, from = handle & 1 ? bound : nullptr;
    Made& m = made[{from.get(), g_im_mask}];
    if (!m.vs) {
        m.from = from;
        m.vs = std::make_shared<gpu::VertexShader>();
        if (from) {
            m.vs->program = from->program;
            m.vs->program_type = from->program_type;
        }
        for (uint32_t s = 0; s < 16; s++)
            if (g_im_mask >> s & 1) {
                m.vs->inputs[s] = {0, m.vs->stream_stride[0], 0x42};
                m.vs->stream_stride[0] += 16;
            }
    }
    packed.clear();
    for (uint32_t v = 0; v < count; v++)
        for (uint32_t s = 0; s < 16; s++)
            if (g_im_mask >> s & 1) packed.insert(packed.end(), &g_im_vertices[v * 64 + s * 4], &g_im_vertices[v * 64 + s * 4] + 4);
    g_state.vs = m.vs;
    g_state.vs_handle = handle | 1;
    DrawUp(prim, count, packed.data(), m.vs->stream_stride[0]);
    g_state.vs = bound;
    g_state.vs_handle = handle;
}

// a begin/end or vertex data method opens, feeds or draws the immediate primitive; false for any other method
bool ApplyImmediate(uint32_t m, uint32_t v) {
    float f;
    float* a = g_im_attr;
    bool last = true;  // the slot's last component
    std::memcpy(&f, &v, 4);
    if (m == 0x17FC) {  // begin/end, 0 ends
        if (v) {
            g_im_prim = v;
            g_im_mask = 0;
            g_im_vertices.clear();
        } else {
            EndPrimitive();
        }
        return true;
    } else if (m >= 0x1880 && m < 0x1900) {  // 2 floats, z = 0 and w = 1
        a += (m - 0x1880) / 8 * 4;
        last = (m - 0x1880) / 4 & 1;
        a[last] = f;
        a[2] = 0;
        a[3] = 1;
    } else if (m >= 0x1940 && m < 0x1980) {  // 4 bytes
        a += (m - 0x1940) / 4 * 4;
        for (int c = 0; c < 4; c++) a[c] = (v >> c * 8 & 0xFF) / 255.0f;
    } else if (m >= 0x1A00 && m < 0x1B00) {  // 4 floats
        a += (m - 0x1A00) / 16 * 4;
        last = ((m - 0x1A00) / 4 & 3) == 3;
        a[(m - 0x1A00) / 4 & 3] = f;
    } else if (m >= 0x1500 && m < 0x150C) {  // position, 3 floats and w = 1
        last = m == 0x1508;
        a[(m - 0x1500) / 4] = f;
        a[3] = 1;
    } else if (m >= 0x1518 && m < 0x1528) {  // position, 4 floats
        last = m == 0x1524;
        a[(m - 0x1518) / 4] = f;
    } else {
        return false;
    }
    if (!g_im_prim) return true;
    g_im_mask |= 1u << (a - g_im_attr) / 4;
    if (a == g_im_attr && last) g_im_vertices.insert(g_im_vertices.end(), g_im_attr, g_im_attr + 64);
    return true;
}

// Vertex constants a title pushes itself (007 does a skinned mesh's ambient and material colour so): a load
// register, then four dwords per register from it on
constexpr uint32_t kConstantLoad = 0x1EA4;
constexpr uint32_t kConstantData = 0x0B80;
constexpr uint32_t kConstantDataEnd = 0x0C00;
constexpr uint32_t kConstantRegisters = 192;
uint32_t g_constant_register = 0;
uint32_t g_constant_component = 0;

// a constant load or data method updates the vertex constants; false for any other method
bool ApplyConstant(uint32_t m, uint32_t v) {
    if (m == kConstantLoad) {
        g_constant_register = v;
        g_constant_component = 0;
        return true;
    }
    if (m < kConstantData || m >= kConstantDataEnd) return false;
    if (g_constant_register < kConstantRegisters) {
        std::memcpy(&g_state.vsc[g_constant_register][g_constant_component], &v, 4);
    }
    if (++g_constant_component == 4) {
        g_constant_component = 0;
        g_constant_register++;
    }
    return true;
}

// reads the commands pushed since the last call: vertex data between a begin and an end is drawn, visibility tests
// are run, vertex program constants and program uploads and starts are applied (ApplyTransformMethod), the rest
// is skipped
void Walk(uint32_t end) {
    if (!g_walk || end < g_walk || end > g_scratch_push + g_scratch_size) return;
    for (uint32_t p = g_walk; p + 4 <= end;) {
        // count in bits 18 to 28, bit 30 keeps the method for every value
        uint32_t head = R32(p), method = head & 0x1FFC, n = (head >> 18) & 0x7FF;
        p += 4;
        if ((head & 0xA0030003) || n * 4 > end - p) break;  // anything but a method header
        for (uint32_t i = 0; i < n; i++, p += 4) {
            uint32_t m = head & 0x40000000 ? method : method + i * 4;
            uint32_t v = R32(p);
            ApplyVisibilityMethod(m, v);
            if (ApplyTransformMethod(m, v)) continue;  // SET_TRANSFORM_CONSTANT(_LOAD), SET_TRANSFORM_PROGRAM(_LOAD/_START)
            ApplyImmediate(m, v);
        }
    }
    g_walk = end;
    std::memcpy(g_state.vattr.data(), g_im_attr, sizeof(g_im_attr));
}
}  // namespace
}  // namespace d3d

HLE_STDCALL(D3DDevice_DrawVerticesUP, 4) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Walk(Dev(kDevPush));
    SyncProgram();
    DrawUp(a[0], a[1], HostPtr(a[2]), a[3]);
    return 0;
}

HLE_STDCALL(D3DDevice_DrawIndexedVerticesUP, 5) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    uint32_t count = a[1], indices = a[2], vertices = a[3], stride = a[4];
    if (!count) return 0;
    if (!indices || !vertices || !stride || stride > kUpHalf || count > kUpHalf / stride) return 0x8876086Cu;
    // UP data may live on the guest stack. Expand the 16-bit indices before DrawUp copies it to its fenced ring.
    std::vector<uint8_t> packed(size_t(count) * stride);
    for (uint32_t i = 0; i < count; ++i) {
        uint64_t at = uint64_t(vertices) + uint64_t(R16(indices + i * 2)) * stride;
        if (at + stride > (1ull << 32)) return 0x8876086Cu;
        std::memcpy(packed.data() + size_t(i) * stride, HostPtr(uint32_t(at)), stride);
    }
    Walk(Dev(kDevPush));
    SyncProgram();
    DrawUp(a[0], count, packed.data(), stride);
    return 0;
}

// BeginPush(dwords) hands out room for raw hardware commands, EndPush(end) runs what the title wrote there:
// a primitive between two begin/end writes with its vertices inline or as vertex data, other methods as simple states.
HLE_STDCALL(D3DDevice_BeginPush, 1) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    g_push_begin = ScratchPush(a[0] * 4);
    return g_push_begin;
}

// a primitive whose vertices came as an inline array, in the bound vertex format
static void DrawInline(uint32_t prim, const std::vector<uint32_t>& vertices) {
    if (vertices.empty()) return;
    SyncProgram();  // the layout of a program selected in this push
    uint32_t stride = VertexBytes();
    uint32_t bytes = uint32_t(vertices.size()) * 4;
    if (stride && bytes % stride == 0) DrawUp(prim, bytes / stride, vertices.data(), stride);
    else LOG_FIRST(8, "d3d: EndPush primitive %u dropped, %u bytes of vertices of %u", prim, bytes, stride);
}

static void RunCommands(uint32_t begin, uint32_t end) {
    constexpr uint32_t kBeginEnd = 0x17FC, kInlineArray = 0x1818;
    // vertex array offsets and formats, then index elements and draw arrays up to the inline array
    constexpr uint32_t kArrayOffsets = 0x1720, kArrayFormatsEnd = 0x17A0, kArrayDraws = 0x1800;
    static std::vector<uint32_t> vertices;
    uint32_t prim = 0;
    for (uint32_t p = begin; begin && p + 4 <= end;) {
        // count in bits 18 to 28, bit 30 keeps the method for every value
        uint32_t head = R32(p), method = head & 0x1FFC, n = (head >> 18) & 0x7FF;
        p += 4;
        if ((head & 0xA0030003) || n * 4 > end - p) {  // anything but a method header
            LOG_FIRST(8, "d3d: pushed commands stop at %08X, %u bytes before the end", head, end - p);
            break;
        }
        for (uint32_t i = 0; i < n; i++, p += 4) {
            uint32_t m = head & 0x40000000 ? method : method + i * 4, v = R32(p);
            ApplyVisibilityMethod(m, v);
            if (ApplyTransformMethod(m, v)) continue;  // constants, program uploads and starts, in order
            if (m == kInlineArray) {
                if (prim) vertices.push_back(v);
            } else if (m == kBeginEnd) {
                if (!v && prim) DrawInline(prim, vertices);
                prim = v;
                vertices.clear();
                ApplyImmediate(m, v);
            } else if ((m >= kArrayOffsets && m < kArrayFormatsEnd) || (m >= kArrayDraws && m < kInlineArray)) {
                LOG_FIRST(16, "d3d: pushed commands draw from vertex arrays (method %04X), which is not run", m);
            } else if (!ApplyConstant(m, v) && !ApplyImmediate(m, v) && m >> 2 < g_state.simple.size()) {
                SetSimple(m, v);
            }
        }
    }
}

HLE_STDCALL(D3DDevice_EndPush, 1) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    RunCommands(g_push_begin, a[0]);
    g_push_begin = 0;
    ScratchPush();
    return a[0];
}

// (push buffer, fixup): its commands run as EndPush runs a title's, after the ones pushed before it. D3DPushBuffer is
// Common, Data, Lock, Size; the library runs Size less its last dword.
HLE_STDCALL(D3DDevice_RunPushBuffer, 2) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    uint32_t data = R32(a[0] + 4), size = R32(a[0] + 12);
    LOG_FIRST(8, "d3d: RunPushBuffer %08X, %u bytes, first command %08X", data, size, size ? R32(data) : 0);
    if (a[1]) LOG_FIRST(1, "d3d: RunPushBuffer fixups are not applied");
    Walk(Dev(kDevPush));
    if (size >= 4) RunCommands(data, data + size - 4);
    return 0;
}

// frame buffer compression tiles are the console's memory controller's business
HLE_STDCALL(D3DDevice_SetTile, 2) { return 0; }

// D3DTILE: flags, memory, size, pitch, z start tag, z offset
constexpr uint32_t kTileMemory = 4;
constexpr uint32_t kTileSize = 8;
constexpr uint32_t kTilePitch = 12;
constexpr uint32_t kTileBytes = 24;
// tiles the library's CreateDevice sets
constexpr uint32_t kBackBufferTile = 0;
constexpr uint32_t kDepthTile = 1;
// surface header fields
constexpr uint32_t kSurfaceData = 4;
constexpr uint32_t kSurfaceSize = 16;

// (index, tile): the frame buffer memory a title wraps in its own textures, as 007 does for a depth texture
HLE_STDCALL(D3DDevice_GetTile, 2) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    uint32_t tile = a[1];
    std::memset(HostPtr(tile), 0, kTileBytes);
    uint32_t surface = 0;
    if (a[0] == kBackBufferTile) surface = g_buffers[1];
    if (a[0] == kDepthTile) surface = g_depth;
    if (!surface) return 0;
    uint32_t size = R32(surface + kSurfaceSize);
    uint32_t pitch = ((size >> 24 & 0xFF) + 1) * 64;
    uint32_t height = (size >> 12 & 0xFFF) + 1;
    W32(tile + kTileMemory, mem::VirtualFromPhysical(R32(surface + kSurfaceData)));
    W32(tile + kTileSize, pitch * height);
    W32(tile + kTilePitch, pitch);
    return 0;
}

// the library's Begin pushes the start of the primitive, here it is opened
HLE_STDCALL(D3DDevice_Begin, 1) {
    XDK_REQUIRE("d3d", XDK_KEY(CDevice_Flags));
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Walk(Dev(kDevPush));
    g_im_prim = a[0];
    g_im_mask = 0;
    g_im_vertices.clear();
    SetDev(XDK_CDevice_Flags, Dev(XDK_CDevice_Flags) | 0x800);
    return 0;
}
// the library's End pushes the end of the primitive, here it is drawn
HLE_STDCALL(D3DDevice_End, 0) {
    XDK_REQUIRE("d3d", XDK_KEY(CDevice_Flags));
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Walk(Dev(kDevPush));
    EndPrimitive();
    SetDev(XDK_CDevice_Flags, Dev(XDK_CDevice_Flags) & ~0x1800u);
    return 0;
}
