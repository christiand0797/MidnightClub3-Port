#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "app/app.h"
#include "core/log.h"
#include <cassert>
#include <cmath>
#include <cstdio>
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
    namespace pc=gpu::pbr::controls;
    assert(!s.pbr.enabled&&s.pbr.pack_path.empty()&&s.pbr.materials.empty());
    s.pbr.enabled=true;s.pbr.pack_path="materials/reviewed";s.pbr.approved_manifest_sha256=std::string(64,'A');
    s.pbr.quality=pc::Quality::Ultra;
    s.pbr.parameters.base_color={1.5f,0.5f,0.25f};s.pbr.parameters.emission_color={0.5f,2,3};
    s.pbr.parameters.roughness_factor=2;s.pbr.parameters.metallic_override=true;s.pbr.parameters.metallic=.75f;
    s.pbr.parameters.height_mode=pc::HeightMode::Displacement;s.pbr.parameters.displacement_height=.2f;
    s.pbr.parameters.displacement_midpoint=.25f;s.pbr.parameters.tessellation=16;
    gpu::pbr::Assignment assignment{0xFEDCBA9876543210ull,0xC123456789ABCDEFull,0xD23456789ABCDEF1ull,3,2,gpu::pbr::SurfaceClass::Vehicle,true};
    const auto key=pc::ExactKey(assignment,std::string(64,'a'));
    pc::MaterialOverride material{key,false,false,{}};
    material.parameters.roughness_override=true;material.parameters.roughness=.2f;
    material.parameters.normal_strength=2;material.parameters.ao_strength=.5f;material.parameters.emission_strength=4;
    material.parameters.height_mode=pc::HeightMode::Parallax;material.parameters.parallax_height=.03f;
    s.pbr.materials.push_back(material);
    assignment.vertex_shader^=1;const auto second_key=pc::ExactKey(assignment,std::string(64,'a'));
    s.pbr.materials.push_back({second_key,true,true,{}});
    assert(s.world.traffic_multiplier == 1 && s.world.pedestrian_multiplier == 1);
    s.world.traffic_multiplier = 2.5f;
    s.world.pedestrian_multiplier = .25f;
    assert(!s.capture.automatic && s.capture.interval == 30 && s.capture.limit == 20);
    s.capture.automatic = true; s.capture.interval = 45; s.capture.limit = 7;
    assert(!s.graphics.car_paint && s.graphics.car_relight == 1 && s.graphics.car_specular == 1);
    s.graphics.car_paint = true; s.graphics.car_specular = 2.5f; s.graphics.car_roughness = .15f;
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
    assert(s.pbr.enabled&&s.pbr.pack_path=="materials/reviewed"&&s.pbr.approved_manifest_sha256==std::string(64,'a'));
    assert(s.pbr.quality==pc::Quality::Ultra&&s.pbr.materials.size()==2);
    assert_close(s.pbr.parameters.base_color[0],1.5f);assert_close(s.pbr.parameters.emission_color[2],3);
    assert(s.pbr.parameters.height_mode==pc::HeightMode::Displacement&&s.pbr.parameters.tessellation==16);
    assert_close(s.pbr.parameters.displacement_height,.2f);assert_close(s.pbr.parameters.displacement_midpoint,.25f);
    const auto* restored=pc::FindOverride(s.pbr,key);assert(restored&&!restored->enabled&&!restored->use_global_parameters);
    assert(restored->parameters==material.parameters);
    assert(pc::FindOverride(s.pbr,second_key)&&pc::FindOverride(s.pbr,second_key)->use_global_parameters);
    assert(restored->key.assignment.original_texture==0xFEDCBA9876543210ull); // No signed-TOML truncation.
    assert_close(s.world.traffic_multiplier, 2.5f);
    assert_close(s.world.pedestrian_multiplier, .25f);
    assert(s.capture.automatic && s.capture.limit == 7); assert_close(s.capture.interval, 45);
    assert(s.graphics.car_paint); assert_close(s.graphics.car_specular, 2.5f); assert_close(s.graphics.car_roughness, .15f);
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
                   "[world]\ntraffic_multiplier = -10.0\npedestrian_multiplier = 99.0\n"
                   "[capture]\ninterval_seconds = 0.5\nmax_per_session = 100000\n";
    }
    s = app::Settings{};
    app::Load();
    assert(s.capture.interval == 5 && s.capture.limit == 500 && !s.capture.automatic);
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
    assert(!s.pbr.enabled&&s.pbr.materials.empty()); // Old preferences remain default-off.
    {
        std::ofstream invalid(fixture::folder / "settings.toml");
        invalid<<"[pbr]\nenabled = true\nquality = 'unknown'\npack_path = ''\napproved_manifest_sha256 = 'bad'\n"
                  "[pbr.parameters]\nbase_color = [-2.0, nan, 99.0]\nemission_color = [inf, 0.5, -2.0]\n"
                  "roughness_factor = nan\nmetallic_factor = 99.0\nroughness = inf\nnormal_strength = -1.0\n"
                  "ao_strength = 99.0\nemission_strength = inf\nparallax_height = 3.0\ndisplacement_height = 3.0\n"
                  "displacement_midpoint = -2.0\ntessellation = -1\nheight_mode = 'invented'\n"
                  "[[pbr.materials]]\nmanifest_sha256 = 'bad'\noriginal_texture = 'FEDCBA9876543210'\n"
                  "vertex_shader = 'C123456789ABCDEF'\npixel_shader = 'D23456789ABCDEF1'\nstage = 0\nuv_set = 0\nsurface = 'world'\n";
    }
    s=app::Settings{};app::Load();
    assert(s.pbr.pack_path.empty()&&s.pbr.approved_manifest_sha256.empty()&&s.pbr.materials.empty());
    assert(s.pbr.quality==pc::Quality::High);
    assert((s.pbr.parameters.base_color==std::array<float,3>{0,1,4}));
    assert((s.pbr.parameters.emission_color==std::array<float,3>{1,.5f,0}));
    assert(s.pbr.parameters.roughness_factor==1&&s.pbr.parameters.metallic_factor==4);
    assert(s.pbr.parameters.roughness==.5f&&s.pbr.parameters.normal_strength==0&&s.pbr.parameters.ao_strength==2);
    assert(s.pbr.parameters.emission_strength==1&&s.pbr.parameters.parallax_height==.1f&&s.pbr.parameters.displacement_height==.25f);
    assert(s.pbr.parameters.displacement_midpoint==0&&s.pbr.parameters.tessellation==1&&s.pbr.parameters.height_mode==pc::HeightMode::None);
    {
        std::ofstream bounded(fixture::folder / "settings.toml");
        bounded<<"[pbr]\napproved_manifest_sha256 = '"<<std::string(64,'a')<<"'\n";
        for(unsigned i=0;i<513;++i)
            bounded<<"[[pbr.materials]]\nmanifest_sha256 = '"<<std::string(64,'a')<<"'\noriginal_texture = 'FEDCBA9876543210'\n"
                "vertex_shader = 'C123456789ABCDEF'\npixel_shader = 'D23456789ABCDEF1'\nstage = 0\nuv_set = 0\nsurface = 'world'\n"
                "enabled = "<<(i?"true":"false")<<"\n";
    }
    s=app::Settings{};app::Load();
    assert(!s.pbr.enabled&&s.pbr.materials.size()==1&&!s.pbr.materials[0].enabled); // First exact identity wins.
    // Exercise the actual file parser/save path at the shared metadata budget, beyond the decoded-map limit.
    {
        std::ofstream bounded(fixture::folder / "settings.toml");
        bounded<<"[pbr]\napproved_manifest_sha256 = '"<<std::string(64,'a')<<"'\n";
        for(size_t i=0;i<=pc::kMaxMaterials;++i) {
            char texture[17];std::snprintf(texture,sizeof(texture),"%016llx",static_cast<unsigned long long>(i+1));
            bounded<<"[[pbr.materials]]\nmanifest_sha256 = '"<<std::string(64,'a')<<"'\noriginal_texture = '"<<texture<<"'\n"
                "vertex_shader = 'C123456789ABCDEF'\npixel_shader = 'D23456789ABCDEF1'\nstage = 0\nuv_set = 0\nsurface = 'world'\n";
        }
    }
    s=app::Settings{};app::Load();
    assert(s.pbr.materials.size()==pc::kMaxMaterials&&s.pbr.materials.front().key.assignment.original_texture==1);
    assert(s.pbr.materials.back().key.assignment.original_texture==pc::kMaxMaterials);
    app::Save();s=app::Settings{};app::Load();
    assert(!s.pbr.enabled&&s.pbr.materials.size()==pc::kMaxMaterials);
    assert(s.capture.automatic==false&&!s.graphics.car_paint); // Independent features retain their default values.
    {
        std::ofstream missing(fixture::folder / "settings.toml");missing<<"pbr = 'invalid optional table'\n";
    }
    s.pbr.enabled=true;app::Load();assert(!s.pbr.enabled&&s.pbr.materials.empty());
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
