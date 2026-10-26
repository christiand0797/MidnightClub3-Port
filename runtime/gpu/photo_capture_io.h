#pragma once
#include "gpu/capture_directory.h"
#include <array>
#include <cstring>
#include <optional>
#include <vector>

namespace gpu::photo_capture {

// Four attempts at most: no more than256 MiB of BMP payload per process/episode.
constexpr uint64_t kMaxFrameBytes = 64ull << 20;

inline std::optional<uint32_t> PixelBytes(uint32_t w, uint32_t h) {
    if (!w || !h || w > 0x7FFFFFFFu || h > 0x7FFFFFFFu) return std::nullopt;
    const uint64_t bytes = uint64_t(w) * h * 4;
    if (bytes > kMaxFrameBytes) return std::nullopt;
    return uint32_t(bytes);
}

struct TextureIdentity { uint64_t original_hash = 0; };

// Upload fallback can reset the entire texture cache entry. Commit the ORIGINAL identity after that reset.
// A failed allocation/upload must not leave an identity claiming a usable resource.
template<class Upload>
bool UploadWithIdentity(TextureIdentity& cache, uint64_t original, Upload upload) {
    cache.original_hash = 0;
    if (!upload()) return false;  // Failure may have erased the cache entry: never touch that reference again.
    cache.original_hash = original;
    return true;
}

// Testable ordering shared by the actual backbuffer hook. No resize/shader pass is involved.
template<class ToCopy, class Read, class Restore>
bool ReadPresented(uint32_t w, uint32_t h, std::vector<uint8_t>& pixels, ToCopy to_copy, Read read, Restore restore) {
    pixels.clear();
    const auto bytes = PixelBytes(w, h);
    if (!bytes) return false;
    to_copy();
    bool ok;
    try { ok = read(pixels); }
    catch (...) { restore(); throw; }
    restore();
    return ok && pixels.size() == *bytes;
}

inline bool SaveBmpNew(const std::filesystem::path& path, uint32_t w, uint32_t h,
                       const std::vector<uint8_t>& pixels) {
    const auto bytes = PixelBytes(w, h);
    if (!bytes || pixels.size() != *bytes) return false;
    FILE* file = OpenCaptureFileNew(path);
    if (!file) return false;
    std::array<uint8_t, 54> header{'B', 'M'};
    auto put = [&](size_t offset, uint32_t value) { std::memcpy(header.data() + offset, &value, 4); };
    put(2, 54 + *bytes); put(10, 54); put(14, 40); put(18, w); put(22, uint32_t(-int32_t(h)));
    header[26] = 1; header[28] = 32; put(34, *bytes);
    const bool written = std::fwrite(header.data(), 1, header.size(), file) == header.size()
                         && std::fwrite(pixels.data(), 1, pixels.size(), file) == pixels.size();
    const bool closed = std::fclose(file) == 0;
    return written && closed;
}

struct Observation {
    uint64_t original_hash = 0;
    uint32_t stage = 0, guest_data = 0, target_data = 0;
    uint32_t guest_w = 0, guest_h = 0, host_w = 0, host_h = 0;
    bool replaced = false;
};

}  // namespace gpu::photo_capture
