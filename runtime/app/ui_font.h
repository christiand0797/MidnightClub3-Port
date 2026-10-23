#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <filesystem>
#include <imgui.h>

namespace app {

// The built-in 13 px font becomes blurred at fractional output scales. Rasterize the Windows UI typeface at
// a readable base size instead; it ships with Windows and adds no portable package dependency.
inline void ConfigureUiFont(ImGuiIO& io) {
    ImFontConfig config;
    config.OversampleH = 3;
    config.OversampleV = 2;
    wchar_t windows[MAX_PATH];
    const UINT count = GetWindowsDirectoryW(windows, MAX_PATH);
    if (count && count < MAX_PATH) {
        const auto font = std::filesystem::path(windows) / L"Fonts" / L"segoeui.ttf";
        std::error_code error;
        if (std::filesystem::is_regular_file(font, error)) {
            const auto utf8 = font.u8string();
            if (io.Fonts->AddFontFromFileTTF(reinterpret_cast<const char*>(utf8.c_str()), 22.0f, &config)) return;
        }
    }
    config.SizePixels = 20.0f;
    io.Fonts->AddFontDefault(&config);
}

}  // namespace app
