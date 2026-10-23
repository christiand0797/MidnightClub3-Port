#pragma once
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <vector>
#include <algorithm>

// diagnostics switched on by environment variables, read once
namespace diag {

// XBE_PERF set: perf log lines once a second and the read-backs named
inline bool Perf() {
    static const bool on = std::getenv("XBE_PERF") != nullptr;
    return on;
}

// XBE_CAPTURE_DIR: where F3 saves a frame, empty: F3 does not capture
inline const std::filesystem::path& CaptureDir() {
    static const std::filesystem::path dir = [] {
#ifdef _WIN32
        const wchar_t* v = _wgetenv(L"XBE_CAPTURE_DIR");
#else
        const char* v = std::getenv("XBE_CAPTURE_DIR");
#endif
        return v ? std::filesystem::path(v) : std::filesystem::path();
    }();
    return dir;
}

// Optional unattended capture: start recording when the requested present is reached.
// The following complete frame is saved using the same path as F3.
inline bool CaptureFrame(uint64_t current) {
    static const std::vector<uint64_t> frames = [] {
        std::vector<uint64_t> result;
        const char* value = std::getenv("XBE_CAPTURE_FRAME");
        while (value && *value) {
            char* end = nullptr;
            auto parsed = std::strtoull(value, &end, 10);
            if (end == value || (*end && *end != ',')) break;
            if (parsed) result.push_back(parsed);
            value = *end ? end + 1 : end;
        }
        return result;
    }();
    return std::find(frames.begin(), frames.end(), current) != frames.end();
}

// XBE_CAPTURE_SNAP: draw numbers of a captured frame ("819-823,900") after which the bound target is saved too,
// to see what each step of a multi-pass effect leaves behind
inline bool CaptureSnap(uint32_t draw) {
    static const std::vector<std::pair<uint32_t, uint32_t>> ranges = [] {
        std::vector<std::pair<uint32_t, uint32_t>> result;
        const char* value = std::getenv("XBE_CAPTURE_SNAP");
        while (value && *value) {
            char* end = nullptr;
            uint32_t lo = uint32_t(std::strtoul(value, &end, 10)), hi = lo;
            if (end == value) break;
            if (*end == '-') {
                const char* next = end + 1;
                hi = uint32_t(std::strtoul(next, &end, 10));
                if (end == next) break;
            }
            result.emplace_back(lo, hi);
            if (*end && *end != ',') break;
            value = *end ? end + 1 : end;
        }
        return result;
    }();
    for (const auto& [lo, hi] : ranges)
        if (draw >= lo && draw <= hi) return true;
    return false;
}

// XBE_CAPTURE_SNAP_TEX: a guest texture address (hex); in a captured frame, every draw that samples it is saved
// too, for effects whose draw number moves from frame to frame
inline uint32_t CaptureSnapTexture() {
    static const uint32_t va = [] {
        const char* v = std::getenv("XBE_CAPTURE_SNAP_TEX");
        return v ? uint32_t(std::strtoul(v, nullptr, 16)) : 0u;
    }();
    return va;
}

}  // namespace diag
