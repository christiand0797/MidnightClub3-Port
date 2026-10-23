// Exercise the real WIC decoder and caller-owned replacement lifetime against an existing local texture pack.
// argv[1] is the fixture root containing textures/replace/*.png. No pack image or native setting is written.
#include "app/app.h"
#include "core/log.h"
#include "gpu/texture_pack.h"

#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <stdexcept>
#include <unordered_set>
#include <vector>

namespace {
std::filesystem::path fixture_root;
std::mutex log_lock;

void Require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}

void CheckMips(const gpu::pack::Image& image) {
    Require(image.width && image.height, "replacement has empty dimensions");
    uint32_t w = image.width, h = image.height;
    size_t level = 0;
    for (;;) {
        Require(level < image.levels.size(), "replacement mip chain ends too early");
        Require(image.levels[level].size() == size_t(w) * h * 4, "replacement mip has invalid BGRA byte count");
        if (w == 1 && h == 1) break;
        w = std::max(w / 2, 1u);
        h = std::max(h / 2, 1u);
        ++level;
    }
    Require(image.levels.size() == level + 1, "replacement mip chain continues past 1x1");
}
}  // namespace

namespace app {
Settings& Get() {
    static Settings settings;
    return settings;
}
std::filesystem::path ExeDir() { return fixture_root; }
}  // namespace app

namespace logx {
void Write(Level, const char* fmt, ...) {
    std::lock_guard<std::mutex> lock(log_lock);
    va_list args;
    va_start(args, fmt);
    std::vfprintf(stderr, fmt, args);
    va_end(args);
    std::fputc('\n', stderr);
}
}  // namespace logx

int main(int argc, char** argv) {
    try {
        Require(argc == 2, "expected fixture root containing textures/replace/*.png");
        fixture_root = std::filesystem::absolute(argv[1]);
        const auto replace = fixture_root / "textures" / "replace";
        Require(std::filesystem::is_directory(replace), "replacement fixture folder is missing");
        std::vector<std::filesystem::path> fixtures;
        std::unordered_set<uint64_t> hashes;
        for (const auto& entry : std::filesystem::recursive_directory_iterator(replace)) {
            if (!entry.is_regular_file()) continue;
            const auto name = entry.path().stem().string();
            if (name.size() != 16 || !std::all_of(name.begin(), name.end(), [](unsigned char c) { return std::isxdigit(c); }))
                continue;
            hashes.insert(std::stoull(name, nullptr, 16));
            if (entry.path().extension() == ".png") fixtures.push_back(entry.path());
        }
        Require(!fixtures.empty(), "no hash-named PNG fixture is available");
        std::sort(fixtures.begin(), fixtures.end());
        const uint64_t hash = std::stoull(fixtures.front().stem().string(), nullptr, 16);
        uint64_t unknown = 0;
        while (hashes.contains(unknown)) ++unknown;
        app::Get().graphics.texture_pack = true;
        Require(gpu::pack::ReplaceEnabled(), "valid texture pack was not indexed");
        const auto before = gpu::pack::GetStats();
        auto first = gpu::pack::Find(hash);
        Require(bool(first), "WIC could not decode the replacement fixture");
        CheckMips(*first);
        Require(gpu::pack::GetStats().used == before.used + 1, "first use was not counted");

        auto second = gpu::pack::Find(hash);
        Require(bool(second), "second WIC decode failed");
        Require(first.get() != second.get() && first->levels[0].data() != second->levels[0].data(),
                "replacement pixels are retained in shared cache ownership");
        Require(first->width == second->width && first->height == second->height && first->levels == second->levels,
                "independent replacement decodes disagree");
        Require(gpu::pack::GetStats().used == before.used + 1, "repeat decode inflated unique-use statistics");

        const uint32_t width = second->width, height = second->height;
        const auto last_pixel = second->levels.back();
        first.reset();
        CheckMips(*second);
        Require(second->width == width && second->height == height && second->levels.back() == last_pixel,
                "releasing one decoded replacement invalidated another owner");
        second.reset();
        Require(!gpu::pack::Find(unknown), "unknown texture hash unexpectedly loaded");
        Require(gpu::pack::GetStats().used == before.used + 1, "missing texture inflated use statistics");
        std::puts("Texture pack WIC decode and caller ownership passed");
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "Texture pack test failed: %s\n", e.what());
        return 1;
    }
}
