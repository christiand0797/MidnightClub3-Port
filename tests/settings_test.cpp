#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "app/app.h"
#include "core/log.h"
#include <cassert>
#include <cmath>
#include <filesystem>

namespace fixture { std::filesystem::path folder; }
namespace app {
const GameInfo kGame{.title = L"Settings component test", .settings = "settings.toml"};
std::filesystem::path ExeDir() { return fixture::folder; }
void* Window() { return nullptr; }
}
namespace logx { void Write(Level, const char*, ...) {} }

int main() {
    namespace fs = std::filesystem;
    const auto temp = fs::absolute(fs::temp_directory_path());
    fixture::folder = temp / ("mc3_settings_test_" + std::to_string(GetCurrentProcessId()) + "_" + std::to_string(GetTickCount64()));
    assert(fs::equivalent(fixture::folder.parent_path(), temp) && fs::create_directory(fixture::folder));
    auto& s = app::Get();
    s.tuning.garage_extended = false;
    s.tuning.ride_front = .15f;
    s.tuning.ride_rear = -.05f;
    s.tuning.wheel_limit = 2.5f;
    s.graphics.fov_scale = 1.2f;
    s.graphics.lod = 2;
    s.force_widescreen = true;
    s.deadzone_inner = .3f;
    s.deadzone_outer = .8f;
    app::Save(); // exercises the new bool beside float fields in the same [tuning] table
    assert(fs::is_regular_file(fixture::folder / "settings.toml"));
    s = app::Settings{};
    assert(s.tuning.garage_extended);
    app::Load();
    auto assert_close = [](float a, float b) { assert(std::abs(a - b) < 1e-5f); };
    assert(!s.tuning.garage_extended && s.force_widescreen);
    assert_close(s.tuning.ride_front, .15f); assert_close(s.tuning.ride_rear, -.05f); assert_close(s.tuning.wheel_limit, 2.5f);
    assert_close(s.graphics.fov_scale, 1.2f); assert_close(s.graphics.lod, 2);
    assert_close(s.deadzone_inner, .3f); assert_close(s.deadzone_outer, .8f);
    s.tuning.garage_extended = true;
    app::Save(); s = app::Settings{}; s.tuning.garage_extended = false;
    app::Load(); assert(s.tuning.garage_extended);
    // Delete only the files and empty directories this test created, all under its unique temporary folder.
    assert(fs::equivalent(fixture::folder.parent_path(), temp));
    assert(fs::remove(fixture::folder / "settings.toml"));
    assert(fs::remove(fixture::folder / "config" / "mc3_enhancements.toml"));
    assert(fs::remove(fixture::folder / "config"));
    assert(fs::remove(fixture::folder));
}
