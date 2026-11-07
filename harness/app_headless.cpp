// the host side without a native Win32 window: the run folder as the exe folder
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <thread>

#include "app/app.h"

namespace app {

#ifndef XBE_WEB
Settings& Get() {
    static Settings s;
    return s;
}
void Load() {}
void Save() {}
Mode Desktop() { return {1280, 720}; }
Mode Output() { return Desktop(); }
Mode RenderResolution() { return Output(); }
Mode ActualRenderResolution() { return Output(); }
void ReportRenderResolution(Mode) {}
float RenderScale(uint32_t) { return 1.0f; }
bool Widescreen() { return true; }
#endif

void ApplyDisplay() {}
void ReportGraphicsFailure(const char*) {}

std::filesystem::path ExeDir() {
    // the browser build's file system may have no absolute working folder (Node on Windows): stay relative
    static const std::filesystem::path dir = [] {
        std::error_code ec;
        auto p = std::filesystem::current_path(ec);
        return ec ? std::filesystem::path(".") : p;
    }();
    return dir;
}
int Instance() {
    static const int n = [] {
        const char* e = std::getenv("INSTANCE");
        return e ? std::atoi(e) : 0;
    }();
    return n;
}
void* Window() { return nullptr; }

bool OpenSplash() { return false; }
void SplashFirstFrame() {}
void CloseSplash() {}
void OpenGameWindow(bool) {}
bool StartHandover() { return false; }
bool HandingOver() { return false; }
void FinishHandover() {}

std::recursive_mutex& UiLock() {
    static std::recursive_mutex m;
    return m;
}

void Pace() {}

}  // namespace app
