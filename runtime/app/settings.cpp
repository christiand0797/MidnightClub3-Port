// the settings file beside the exe
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <charconv>
#include <cstdio>
#include <fstream>
#include <sstream>

#include <toml++/toml.hpp>

#include "app/app.h"
#include "core/log.h"
#include "core/host_path.h"

namespace app {

namespace {

const char* kModes[] = {"fullscreen", "borderless", "windowed"};

std::filesystem::path File() { return ExeDir() / kGame.settings; }

// [graphics]: every field by its key, so load and save stay one list
struct GraphicsField {
    const char* key;
    float* f;
    bool* b;
    uint32_t* u;
    float lo, hi;
};
std::vector<GraphicsField> TuningFields(Settings::Tuning& v) {
    return {
        {"garage_extended", nullptr, &v.garage_extended, nullptr, 0, 1},
        {"ride_height_front", &v.ride_front, nullptr, nullptr, -2.0f, 2.0f},
        {"ride_height_rear", &v.ride_rear, nullptr, nullptr, -2.0f, 2.0f},
        {"wheel_size_limit", &v.wheel_limit, nullptr, nullptr, 1, 3},
        {"camber_front", &v.camber_front, nullptr, nullptr, -30, 20},
        {"camber_rear", &v.camber_rear, nullptr, nullptr, -30, 20},
        {"offset_front", &v.offset_front, nullptr, nullptr, -0.3f, 0.3f},
        {"offset_rear", &v.offset_rear, nullptr, nullptr, -0.3f, 0.3f},
        {"spring_front", &v.spring_front, nullptr, nullptr, 0.25f, 3},
        {"spring_rear", &v.spring_rear, nullptr, nullptr, 0.25f, 3},
        {"damper_front", &v.damper_front, nullptr, nullptr, 0.25f, 3},
        {"damper_rear", &v.damper_rear, nullptr, nullptr, 0.25f, 3},
        {"brake_front", &v.brake_front, nullptr, nullptr, 0.25f, 3},
        {"brake_rear", &v.brake_rear, nullptr, nullptr, 0.25f, 3},
        {"grip_front", &v.grip_front, nullptr, nullptr, 0.5f, 2},
        {"grip_rear", &v.grip_rear, nullptr, nullptr, 0.5f, 2},
        {"engine_power", &v.engine_power, nullptr, nullptr, 0.5f, 3},
        {"airbag_match_ride", nullptr, &v.airbag_match_ride, nullptr, 0, 1},
        {"wheel_tuck", nullptr, &v.wheel_tuck, nullptr, 0, 1},
    };
}

std::vector<GraphicsField> GraphicsFields(Settings::Graphics& g) {
    return {
        {"scene_width", nullptr, nullptr, &g.scene_width, 0, resolution::kMaxDimension},
        {"scene_height", nullptr, nullptr, &g.scene_height, 0, resolution::kMaxDimension},
        {"post", nullptr, &g.post, nullptr, 0, 1},
        {"anti_aliasing", nullptr, nullptr, &g.aa, 0, 1},
        {"anti_aliasing_strength", &g.aa_strength, nullptr, nullptr, 0, 1},
        {"sharpen", &g.sharpen, nullptr, nullptr, 0, 1},
        {"brightness", &g.brightness, nullptr, nullptr, -0.5f, 0.5f},
        {"contrast", &g.contrast, nullptr, nullptr, 0.5f, 1.5f},
        {"saturation", &g.saturation, nullptr, nullptr, 0, 2},
        {"vibrance", &g.vibrance, nullptr, nullptr, -1, 1},
        {"gamma", &g.gamma, nullptr, nullptr, 0.5f, 2},
        {"sixty_fps", nullptr, &g.sixty, nullptr, 0, 1},
        {"motion_blur", &g.motion_blur, nullptr, nullptr, 0, 2},
        {"glow", &g.glow, nullptr, nullptr, 0, 2},
        {"headlights", &g.headlights, nullptr, nullptr, 0, 2},
        {"neon", &g.neon, nullptr, nullptr, 0, 4},
        {"show_fps", nullptr, &g.show_fps, nullptr, 0, 1},
        {"texture_pack", nullptr, &g.texture_pack, nullptr, 0, 1},
        {"texture_dump", nullptr, &g.texture_dump, nullptr, 0, 1},
        {"texture_detail", nullptr, nullptr, &g.texture_detail, 0, 2},
        {"hd_movies", nullptr, &g.hd_movies, nullptr, 0, 1},
        {"lod", &g.lod, nullptr, nullptr, 0.5f, 4},
        {"fov_scale", &g.fov_scale, nullptr, nullptr, 0.75f, 1.5f},
        {"car_paint", nullptr, &g.car_paint, nullptr, 0, 1},
        {"car_relight", &g.car_relight, nullptr, nullptr, 0, 1},
        {"car_specular", &g.car_specular, nullptr, nullptr, 0, 3},
        {"car_roughness", &g.car_roughness, nullptr, nullptr, 0.05f, 0.8f},
        {"car_reflection", &g.car_reflection, nullptr, nullptr, 0, 2},
    };
}

std::vector<GraphicsField> WorldFields(Settings::World& w) {
    return {
        {"traffic_multiplier", &w.traffic_multiplier, nullptr, nullptr, 0, 4},
        {"pedestrian_multiplier", &w.pedestrian_multiplier, nullptr, nullptr, 0, 4},
    };
}

std::vector<GraphicsField> CaptureFields(Settings::Capture& c) {
    return {
        {"automatic", nullptr, &c.automatic, nullptr, 0, 1},
        {"interval_seconds", &c.interval, nullptr, nullptr, 5, 600},
        {"max_per_session", nullptr, nullptr, &c.limit, 1, 500},
    };
}

namespace pc = gpu::pbr::controls;
std::vector<GraphicsField> PbrFields(pc::Parameters& p) {
    return {{"roughness_factor",&p.roughness_factor,nullptr,nullptr,0,4},
            {"metallic_factor",&p.metallic_factor,nullptr,nullptr,0,4},
            {"roughness_override",nullptr,&p.roughness_override,nullptr,0,1},
            {"metallic_override",nullptr,&p.metallic_override,nullptr,0,1},
            {"roughness",&p.roughness,nullptr,nullptr,0,1},
            {"metallic",&p.metallic,nullptr,nullptr,0,1},
            {"normal_strength",&p.normal_strength,nullptr,nullptr,0,4},
            {"ao_strength",&p.ao_strength,nullptr,nullptr,0,2},
            {"emission_strength",&p.emission_strength,nullptr,nullptr,0,16},
            {"parallax_height",&p.parallax_height,nullptr,nullptr,0,0.1f},
            {"displacement_height",&p.displacement_height,nullptr,nullptr,0,0.25f},
            {"displacement_midpoint",&p.displacement_midpoint,nullptr,nullptr,0,1},
            {"tessellation",nullptr,nullptr,&p.tessellation,1,16}};
}
const char* QualityName(pc::Quality q) {
    constexpr const char* names[]={"low","medium","high","ultra"};
    return names[std::min(uint32_t(q),3u)];
}
const char* HeightName(pc::HeightMode mode) {
    constexpr const char* names[]={"none","parallax","displacement"};
    return names[std::min(uint32_t(mode),2u)];
}
void ReadPbrParameters(const toml::table& table,pc::Parameters& p) {
    for(const auto& f:PbrFields(p)) {
        if(f.f)*f.f=table[f.key].value_or(*f.f);
        if(f.b)*f.b=table[f.key].value_or(*f.b);
        if(f.u)if(auto v=table[f.key].value<int64_t>())*f.u=uint32_t(std::clamp(*v,int64_t(f.lo),int64_t(f.hi)));
    }
    for(auto [key,color]:{std::pair{"base_color",&p.base_color},std::pair{"emission_color",&p.emission_color}})
        if(const auto* values=table[key].as_array();values&&values->size()==3)
            for(size_t i=0;i<3;++i)(*color)[i]=(*values)[i].value_or((*color)[i]);
    const auto mode=table["height_mode"].value_or(std::string("none"));
    p.height_mode=mode=="parallax"?pc::HeightMode::Parallax:mode=="displacement"?pc::HeightMode::Displacement:pc::HeightMode::None;
    pc::Sanitize(p);
}
toml::table PbrParametersTable(pc::Parameters p) {
    pc::Sanitize(p);toml::table result;
    for(const auto& f:PbrFields(p)) {
        if(f.f)result.insert(f.key,double(*f.f));
        if(f.b)result.insert(f.key,*f.b);
        if(f.u)result.insert(f.key,int64_t(*f.u));
    }
    result.insert("base_color",toml::array{double(p.base_color[0]),double(p.base_color[1]),double(p.base_color[2])});
    result.insert("emission_color",toml::array{double(p.emission_color[0]),double(p.emission_color[1]),double(p.emission_color[2])});
    result.insert("height_mode",HeightName(p.height_mode));return result;
}
uint64_t PbrHex(const toml::table& t,const char* key) {
    const auto text=t[key].value_or(std::string{});uint64_t value=0;
    if(text.size()!=16)return 0;
    const auto parsed=std::from_chars(text.data(),text.data()+text.size(),value,16);
    return parsed.ec==std::errc{}&&parsed.ptr==text.data()+text.size()?value:0;
}
std::string PbrHex(uint64_t value) {
    char text[17];std::snprintf(text,sizeof(text),"%016llx",static_cast<unsigned long long>(value));return text;
}
void LoadPbr(const toml::table& root,pc::Settings& settings) {
    settings={}; // Missing/invalid optional table must never retain a preceding enabled session.
    const auto* t=root["pbr"].as_table();if(!t)return;
    settings.enabled=(*t)["enabled"].value_or(false);
    settings.pack_path=(*t)["pack_path"].value_or(std::string{});
    settings.approved_manifest_sha256=(*t)["approved_manifest_sha256"].value_or(std::string{});
    const auto quality=(*t)["quality"].value_or(std::string("high"));
    settings.quality=quality=="low"?pc::Quality::Low:quality=="medium"?pc::Quality::Medium:
        quality=="ultra"?pc::Quality::Ultra:pc::Quality::High;
    if(const auto* p=(*t)["parameters"].as_table())ReadPbrParameters(*p,settings.parameters);
    if(const auto* rows=(*t)["materials"].as_array()) {
        const size_t limit=std::min(rows->size(),pc::kMaxMaterials);
        for(size_t i=0;i<limit;++i) {
            const auto* row=(*rows)[i].as_table();if(!row)continue;
            pc::MaterialOverride entry;
            entry.key.approved_manifest_sha256=(*row)["manifest_sha256"].value_or(std::string{});
            auto& a=entry.key.assignment;
            a.original_texture=PbrHex(*row,"original_texture");a.vertex_shader=PbrHex(*row,"vertex_shader");a.pixel_shader=PbrHex(*row,"pixel_shader");
            const auto stage=(*row)["stage"].value_or(int64_t(-1)),uv=(*row)["uv_set"].value_or(int64_t(-1));
            if(stage<0||stage>=4||uv<0||uv>=4)continue;
            a.stage=uint32_t(stage);a.uv_set=uint32_t(uv);
            const auto surface=(*row)["surface"].value_or(std::string{});
            if(surface!="world"&&surface!="vehicle")continue;
            a.surface=surface=="world"?gpu::pbr::SurfaceClass::World:gpu::pbr::SurfaceClass::Vehicle;
            a.cutout=(*row)["cutout"].value_or(false);
            entry.enabled=(*row)["enabled"].value_or(true);
            entry.use_global_parameters=(*row)["use_global_parameters"].value_or(true);
            if(const auto* p=(*row)["parameters"].as_table())ReadPbrParameters(*p,entry.parameters);
            settings.materials.push_back(std::move(entry));
        }
    }
    pc::Sanitize(settings);
}
toml::table PbrTable(pc::Settings settings) {
    pc::Sanitize(settings);
    toml::table result{{"enabled",settings.enabled},{"pack_path",settings.pack_path},
        {"approved_manifest_sha256",settings.approved_manifest_sha256},{"quality",QualityName(settings.quality)},
        {"parameters",PbrParametersTable(settings.parameters)}};
    toml::array rows;
    for(const auto& entry:settings.materials) {
        const auto& a=entry.key.assignment;
        rows.push_back(toml::table{{"manifest_sha256",entry.key.approved_manifest_sha256},
            {"original_texture",PbrHex(a.original_texture)},{"vertex_shader",PbrHex(a.vertex_shader)},
            {"pixel_shader",PbrHex(a.pixel_shader)},{"stage",int64_t(a.stage)},{"uv_set",int64_t(a.uv_set)},
            {"surface",a.surface==gpu::pbr::SurfaceClass::World?"world":"vehicle"},{"cutout",a.cutout},
            {"enabled",entry.enabled},{"use_global_parameters",entry.use_global_parameters},
            {"parameters",PbrParametersTable(entry.parameters)}});
    }
    result.insert("materials",std::move(rows));return result;
}

// config/mc3_enhancements.toml beside the exe: the settings app.h lists under it. An earlier session's version of the
// file held many more keys that nothing read; those are ignored now and dropped when the file is next written.
std::filesystem::path EnhancementsFile() { return ExeDir() / "config" / "mc3_enhancements.toml"; }

void ClampDeadzones(Settings& s) {
    s.deadzone_inner = std::clamp(s.deadzone_inner, 0.0f, 0.9f);
    s.deadzone_outer = std::clamp(s.deadzone_outer, s.deadzone_inner + 0.05f, 1.0f);
}

// after the settings file: graphics_keys says which [graphics] keys that file set, so an older name here for one of
// them only applies when settings.toml does not have it
void LoadEnhancements(Settings& s, const toml::table* graphics_keys) {
    toml::table t;
    try {
        t = toml::parse_file(EnhancementsFile().wstring());
    } catch (...) {
        return;  // optional
    }
    if (const toml::table* r = t["render"].as_table()) {
        s.force_1080p = (*r)["force_1080p"].value_or(s.force_1080p);
        s.force_widescreen = (*r)["force_widescreen"].value_or(s.force_widescreen);
    }
    // [input]; the first version of the file had the deadzones under [gameplay]
    const toml::table* in = t["input"].as_table();
    if (!in) in = t["gameplay"].as_table();
    if (in) {
        s.deadzone_inner = (*in)["deadzone_inner"].value_or(s.deadzone_inner);
        s.deadzone_outer = (*in)["deadzone_outer"].value_or(s.deadzone_outer);
    }
    ClampDeadzones(s);
    // older names for [graphics] settings, read once: the next save keeps them in settings.toml only
    auto unset = [&](const char* key) { return !graphics_keys || !graphics_keys->contains(key); };
    if (unset("sixty_fps"))
        if (auto v = t["gameplay"]["force_60fps"].value<bool>()) s.graphics.sixty = *v;
    if (unset("show_fps"))
        if (auto v = t["debug"]["show_fps"].value<bool>()) s.graphics.show_fps = *v;
}

}  // namespace

Settings& Get() {
    static Settings s;
    return s;
}

void Load() {
    Settings& s = Get();
    toml::table t;
    try {
        t = toml::parse_file(File().wstring());
    } catch (...) {
        // none yet (first run) or unreadable: the defaults, and the game's load hook and the enhancements file
        // still run
        t = toml::table{};
    }
    std::string mode = t["display_mode"].value_or(std::string(kModes[uint32_t(s.display_mode)]));
    for (uint32_t i = 0; i < 3; i++)
        if (mode == kModes[i]) s.display_mode = DisplayMode(i);
    s.width = t["width"].value_or(s.width);
    s.height = t["height"].value_or(s.height);
    s.render_percent = t["render_percent"].value_or(s.render_percent);
    s.fps_cap = t["fps_cap"].value_or(s.fps_cap);
    s.vsync = t["vsync"].value_or(s.vsync);
    s.aniso = t["anisotropic_filtering"].value_or(s.aniso);
    s.join_ip = t["join_ip"].value_or(s.join_ip);
    for (const Option& o : kGame.options) {
        if (o.flag) *o.flag = t[o.key].value_or(*o.flag);
        else *o.value = t[o.key].value_or(*o.value);
    }
    if (kGame.load) kGame.load(&t);
    if (const toml::table* g = t["tuning"].as_table())
        for (const GraphicsField& f : TuningFields(s.tuning)) {
            if (f.f) {
                float v = (*g)[f.key].value_or(*f.f);
                if (std::isfinite(v)) *f.f = std::clamp(v, f.lo, f.hi);
            }
            if (f.b) *f.b = (*g)[f.key].value_or(*f.b);
        }
    if (const toml::table* g = t["graphics"].as_table()) {
        for (const GraphicsField& f : GraphicsFields(s.graphics)) {
            if (f.f) {
                float v = (*g)[f.key].value_or(*f.f);
                if (std::isfinite(v)) *f.f = std::clamp(v, f.lo, f.hi);
            }
            if (f.b) *f.b = (*g)[f.key].value_or(*f.b);
            if (f.u) *f.u = std::min<uint32_t>((*g)[f.key].value_or(*f.u), uint32_t(f.hi));
        }
    }
    if (const toml::table* w = t["world"].as_table())
        for (const GraphicsField& f : WorldFields(s.world)) {
            float v = (*w)[f.key].value_or(*f.f);
            if (std::isfinite(v)) *f.f = std::clamp(v, f.lo, f.hi);
        }
    if (const toml::table* c = t["capture"].as_table())
        for (const GraphicsField& f : CaptureFields(s.capture)) {
            if (f.f) {
                float v = (*c)[f.key].value_or(*f.f);
                if (std::isfinite(v)) *f.f = std::clamp(v, f.lo, f.hi);
            }
            if (f.b) *f.b = (*c)[f.key].value_or(*f.b);
            if (f.u)
                if (auto v = (*c)[f.key].value<int64_t>()) *f.u = uint32_t(std::clamp(*v, int64_t(f.lo), int64_t(f.hi)));
        }
    LoadPbr(t,s.pbr);
    LoadEnhancements(s, t["graphics"].as_table());
    s.render_percent = std::clamp(s.render_percent, 25u, 400u);
    s.width = std::min(s.width, resolution::kMaxDimension);
    s.height = std::min(s.height, resolution::kMaxDimension);
    if (!s.width || !s.height) s.width = s.height = 0;
    if (!resolution::ValidScene({s.graphics.scene_width, s.graphics.scene_height}))
        s.graphics.scene_width = s.graphics.scene_height = 0;
}

namespace {

toml::table Table() {
    const Settings& s = Get();
    toml::table t{{"display_mode", kModes[uint32_t(s.display_mode)]},
                  {"width", int64_t(s.width)},
                  {"height", int64_t(s.height)},
                  {"render_percent", int64_t(s.render_percent)},
                  {"fps_cap", int64_t(s.fps_cap)},
                  {"vsync", s.vsync},
                  {"anisotropic_filtering", int64_t(s.aniso)},
                  {"join_ip", s.join_ip}};
    for (const Option& o : kGame.options) {
        if (o.flag) t.insert(o.key, *o.flag);
        else t.insert(o.key, int64_t(*o.value));
    }
    if (kGame.save) kGame.save(&t);
    toml::table g;
    for (const GraphicsField& f : GraphicsFields(const_cast<Settings&>(s).graphics)) {
        if (f.f) g.insert(f.key, double(*f.f));
        if (f.b) g.insert(f.key, *f.b);
        if (f.u) g.insert(f.key, int64_t(*f.u));
    }
    t.insert("graphics", std::move(g));
    toml::table v;
    for (const GraphicsField& f : TuningFields(const_cast<Settings&>(s).tuning)) {
        if (f.f) v.insert(f.key, double(*f.f));
        if (f.b) v.insert(f.key, *f.b);
    }
    t.insert("tuning", std::move(v));
    toml::table w;
    for (const GraphicsField& f : WorldFields(const_cast<Settings&>(s).world))
        w.insert(f.key, double(*f.f));
    t.insert("world", std::move(w));
    toml::table c;
    for (const GraphicsField& f : CaptureFields(const_cast<Settings&>(s).capture)) {
        if (f.f) c.insert(f.key, double(*f.f));
        if (f.b) c.insert(f.key, *f.b);
        if (f.u) c.insert(f.key, int64_t(*f.u));
    }
    t.insert("capture", std::move(c));
    t.insert("pbr",PbrTable(s.pbr));
    return t;
}

toml::table EnhancementsTable() {
    const Settings& s = Get();
    toml::table t;
    t.insert("render", toml::table{{"force_1080p", s.force_1080p}, {"force_widescreen", s.force_widescreen}});
    t.insert("input", toml::table{{"deadzone_inner", double(s.deadzone_inner)}, {"deadzone_outer", double(s.deadzone_outer)}});
    return t;
}

}  // namespace

void Save() {
    std::ofstream f(File());
    f << Table() << "\n";
    std::error_code ec;
    std::filesystem::create_directories(EnhancementsFile().parent_path(), ec);
    std::ofstream ef(EnhancementsFile());
    ef << EnhancementsTable() << "\n";
    if (kGame.save_aux) kGame.save_aux();
}

void LogSettings() {
    std::ostringstream text;
    text << Table() << "\n" << hostpath::Utf8(EnhancementsFile().filename()) << ":\n" << EnhancementsTable();
    std::istringstream lines(text.str());
    LOGR("Settings:");
    for (std::string line; std::getline(lines, line);)
        if (!line.empty()) LOGR("  %s", line.c_str());
}

Mode Desktop() {
    HMONITOR mon = MonitorFromWindow(static_cast<HWND>(Window()), MONITOR_DEFAULTTOPRIMARY);
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    if (GetMonitorInfoW(mon, &mi) && EnumDisplaySettingsW(mi.szDevice, ENUM_REGISTRY_SETTINGS, &dm))
        return {dm.dmPelsWidth, dm.dmPelsHeight};
    return {uint32_t(GetSystemMetrics(SM_CXSCREEN)), uint32_t(GetSystemMetrics(SM_CYSCREEN))};
}

Mode Output() {
    const Settings& s = Get();
    if (s.force_1080p) return {1920, 1080};
    if (s.width && s.height) return {s.width, s.height};
    return Desktop();
}

Mode RenderResolution() {
    const Settings& s = Get();
    return resolution::Scene({s.graphics.scene_width, s.graphics.scene_height}, Output(), s.render_percent);
}

namespace { std::atomic<uint64_t> g_actual_scene{0}; }
Mode ActualRenderResolution() {
    uint64_t v = g_actual_scene.load(std::memory_order_relaxed);
    return {uint32_t(v >> 32), uint32_t(v)};
}
void ReportRenderResolution(Mode m) {
    g_actual_scene.store(uint64_t(m.width) << 32 | m.height, std::memory_order_relaxed);
}

bool Widescreen() {
    // asked per frame and per HUD quad: the desktop mode lookup only runs when the resolution setting changes
    static std::atomic<uint64_t> key{~0ull};
    static std::atomic<uint64_t> scene_key{~0ull};
    static std::atomic<bool> wide{true};
    const Settings& s = Get();
    // the two force switches are in the key too: they change from the settings menu while the game runs
    uint64_t k = uint64_t(s.width) << 34 | uint64_t(s.height) << 2 | uint64_t(s.force_widescreen) << 1 | uint64_t(s.force_1080p);
    uint64_t sk = uint64_t(s.graphics.scene_width) << 32 | s.graphics.scene_height;
    bool changed = key.exchange(k) != k;
    changed |= scene_key.exchange(sk) != sk;
    if (changed) {
        Mode explicit_scene{s.graphics.scene_width, s.graphics.scene_height};
        Mode m = resolution::ValidScene(explicit_scene) ? explicit_scene : Output();
        wide = s.force_widescreen || m.width * 2 >= m.height * 3;  // 3:2 or wider
    }
    return wide;
}

float RenderScale(uint32_t guest_height) {
    float h = float(RenderResolution().height);
    return guest_height ? std::max(h / guest_height, 0.1f) : 1.0f;
}

// the renderer (gpu/d3d12.cpp) declares and calls this name; force_1080p is already in Output(), so it is RenderScale
float EnhancedRenderScale(uint32_t guest_height) { return RenderScale(guest_height); }

}  // namespace app
