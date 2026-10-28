#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "app/app.h"
#include "core/log.h"
#include <cassert>
#include <cmath>
#include <filesystem>
#include <fstream>

namespace fixture {
std::filesystem::path folder;
int auxiliary_saves = 0;
void SaveAuxiliary() { ++auxiliary_saves; }
}
namespace app {
const GameInfo kGame{.title = L"Settings component test", .settings = "settings.toml",
                    .save_aux = fixture::SaveAuxiliary};
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
    assert(s.world.traffic_multiplier == 1 && s.world.pedestrian_multiplier == 1);
    s.world.traffic_multiplier = 2.5f;
    s.world.pedestrian_multiplier = .25f;
    s.tuning.garage_extended = false;
    s.tuning.ride_front = 2.0f;
    s.tuning.ride_rear = -2.0f;
    s.tuning.wheel_limit = 2.5f;
    s.tuning.camber_front = -12; s.tuning.camber_rear = -8;
    s.tuning.offset_front = .08f; s.tuning.offset_rear = -.03f;
    s.tuning.spring_front = 1.5f; s.tuning.spring_rear = .75f;
    s.tuning.damper_front = 1.25f; s.tuning.damper_rear = .5f;
    s.tuning.brake_front = 2; s.tuning.brake_rear = .5f;
    s.tuning.grip_front = 1.25f; s.tuning.grip_rear = .75f;
    s.tuning.engine_power = 2; s.tuning.airbag_match_ride = false; s.tuning.wheel_tuck = false;
    s.graphics.fov_scale = 1.2f;
    s.graphics.lod = 2;
    s.graphics.scene_width = 15360;
    s.graphics.scene_height = 8640;
    s.render_percent = 100;
    s.width = 1280; s.height = 720;
    s.force_widescreen = true;
    s.deadzone_inner = .3f;
    s.deadzone_outer = .8f;
    app::Save(); // exercises the new bool beside float fields in the same [tuning] table
    assert(fixture::auxiliary_saves == 1);
    app::LogSettings();
    assert(fixture::auxiliary_saves == 1);  // Logging preferences must never rewrite a car-profile sidecar.
    assert(fs::is_regular_file(fixture::folder / "settings.toml"));
    s = app::Settings{};
    assert(s.tuning.garage_extended);
    app::Load();
    auto assert_close = [](float a, float b) { assert(std::abs(a - b) < 1e-5f); };
    assert_close(s.world.traffic_multiplier, 2.5f);
    assert_close(s.world.pedestrian_multiplier, .25f);
    assert(!s.tuning.garage_extended && s.force_widescreen);
    assert_close(s.tuning.ride_front, 2.0f); assert_close(s.tuning.ride_rear, -2.0f); assert_close(s.tuning.wheel_limit, 2.5f);
    assert_close(s.tuning.camber_front, -12); assert_close(s.tuning.camber_rear, -8);
    assert_close(s.tuning.offset_front, .08f); assert_close(s.tuning.offset_rear, -.03f);
    assert_close(s.tuning.spring_front, 1.5f); assert_close(s.tuning.spring_rear, .75f);
    assert_close(s.tuning.damper_front, 1.25f); assert_close(s.tuning.damper_rear, .5f);
    assert_close(s.tuning.brake_front, 2); assert_close(s.tuning.brake_rear, .5f);
    assert_close(s.tuning.grip_front, 1.25f); assert_close(s.tuning.grip_rear, .75f);
    assert_close(s.tuning.engine_power, 2); assert(!s.tuning.airbag_match_ride && !s.tuning.wheel_tuck);
    assert_close(s.graphics.fov_scale, 1.2f); assert_close(s.graphics.lod, 2);
    assert(s.graphics.scene_width == 15360 && s.graphics.scene_height == 8640);
    assert((app::RenderResolution() == app::Mode{15360, 8640}));
    s.render_percent = 400;
    assert((app::RenderResolution() == app::Mode{16384, 9216}));
    s.render_percent = 100;
    assert_close(s.deadzone_inner, .3f); assert_close(s.deadzone_outer, .8f);
    s.tuning.garage_extended = true;
    app::Save(); s = app::Settings{}; s.tuning.garage_extended = false;
    app::Load(); assert(s.tuning.garage_extended);
    // A laptop loading settings from a larger GPU still accepts 16K; allocation recovery is the renderer's job.
    // Invalid/partial pairs and oversized percentages must never form impossible D3D12 resources.
    {
        std::ofstream invalid(fixture::folder / "settings.toml");
        invalid << "render_percent = 99999\nwidth = 999999\nheight = 1080\n[graphics]\nscene_width = 0\nscene_height = 8640\n"
                   "[world]\ntraffic_multiplier = -10.0\npedestrian_multiplier = 99.0\n";
    }
    s = app::Settings{};
    app::Load();
    assert(s.render_percent == 400 && s.width == 16384 && s.height == 1080);
    assert(!s.graphics.scene_width && !s.graphics.scene_height);
    assert(s.world.traffic_multiplier == 0 && s.world.pedestrian_multiplier == 4);
    assert(app::RenderResolution().width <= 16384 && app::RenderResolution().height <= 16384);
    {
        std::ofstream invalid(fixture::folder / "settings.toml");
        invalid << "[world]\ntraffic_multiplier = nan\npedestrian_multiplier = inf\n";
    }
    s = app::Settings{};
    app::Load();
    assert(s.world.traffic_multiplier == 1 && s.world.pedestrian_multiplier == 1);
    // A scene's aspect drives the Xbox anamorphic projection independently of the physical monitor.
    s.graphics.scene_width = 640; s.graphics.scene_height = 480; s.force_widescreen = false;
    assert(!app::Widescreen());
    s.graphics.scene_width = 1920; s.graphics.scene_height = 1080;
    assert(app::Widescreen());
    // Delete only the files and empty directories this test created, all under its unique temporary folder.
    assert(fs::equivalent(fixture::folder.parent_path(), temp));
    assert(fs::remove(fixture::folder / "settings.toml"));
    assert(fs::remove(fixture::folder / "config" / "mc3_enhancements.toml"));
    assert(fs::remove(fixture::folder / "config"));
    assert(fs::remove(fixture::folder));
}
