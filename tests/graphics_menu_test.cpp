#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cassert>
#include <filesystem>
#include <mutex>
#include <imgui.h>
#include <imgui_internal.h>
#include "app/app.h"
#include "app/ui_font.h"
#include "core/input.h"
#include "core/log.h"

namespace fixture { std::filesystem::path folder; int applies = 0; }
namespace app {
const GameInfo kGame{.title = L"Graphics menu component test", .settings = "settings.toml"};
std::filesystem::path ExeDir() { return fixture::folder; }
void* Window() { return nullptr; }
std::recursive_mutex& UiLock() { static std::recursive_mutex lock; return lock; }
void ApplyDisplay() { fixture::applies++; }
}
namespace input { Pad RawPad(int) { return {}; } }
namespace logx { void Write(Level, const char*, ...) {} }

// Exercise the real UI and its transaction/recovery functions, without a game or GPU.
#include "../runtime/app/settings_menu.cpp"

void Frame(float w = 640, float h = 480) {
    auto& io = ImGui::GetIO();
    io.DisplaySize = {w, h}; io.DeltaTime = 1.0f / 60;
    ImGui::NewFrame();
    app::DrawSettingsMenu(60);
    ImGui::Render();
}
void Preview(app::Mode scene) {
    auto& s = app::Get();
    app::g_previous = app::Selection(s);
    s.graphics.scene_width = scene.width; s.graphics.scene_height = scene.height;
    app::g_confirm_display = true;
    app::g_display_deadline = app::DisplayClock::now() + std::chrono::seconds(20);
}

int main() {
    namespace fs = std::filesystem;
    auto temp = fs::absolute(fs::temp_directory_path());
    fixture::folder = temp / ("mc3_graphics_menu_test_" + std::to_string(GetCurrentProcessId()) + "_" + std::to_string(GetTickCount64()));
    assert(fs::equivalent(fixture::folder.parent_path(), temp) && fs::create_directory(fixture::folder));
    ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    app::ConfigureUiFont(io);
    unsigned char* pixels; int fw, fh;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &fw, &fh);
    const ImFontGlyph* glyph = io.Fonts->Fonts[0]->FindGlyphNoFallback('M');
    assert(glyph && glyph->Y1 - glyph->Y0 >= 12);
    auto& s = app::Get();
    s.width = 1280; s.height = 720;
    s.graphics.scene_width = 1920; s.graphics.scene_height = 1080;
    app::ToggleMenu();
    for (app::Mode display : {app::Mode{640, 480}, {1280, 720}, {3840, 2160}}) {
        Frame(float(display.width), float(display.height));
        Frame(float(display.width), float(display.height));
        auto* window = ImGui::FindWindowByName("Game settings");
        assert(window && window->Pos.x >= 0 && window->Pos.y >= 0);
        assert(window->Pos.x + window->Size.x <= display.width + 1);
        assert(window->Pos.y + window->Size.y <= display.height + 1);
        assert(io.FontDefault == nullptr || io.FontDefault->FontSize >= 20);
        assert(io.Fonts->Fonts[0]->FontSize >= 20);
        if (display.height == 480) {
            bool scrollable_contents = false;
            for (ImGuiWindow* child : GImGui->Windows)
                if (child->ParentWindow == window && (child->Flags & ImGuiWindowFlags_ChildWindow) && child->ScrollMax.y > 0)
                    scrollable_contents = true;
            assert(scrollable_contents);
        }
    }
    // Previewing a mode does not persist it, and cancelling restores only display settings.
    Preview({15360, 8640});
    s.graphics.brightness = .2f;
    app::ReportRenderResolution({15360, 8640});
    Frame();
    assert(!fs::exists(fixture::folder / "settings.toml"));
    app::ReportGraphicsFailure("simulated GPU allocation rejection");
    assert(!app::g_confirm_display && s.graphics.scene_width == 1920 && s.graphics.scene_height == 1080);
    assert(s.graphics.brightness == .2f && app::MenuOpen() && fixture::applies == 1);
    Frame();
    // A timed-out preview and closing with F1 both restore the last accepted selection.
    Preview({7680, 4320});
    app::g_display_deadline = app::DisplayClock::now() - std::chrono::seconds(1);
    Frame();
    assert(!app::g_confirm_display && s.graphics.scene_width == 1920 && fixture::applies == 2);
    Preview({2560, 1440});
    app::ToggleMenu();
    assert(!app::MenuOpen() && !app::g_confirm_display && s.graphics.scene_width == 1920);
    assert(!fs::exists(fixture::folder / "settings.toml"));
    // Recovery from a copied oversized startup configuration remains interactive and saves a working fallback.
    s.graphics.scene_width = 15360; s.graphics.scene_height = 8640;
    app::ReportGraphicsFailure("startup allocation rejection");
    assert(app::MenuOpen() && s.graphics.scene_width == 1280 && s.graphics.scene_height == 720);
    Frame();
    app::ReportGraphicsFailure("fallback allocation rejection");
    assert(s.graphics.scene_width == 640 && s.graphics.scene_height == 480);
    Frame();
    app::ToggleMenu();
    assert(fs::is_regular_file(fixture::folder / "settings.toml"));
    s = app::Settings{};
    app::Load();
    assert(s.graphics.scene_width == 640 && s.graphics.scene_height == 480);
    ImGui::DestroyContext();
    assert(fs::equivalent(fixture::folder.parent_path(), temp));
    assert(fs::remove(fixture::folder / "settings.toml"));
    assert(fs::remove(fixture::folder / "config" / "mc3_enhancements.toml"));
    assert(fs::remove(fixture::folder / "config"));
    assert(fs::remove(fixture::folder));
}
