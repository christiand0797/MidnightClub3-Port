#pragma once
// Capture evidence owns a NEW directory and new files. Repeated wall-clock times never permit reuse.
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>
#include <utility>

namespace gpu {

struct CaptureStamp {
    uint64_t frame = 0, presents = 0, wall_unix_ns = 0, monotonic_ns = 0;
    uint32_t process_id = 0;
};

struct CaptureDirectory {
    std::filesystem::path path;
    std::error_code error;
    uint32_t collision_attempt = 0;
};

inline std::string CaptureDirectoryName(const CaptureStamp& stamp, uint32_t attempt) {
    return "f" + std::to_string(stamp.frame) + "-p" + std::to_string(stamp.presents) +
           "-w" + std::to_string(stamp.wall_unix_ns) + "-m" + std::to_string(stamp.monotonic_ns) +
           "-pid" + std::to_string(stamp.process_id) + "-" + std::to_string(attempt);
}

// where a requested frame is saved: XBE_CAPTURE_DIR when set, else the executable's captures folder (a relative
// path is taken from the executable's directory)
inline std::filesystem::path CaptureBase(const std::filesystem::path& configured) {
    return configured.empty() ? std::filesystem::path("captures") : configured;
}

inline CaptureDirectory CreateUniqueCaptureDirectory(const std::filesystem::path& base, const CaptureStamp& stamp,
                                                       uint32_t max_attempts = 64) {
    CaptureDirectory result;
    if (!max_attempts) {
        result.error = std::make_error_code(std::errc::invalid_argument);
        return result;
    }
    std::filesystem::create_directories(base, result.error);
    if (result.error) return result;
    for (uint32_t attempt = 0; attempt < max_attempts; ++attempt) {
        auto candidate = base / CaptureDirectoryName(stamp, attempt);
        std::error_code error;
        if (std::filesystem::create_directory(candidate, error)) {
            result.path = std::move(candidate);
            result.collision_attempt = attempt;
            return result;
        }
        // Both an existing directory and an existing file are someone else's evidence.
        if (error && error != std::errc::file_exists) {
            result.error = error;
            return result;
        }
    }
    result.error = std::make_error_code(std::errc::file_exists);
    return result;
}

inline FILE* OpenCaptureFileNew(const std::filesystem::path& path) {
#ifdef _WIN32
    FILE* file = nullptr;
    return _wfopen_s(&file, path.c_str(), L"wbx") == 0 ? file : nullptr;
#else
    return std::fopen(path.c_str(), "wbx");
#endif
}

inline bool WriteCaptureMetadata(const CaptureDirectory& directory, const CaptureStamp& stamp,
                                 std::error_code& error) {
    error.clear();
    if (directory.error || directory.path.empty()) {
        error = directory.error ? directory.error : std::make_error_code(std::errc::invalid_argument);
        return false;
    }
    FILE* file = OpenCaptureFileNew(directory.path / "capture.json");
    if (!file) {
        error = std::error_code(errno ? errno : EIO, std::generic_category());
        return false;
    }
    int written = std::fprintf(file,
        "{\n  \"format\": \"mc3-frame-capture-v1\",\n"
        "  \"armed_frame\": %llu,\n  \"armed_completed_presents\": %llu,\n"
        "  \"wall_time_unix_ns\": %llu,\n  \"monotonic_ns\": %llu,\n"
        "  \"process_id\": %u,\n  \"collision_attempt\": %u,\n"
        "  \"scope\": \"following draw sequence until next Present\"\n}\n",
        static_cast<unsigned long long>(stamp.frame), static_cast<unsigned long long>(stamp.presents),
        static_cast<unsigned long long>(stamp.wall_unix_ns), static_cast<unsigned long long>(stamp.monotonic_ns),
        stamp.process_id, directory.collision_attempt);
    int closed = std::fclose(file);
    if (written < 0 || closed != 0) {
        error = std::error_code(errno ? errno : EIO, std::generic_category());
        return false;
    }
    return true;
}

}  // namespace gpu
