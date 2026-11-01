// NVIDIA DLSS Super Resolution through NGX (dlss.h). This software contains source code provided by NVIDIA
// Corporation: the NGX helper headers fetched by cmake/DLSS.cmake.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "gpu/dlss.h"

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <string>

#include "core/log.h"

#if XBE_DLSS
#include <nvsdk_ngx_helpers.h>
#endif

namespace gpu::dlss {

const char* ModeName(Mode m) {
    switch (m) {
        case Mode::Off: return "Off";
        case Mode::Dlaa: return "DLAA (native)";
        case Mode::Quality: return "Quality";
        case Mode::Balanced: return "Balanced";
        case Mode::Performance: return "Performance";
        case Mode::UltraPerformance: return "Ultra Performance";
        default: return "?";
    }
}

uint32_t RenderPercent(Mode m) {
    switch (m) {
        case Mode::Dlaa: return 100;
        case Mode::Quality: return 67;
        case Mode::Balanced: return 58;
        case Mode::Performance: return 50;
        case Mode::UltraPerformance: return 33;
        default: return 100;
    }
}

namespace {

double Halton(uint32_t index, uint32_t base) {
    double f = 1, r = 0;
    for (; index; index /= base) {
        f /= base;
        r += f * (index % base);
    }
    return r;
}

std::string g_status = "not started";

#if XBE_DLSS
bool g_ready = false;
NVSDK_NGX_Parameter* g_params = nullptr;
NVSDK_NGX_Handle* g_feature = nullptr;
struct Created {
    uint32_t in_w = 0, in_h = 0, out_w = 0, out_h = 0, preset = ~0u;
    Mode mode = Mode::Off;
} g_created;

const char* Text(NVSDK_NGX_Result r) {
    static char buf[32];
    std::snprintf(buf, sizeof(buf), "0x%08X", unsigned(r));
    return buf;
}
#endif

}  // namespace

void Jitter(uint64_t frame, uint32_t in_w, uint32_t out_w, float& x, float& y) {
    // 8 phases times the square of the upscale ratio (NVIDIA's guidance), at least 8
    const double ratio = in_w ? double(out_w) / in_w : 1;
    const uint32_t phases = uint32_t(std::max(8.0, std::ceil(8.0 * ratio * ratio)));
    const uint32_t i = uint32_t(frame % phases) + 1;
    x = float(Halton(i, 2) - 0.5);
    y = float(Halton(i, 3) - 0.5);
}

#if XBE_DLSS

bool Init(ID3D12Device* device, const wchar_t* data_dir) {
    if (g_ready) return true;
    NVSDK_NGX_FeatureCommonInfo info{};
    const wchar_t* paths[] = {data_dir};
    info.PathListInfo.Path = paths;  // nvngx_dlss.dll beside the executable
    info.PathListInfo.Length = 1;
    info.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_OFF;
    NVSDK_NGX_Result r = NVSDK_NGX_D3D12_Init_with_ProjectID("8f6a8ab1-2b94-4c7e-9a3a-5b7d3c2e1f10", NVSDK_NGX_ENGINE_TYPE_CUSTOM,
                                                           "1.0", data_dir, device, &info);
    if (NVSDK_NGX_FAILED(r)) {
        g_status = std::string("NGX could not start (") + Text(r) + "): needs an NVIDIA RTX GPU and a current driver";
        LOGR("DLSS: %s", g_status.c_str());
        return false;
    }
    r = NVSDK_NGX_D3D12_GetCapabilityParameters(&g_params);
    if (NVSDK_NGX_FAILED(r) || !g_params) {
        g_status = std::string("NGX capability query failed (") + Text(r) + ")";
        LOGR("DLSS: %s", g_status.c_str());
        NVSDK_NGX_D3D12_Shutdown1(device);
        return false;
    }
    int available = 0, needs_driver = 0;
    unsigned min_major = 0, min_minor = 0;
    NVSDK_NGX_Parameter_GetI(g_params, NVSDK_NGX_Parameter_SuperSampling_Available, &available);
    NVSDK_NGX_Parameter_GetI(g_params, NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &needs_driver);
    NVSDK_NGX_Parameter_GetUI(g_params, NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMajor, &min_major);
    NVSDK_NGX_Parameter_GetUI(g_params, NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMinor, &min_minor);
    if (!available) {
        char buf[160];
        if (needs_driver) std::snprintf(buf, sizeof(buf), "needs NVIDIA driver %u.%u or newer", min_major, min_minor);
        else std::snprintf(buf, sizeof(buf), "not supported on this GPU (DLSS needs an NVIDIA RTX GPU)");
        g_status = buf;
        LOGR("DLSS: %s", g_status.c_str());
        NVSDK_NGX_D3D12_DestroyParameters(g_params);
        g_params = nullptr;
        NVSDK_NGX_D3D12_Shutdown1(device);
        return false;
    }
    g_ready = true;
    g_status = "ready (DLSS 4, SDK 310.9.16)";
    LOGR("DLSS: Super Resolution available");
    return true;
}

bool Available() { return g_ready; }
const char* Status() { return g_status.c_str(); }

void Shutdown(ID3D12Device* device) {
    if (!g_ready) return;
    if (g_feature) NVSDK_NGX_D3D12_ReleaseFeature(g_feature);
    g_feature = nullptr;
    if (g_params) NVSDK_NGX_D3D12_DestroyParameters(g_params);
    g_params = nullptr;
    NVSDK_NGX_D3D12_Shutdown1(device);
    g_ready = false;
}

bool Evaluate(ID3D12GraphicsCommandList* list, Mode mode, uint32_t preset, const Frame& f) {
    if (!g_ready || mode == Mode::Off || mode >= Mode::Count) return false;
    const bool remake = !g_feature || g_created.in_w != f.in_w || g_created.in_h != f.in_h ||
                        g_created.out_w != f.out_w || g_created.out_h != f.out_h || g_created.mode != mode ||
                        g_created.preset != preset;
    if (remake) {
        if (g_feature) {
            // the GPU may still be reading the old feature: the renderer waits for idle before a size change
            NVSDK_NGX_D3D12_ReleaseFeature(g_feature);
            g_feature = nullptr;
        }
        NVSDK_NGX_PerfQuality_Value q = NVSDK_NGX_PerfQuality_Value_MaxQuality;
        switch (mode) {
            case Mode::Dlaa: q = NVSDK_NGX_PerfQuality_Value_DLAA; break;
            case Mode::Quality: q = NVSDK_NGX_PerfQuality_Value_MaxQuality; break;
            case Mode::Balanced: q = NVSDK_NGX_PerfQuality_Value_Balanced; break;
            case Mode::Performance: q = NVSDK_NGX_PerfQuality_Value_MaxPerf; break;
            case Mode::UltraPerformance: q = NVSDK_NGX_PerfQuality_Value_UltraPerformance; break;
            default: break;
        }
        const char* hints[] = {NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA,
                               NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality,
                               NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced,
                               NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance,
                               NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance};
        for (const char* h : hints) NVSDK_NGX_Parameter_SetUI(g_params, h, preset);
        NVSDK_NGX_DLSS_Create_Params cp{};
        cp.Feature.InWidth = f.in_w;
        cp.Feature.InHeight = f.in_h;
        cp.Feature.InTargetWidth = f.out_w;
        cp.Feature.InTargetHeight = f.out_h;
        cp.Feature.InPerfQualityValue = q;
        // low resolution motion vectors (render size), not jittered; depth stored reversed; LDR colour
        cp.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_MVLowRes | NVSDK_NGX_DLSS_Feature_Flags_DepthInverted;
        const NVSDK_NGX_Result r = NGX_D3D12_CREATE_DLSS_EXT(list, 1, 1, &g_feature, g_params, &cp);
        if (NVSDK_NGX_FAILED(r)) {
            g_status = std::string("could not create DLSS (") + Text(r) + ")";
            LOGR("DLSS: %s for %ux%u -> %ux%u", g_status.c_str(), f.in_w, f.in_h, f.out_w, f.out_h);
            g_feature = nullptr;
            g_created = {};
            return false;
        }
        g_created = {f.in_w, f.in_h, f.out_w, f.out_h, preset, mode};
        g_status = "ready (DLSS 4, SDK 310.9.16)";
        LOGR("DLSS: %s %ux%u -> %ux%u, preset %u", ModeName(mode), f.in_w, f.in_h, f.out_w, f.out_h, preset);
    }
    NVSDK_NGX_D3D12_DLSS_Eval_Params e{};
    e.Feature.pInColor = f.color;
    e.Feature.pInOutput = f.output;
    e.pInDepth = f.depth;
    e.pInMotionVectors = f.motion;
    e.pInBiasCurrentColorMask = f.mask;
    e.InJitterOffsetX = f.jitter_x;
    e.InJitterOffsetY = f.jitter_y;
    e.InRenderSubrectDimensions = {f.in_w, f.in_h};
    e.InReset = (remake || f.reset) ? 1 : 0;
    e.InMVScaleX = float(f.in_w);  // motion is in uv units
    e.InMVScaleY = float(f.in_h);
    const NVSDK_NGX_Result r = NGX_D3D12_EVALUATE_DLSS_EXT(list, g_feature, g_params, &e);
    if (NVSDK_NGX_FAILED(r)) {
        static int logged = 0;
        if (logged++ < 4) LOGR("DLSS: evaluate failed (%s)", Text(r));
        return false;
    }
    return true;
}

#else  // built without the NGX SDK

bool Init(ID3D12Device*, const wchar_t*) {
    g_status = "this build has no DLSS (built with XBE_DLSS=OFF or without the SDK download)";
    return false;
}
bool Available() { return false; }
const char* Status() { return g_status.c_str(); }
void Shutdown(ID3D12Device*) {}
bool Evaluate(ID3D12GraphicsCommandList*, Mode, uint32_t, const Frame&) { return false; }

#endif

}  // namespace gpu::dlss
