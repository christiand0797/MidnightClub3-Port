#include "core/host_path.h"
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

std::filesystem::path hostpath::ExecutablePath() {
    return ReadExecutablePath([](wchar_t* buffer, uint32_t capacity) {
        return GetModuleFileNameW(nullptr, buffer, capacity);
    });
}
#endif
