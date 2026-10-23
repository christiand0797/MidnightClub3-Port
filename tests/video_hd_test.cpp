// Exercise the real Media Foundation movie decoder with a completed local HD movie, without starting the game.
// argv[1] is the fixture root containing video_hd/rockstar.mp4. App settings, the texture header, and logging are
// supplied here; runtime/gpu/video_hd.cpp is linked unchanged by the test target.
#include "app/app.h"
#include "core/log.h"
#include "gpu/texture.h"
#include "gpu/video_hd.h"

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace {
constexpr uint32_t kHeader = 0x00100000;
constexpr uint32_t kOtherHeader = 0x00110000;
constexpr uint32_t kData = 0x81000000;
std::filesystem::path fixture_root;
std::mutex log_lock;

void Require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}

struct CloseMovie {
    ~CloseMovie() {
        gpu::video::Close(kHeader);
        gpu::video::Close(kOtherHeader);
    }
};

struct Picture {
    uint32_t width = 0, height = 0;
    bool opaque = false;
    bool colored = false;
    uint32_t calls = 0;

    void Read(const uint8_t* bgra, uint32_t w, uint32_t h) {
        width = w;
        height = h;
        opaque = true;
        colored = false;
        ++calls;
        Require(bgra && w && h, "decoder returned an empty picture");
        for (size_t i = 0; i < size_t(w) * h * 4; i += 4) {
            opaque = opaque && bgra[i + 3] == 255;
            colored = colored || bgra[i] || bgra[i + 1] || bgra[i + 2];
        }
    }
};

void WaitPicture(uint64_t& serial, Picture& picture, uint64_t after = 0) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
        if (gpu::video::Playing(kData)) {
            gpu::video::Picture(kData, serial,
                                [&](const uint8_t* px, uint32_t w, uint32_t h) { picture.Read(px, w, h); });
            if (serial > after && picture.calls) return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    throw std::runtime_error("timed out waiting for a decoded HD movie picture");
}
}  // namespace

namespace app {
Settings& Get() {
    static Settings settings;
    return settings;
}
std::filesystem::path ExeDir() { return fixture_root; }
}  // namespace app

namespace gpu {
bool Describe(uint32_t header, TextureDesc& d) {
    if (header != kHeader && header != kOtherHeader) return false;
    d = {};
    d.data = kData;
    d.format = 0x1E;  // the player's LIN_X8R8G8B8 movie texture
    d.width = 640;
    d.height = 480;
    d.pitch = 640 * 4;
    d.linear = true;
    return true;
}
}  // namespace gpu

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
        Require(argc == 2, "expected fixture root containing video_hd/rockstar.mp4");
        fixture_root = std::filesystem::absolute(argv[1]);
        Require(std::filesystem::is_regular_file(fixture_root / "video_hd" / "rockstar.mp4"),
                "completed rockstar.mp4 fixture is missing");
        CloseMovie cleanup;
        auto& enabled = app::Get().graphics.hd_movies;
        enabled = false;
        Require(!gpu::video::Enabled(), "HD movie setting did not disable");
        Require(!gpu::video::Open(kHeader, "d:\\video\\RockStar.bik"),
                "disabled movie open must report original playback");
        gpu::video::Frame(kHeader, 1);
        Require(!gpu::video::Playing(kData), "disabled movie should not replace the original");
        uint64_t serial = 0;
        Picture picture;
        auto read = [&](const uint8_t* px, uint32_t w, uint32_t h) { picture.Read(px, w, h); };
        Require(!gpu::video::Picture(kData, serial, read) && !picture.calls,
                "disabled movie unexpectedly supplied a picture");

        enabled = true;
        gpu::video::Frame(kHeader, 61);  // first enable is midclip; the decoder must catch up from its start
        WaitPicture(serial, picture);
        Require(picture.width == 1920 && picture.height == 1440, "unexpected fixture decode dimensions");
        Require(picture.opaque, "RGB32 alpha was not made opaque");
        Require(picture.colored, "decoded midclip picture contains no RGB image");

        const uint64_t previous = serial;
        gpu::video::Frame(kHeader, 91);
        WaitPicture(serial, picture, previous);
        Require(picture.opaque, "subsequent decoded frame has invalid alpha");

        enabled = false;
        const uint32_t calls = picture.calls;
        const uint64_t hidden_serial = serial;
        Require(!gpu::video::Playing(kData), "HD movie remained selected after disabling");
        Require(!gpu::video::Picture(kData, serial, read), "disabled movie exposed its HD picture");
        Require(picture.calls == calls && serial == hidden_serial,
                "disabled movie changed the renderer's picture/cache serial");

        enabled = true;
        gpu::video::Frame(kHeader, 91);
        Require(gpu::video::Playing(kData), "existing HD movie did not resume after re-enabling");
        uint64_t fresh_serial = 0;
        Require(gpu::video::Picture(kData, fresh_serial, read) && fresh_serial,
                "re-enabled movie did not provide its current frame");
        Require(picture.calls == calls + 1 && picture.opaque,
                "re-enabled movie did not reuse a valid decoded picture");

        const uint64_t before_loop = fresh_serial;
        gpu::video::Frame(kHeader, 31);
        WaitPicture(fresh_serial, picture, before_loop);
        Require(picture.opaque, "restarted movie has invalid pixels");

        // A different guest texture header may reuse the same data allocation. Closing the retired header
        // must not erase the replacement movie's registration, and its first frame must have a fresh cache token.
        const uint64_t before_reuse = fresh_serial;
        Require(gpu::video::Open(kOtherHeader, "d:\\video\\RockStar.bik"), "reused movie data did not open");
        gpu::video::Frame(kOtherHeader, 61);
        gpu::video::Close(kHeader);
        WaitPicture(fresh_serial, picture, before_reuse);
        Require(picture.opaque, "reused movie data has invalid pixels");
        gpu::video::Close(kHeader);  // a repeated stale close is also harmless
        Require(gpu::video::Playing(kData), "stale header close stopped the new movie");

        gpu::video::Close(kOtherHeader);
        gpu::video::Close(kHeader);
        Require(!gpu::video::Playing(kData), "closed movie stayed registered");
        Require(!gpu::video::Picture(kData, fresh_serial, read), "closed movie exposed a stale picture");
        Require(!gpu::video::Open(kHeader, "d:\\video\\__mc3_missing_fixture_7CF4C169__.bik"),
                "missing replacement should keep the original movie");
        Require(!gpu::video::Playing(kData), "missing replacement registered a stale player");
        std::puts("HD movie decode and live selection passed");
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "HD movie test failed: %s\n", e.what());
        return 1;
    }
}
