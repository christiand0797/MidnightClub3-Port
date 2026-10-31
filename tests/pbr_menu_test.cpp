#include "app/app.h"
#include "gpu/pbr_controls.h"
#include <cassert>
#include <cstdio>
#include <iostream>
#include <memory>
#include <imgui.h>
#include <imgui_internal.h>

namespace fixture {
std::shared_ptr<const gpu::pbr::controls::CatalogSnapshot> catalog;
}
namespace gpu::pbr::controls {
std::shared_ptr<const CatalogSnapshot> GetCatalog(){return fixture::catalog;}
const char* ReasonText(Reason){return "Fixture actual renderer reason";}
const char* RuntimeStatusText(RuntimeStatus){return "Fixture actual renderer status";}
}
// Exercise production ImGui controls without a window/device, pack loader or game.
#include "../runtime/app/pbr_menu.cpp"

namespace {
namespace pc=gpu::pbr::controls;
app::Settings settings;
pc::Capabilities capabilities;
enum class Page{Full,Parameters};
bool Frame(Page page=Page::Full) {
    auto& io=ImGui::GetIO();io.DisplaySize={640,480};io.DeltaTime=1.0f/60;
    ImGui::NewFrame();ImGui::SetNextWindowPos({8,8},ImGuiCond_Always);ImGui::SetNextWindowSize({624,464},ImGuiCond_Always);
    ImGui::Begin("PBR CPU menu",nullptr,ImGuiWindowFlags_NoSavedSettings);
    const bool dirty=page==Page::Full?app::DrawPbrSettings(settings):app::Parameters(settings.pbr.parameters,capabilities,true,true);
    ImGui::End();ImGui::Render();
    assert(ImGui::GetDrawData()&&ImGui::GetDrawData()->DisplaySize.y==480);
    return dirty;
}
ImGuiWindow* Window(){auto* w=ImGui::FindWindowByName("PBR CPU menu");assert(w);return w;}
void ScrollBottom(Page page=Page::Full){ImGui::SetScrollY(Window(),Window()->ScrollMax.y);Frame(page);}
void Top(Page page=Page::Full){ImGui::SetScrollY(Window(),0);Frame(page);}
bool Click(const char* text,Page page=Page::Full){ImGui::ActivateItemByID(Window()->GetID(text));return Frame(page);}
void Catalog(const pc::ExactMaterialKey& first,const pc::ExactMaterialKey& second) {
    pc::CatalogSnapshot c;c.status=pc::RuntimeStatus::BindingsUnavailable;c.manifest_sha256=first.approved_manifest_sha256;
    c.materials.push_back({first,true,true,pc::Reason::RendererBindingsUnavailable,7,0,false});
    c.materials.push_back({second,true,false,pc::Reason::RendererBindingsUnavailable,2,0,false});
    fixture::catalog=std::make_shared<const pc::CatalogSnapshot>(std::move(c));
}
}

int main(){
    ImGui::CreateContext();auto& io=ImGui::GetIO();io.IniFilename=nullptr;io.ConfigFlags|=ImGuiConfigFlags_NavEnableKeyboard;
    unsigned char* pixels;int w,h;io.Fonts->GetTexDataAsRGBA32(&pixels,&w,&h);
    assert(!settings.pbr.enabled&&settings.pbr.materials.empty());
    assert(!Frame());assert(!Frame()); // Viewing unavailable controls cannot activate or mutate preferences.
    assert(Click("Enable PBR")&&settings.pbr.enabled);assert(Click("Enable PBR")&&!settings.pbr.enabled);
    // A forced navigation activation cannot bypass the invalid-pack guard.
    assert(!Click("Apply pack selection")&&settings.pbr.pack_path.empty());
    std::snprintf(app::g_pack_draft.path.data(),app::g_pack_draft.path.size(),"materials/reviewed");
    std::snprintf(app::g_pack_draft.digest.data(),app::g_pack_draft.digest.size(),"%s",std::string(64,'A').c_str());
    assert(Click("Apply pack selection"));
    assert(settings.pbr.pack_path=="materials/reviewed"&&settings.pbr.approved_manifest_sha256==std::string(64,'a'));
    gpu::pbr::Assignment a{0xFEDCBA9876543210ull,0xC123456789ABCDEFull,0xD23456789ABCDEF1ull,0,0,gpu::pbr::SurfaceClass::World,false};
    const auto first=pc::ExactKey(a,std::string(64,'a'));a.vertex_shader^=1;const auto second=pc::ExactKey(a,std::string(64,'a'));
    Catalog(first,second);Frame();Window()->StateStorage.SetInt(Window()->GetID("Global material defaults"),0);Frame();ScrollBottom();
    assert(settings.pbr.materials.empty());
    assert(Click("Enable this texture"));
    assert(settings.pbr.materials.size()==1&&!pc::FindOverride(settings.pbr,first)->enabled&&!pc::FindOverride(settings.pbr,second));
    app::g_selected=second;Frame();ScrollBottom();
    assert(Click("Use global parameters for this texture"));
    assert(settings.pbr.materials.size()==2&&!pc::FindOverride(settings.pbr,second)->use_global_parameters);
    assert(!pc::FindOverride(settings.pbr,first)->enabled); // Same source texture/different shader must remain independent.
    app::g_selected=first;Frame();ScrollBottom();
    assert(Click("Reset this texture to global defaults"));
    assert(!pc::FindOverride(settings.pbr,first)&&pc::FindOverride(settings.pbr,second));
    // A different approved manifest cannot expose stale catalog controls as current.
    settings.pbr.approved_manifest_sha256=std::string(64,'b');Frame();ScrollBottom();
    assert(!Click("Enable this texture")&&settings.pbr.materials.size()==1);
    settings.pbr.approved_manifest_sha256.clear();Frame();ScrollBottom();
    assert(!Click("Enable this texture")&&settings.pbr.materials.size()==1);
    // Distinct height modes require corresponding real capabilities, not an enabled preference.
    settings.pbr.parameters={};Frame(Page::Parameters);ScrollBottom(Page::Parameters);
    assert(!Click("Parallax (UV shift)",Page::Parameters));assert(settings.pbr.parameters.height_mode==pc::HeightMode::None);
    assert(!Click("Geometry displacement (moves vertices)",Page::Parameters));assert(settings.pbr.parameters.height_mode==pc::HeightMode::None);
    capabilities.geometry=capabilities.view=capabilities.height_units=capabilities.parallax=true;
    ScrollBottom(Page::Parameters);assert(Click("Parallax (UV shift)",Page::Parameters));
    assert(settings.pbr.parameters.height_mode==pc::HeightMode::Parallax);
    assert(!Click("Geometry displacement (moves vertices)",Page::Parameters));
    capabilities.displacement=true;ScrollBottom(Page::Parameters);
    assert(!Click("Geometry displacement (moves vertices)",Page::Parameters));
    assert(settings.pbr.parameters.height_mode==pc::HeightMode::Parallax); // The backend also requires real tessellation.
    capabilities.tessellation=true;ScrollBottom(Page::Parameters);
    assert(Click("Geometry displacement (moves vertices)",Page::Parameters));
    assert(settings.pbr.parameters.height_mode==pc::HeightMode::Displacement);
    assert(!settings.pbr.enabled); // Editing geometry preferences never enables the renderer.
    ImGui::DestroyContext();std::cout<<"PBR menu default-off, exact material identity, capability gates and480p controls passed\n";
}
