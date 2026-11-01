#pragma once
// NVIDIA DLSS Super Resolution through NGX (cmake/DLSS.cmake: SDK 310.9.16, DLSS 4; never DLSS 5). The renderer
// jitters the frame's 3D draws, writes motion vectors and a HUD mask, and hands DLSS the frame at Present
// (d3d12.cpp, DlssFrame). Built without XBE_DLSS, or on a GPU without DLSS, Available() is false and the renderer
// keeps its own path.
#include <d3d12.h>

#include <cstdint>

namespace gpu::dlss {

// app::Settings::Graphics::dlss
enum class Mode : uint32_t { Off, Dlaa, Quality, Balanced, Performance, UltraPerformance, Count };
const char* ModeName(Mode m);
// render percent of the output each mode renders at (NGX's optimal ratios)
uint32_t RenderPercent(Mode m);

bool Init(ID3D12Device* device, const wchar_t* data_dir);  // once, on the render thread; logs why not
bool Available();
const char* Status();  // why DLSS is unavailable, or the driver's DLSS version, for the settings menu
void Shutdown(ID3D12Device* device);

struct Frame {
    ID3D12Resource* color = nullptr;   // render size, pixel/non-pixel shader resource state
    ID3D12Resource* depth = nullptr;   // R32_FLOAT, 1 near (reversed)
    ID3D12Resource* motion = nullptr;  // R16G16_FLOAT, previous minus current in uv units (scaled by mv_scale)
    ID3D12Resource* mask = nullptr;    // R8_UNORM: 1 where DLSS keeps the current frame (the HUD)
    ID3D12Resource* output = nullptr;  // output size, unordered access state
    uint32_t in_w = 0, in_h = 0, out_w = 0, out_h = 0;
    float jitter_x = 0, jitter_y = 0;  // pixels the frame's projection moved, render pixel units
    bool reset = false;
};
// preset: 0 the driver's choice, else NVSDK_NGX_DLSS_Hint_Render_Preset (10 J, 11 K, 12 L, 13 M)
bool Evaluate(ID3D12GraphicsCommandList* list, Mode mode, uint32_t preset, const Frame& f);

// Halton(2,3) sub-pixel offset in [-0.5, 0.5) for frame n, with the phase count DLSS recommends for the scale
void Jitter(uint64_t frame, uint32_t in_w, uint32_t out_w, float& x, float& y);

}  // namespace gpu::dlss
