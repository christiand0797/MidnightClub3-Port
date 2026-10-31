#include "app/pbr_menu.h"
#include "app/app.h"
#include "gpu/pbr_controls.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <imgui.h>

namespace app {
namespace {
namespace pc = gpu::pbr::controls;
struct PackDraft {
    std::array<char,pc::kMaxPackPathBytes+1> path{};
    std::array<char,65> digest{};
    std::string committed_path,committed_digest;
    bool initialized=false;
};
PackDraft g_pack_draft;
pc::ExactMaterialKey g_selected;
ImGuiTextFilter g_material_filter;
void SyncPackDraft(const pc::Settings& s) {
    if(g_pack_draft.initialized&&g_pack_draft.committed_path==s.pack_path&&
       g_pack_draft.committed_digest==s.approved_manifest_sha256)return;
    g_pack_draft={};g_pack_draft.initialized=true;
    g_pack_draft.committed_path=s.pack_path;g_pack_draft.committed_digest=s.approved_manifest_sha256;
    std::snprintf(g_pack_draft.path.data(),g_pack_draft.path.size(),"%s",s.pack_path.c_str());
    std::snprintf(g_pack_draft.digest.data(),g_pack_draft.digest.size(),"%s",s.approved_manifest_sha256.c_str());
}
void Help(const char* text) { ImGui::TextWrapped("%s",text); }
bool Parameters(pc::Parameters& p,const pc::Capabilities& caps,bool height_map,bool emission_map) {
    const auto before=p;
    constexpr ImGuiSliderFlags clamp=ImGuiSliderFlags_AlwaysClamp;
    ImGui::ColorEdit3("Base color (linear RGB)",p.base_color.data(),ImGuiColorEditFlags_Float|ImGuiColorEditFlags_HDR);
    ImGui::Checkbox("Override roughness",&p.roughness_override);
    if(p.roughness_override)ImGui::SliderFloat("Roughness",&p.roughness,0,1,"%.3f",clamp);
    else ImGui::SliderFloat("Roughness map factor",&p.roughness_factor,0,4,"x%.2f",clamp);
    ImGui::Checkbox("Override metallic",&p.metallic_override);
    if(p.metallic_override)ImGui::SliderFloat("Metallic",&p.metallic,0,1,"%.3f",clamp);
    else ImGui::SliderFloat("Metallic map factor",&p.metallic_factor,0,4,"x%.2f",clamp);
    ImGui::SliderFloat("Normal strength",&p.normal_strength,0,4,"x%.2f",clamp);
    ImGui::SliderFloat("Ambient occlusion strength",&p.ao_strength,0,2,"x%.2f",clamp);
    ImGui::BeginDisabled(!emission_map);
    auto emission=p.emission_color;float strength=p.emission_strength;
    const bool color_changed=ImGui::ColorEdit3("Emission color (linear RGB)",emission.data(),ImGuiColorEditFlags_Float|ImGuiColorEditFlags_HDR);
    const bool strength_changed=ImGui::SliderFloat("Emission strength",&strength,0,16,"x%.2f",clamp);
    if(emission_map&&color_changed)p.emission_color=emission;
    if(emission_map&&strength_changed)p.emission_strength=strength;
    ImGui::EndDisabled();
    if(!emission_map)Help("This assignment has no reviewed emission map.");
    ImGui::Separator();
    ImGui::TextUnformatted("Height treatment");
    if(ImGui::RadioButton("No height effect",p.height_mode==pc::HeightMode::None))p.height_mode=pc::HeightMode::None;
    const bool parallax=height_map&&caps.parallax&&caps.geometry&&caps.view&&caps.height_units;
    const bool displacement=height_map&&caps.displacement&&caps.tessellation&&caps.geometry&&caps.view&&caps.height_units;
    ImGui::BeginDisabled(!parallax);
    if(ImGui::RadioButton("Parallax (UV shift)",p.height_mode==pc::HeightMode::Parallax)&&parallax)p.height_mode=pc::HeightMode::Parallax;
    float parallax_height=p.parallax_height;
    if(ImGui::SliderFloat("Parallax height (draw units)",&parallax_height,0,0.1f,"%.4f",clamp)&&parallax)p.parallax_height=parallax_height;
    ImGui::EndDisabled();
    ImGui::BeginDisabled(!displacement);
    if(ImGui::RadioButton("Geometry displacement (moves vertices)",p.height_mode==pc::HeightMode::Displacement)&&displacement)p.height_mode=pc::HeightMode::Displacement;
    float displacement_height=p.displacement_height,midpoint=p.displacement_midpoint;
    if(ImGui::SliderFloat("Displacement height (draw units)",&displacement_height,0,0.25f,"%.4f",clamp)&&displacement)p.displacement_height=displacement_height;
    if(ImGui::SliderFloat("Displacement midpoint",&midpoint,0,1,"%.3f",clamp)&&displacement)p.displacement_midpoint=midpoint;
    ImGui::EndDisabled();
    const bool tessellation=displacement&&caps.tessellation;
    ImGui::BeginDisabled(!tessellation);
    int subdivisions=int(p.tessellation);
    if(ImGui::SliderInt("Tessellation factor",&subdivisions,1,16,"%d",clamp)&&tessellation)p.tessellation=uint32_t(subdivisions);
    ImGui::EndDisabled();
    if(!height_map)Help("A reviewed height map is required for either height effect.");
    else if(!parallax||!displacement)Help("Unavailable height modes stay disabled until this renderer establishes geometry, view and height units. Parallax changes UVs; geometry displacement changes vertices. Extra silhouette detail also needs sufficient geometry.");
    if(p.height_mode==pc::HeightMode::Parallax&&!parallax)Help("Saved parallax request is unavailable in the current renderer and will fall back.");
    if(p.height_mode==pc::HeightMode::Displacement&&!displacement)Help("Saved geometry displacement request is unavailable in the current renderer and will fall back.");
    if(ImGui::Button("Reset parameters"))p={};
    if(!(p==before)){pc::Sanitize(p);return true;}
    return false;
}
std::string MaterialLabel(const pc::ExactMaterialKey& key) {
    const auto& a=key.assignment;char label[180];
    std::snprintf(label,sizeof(label),"%016llX | %s | stage %u UV%u | VS %016llX PS %016llX",
        static_cast<unsigned long long>(a.original_texture),a.surface==gpu::pbr::SurfaceClass::World?"World":"Vehicle",
        a.stage,a.uv_set,static_cast<unsigned long long>(a.vertex_shader),static_cast<unsigned long long>(a.pixel_shader));
    return label;
}
}

bool DrawPbrSettings(Settings& settings) {
    auto& s=settings.pbr;bool changed=false;
    const auto catalog=pc::GetCatalog();
    changed|=ImGui::Checkbox("Enable PBR",&s.enabled);
    Help("Toggle for an A/B comparison. Only reviewed materials on compatible draws use PBR; unsupported draws retain the original shading. These are PC preferences and do not change career saves.");
    if(catalog) {
        const bool real_ready=pc::ShadingReady(catalog->capabilities)&&catalog->ready_materials>0;
        ImGui::Text("Last renderer report: %s",catalog->status==pc::RuntimeStatus::Ready&&!real_ready?
            "Waiting for eligible rendered draws":pc::RuntimeStatusText(catalog->status));
        ImGui::Text("Materials with rendered PBR evidence: %u",real_ready?catalog->ready_materials:0);
        if(!catalog->message.empty())ImGui::TextWrapped("%s",catalog->message.c_str());
    } else Help("Renderer has not published PBR capabilities. Enabling the preference does not establish an active PBR path.");
    SyncPackDraft(s);
    ImGui::Separator();ImGui::TextUnformatted("Reviewed material pack");
    ImGui::InputText("Pack folder",g_pack_draft.path.data(),g_pack_draft.path.size());
    ImGui::InputText("Approved manifest SHA-256",g_pack_draft.digest.data(),g_pack_draft.digest.size(),ImGuiInputTextFlags_CharsHexadecimal);
    const bool valid_pack=pc::ValidPackPath(g_pack_draft.path.data())&&pc::ValidDigest(g_pack_draft.digest.data());
    ImGui::BeginDisabled(!valid_pack);
    if(ImGui::Button("Apply pack selection")&&valid_pack) {
        s.pack_path=g_pack_draft.path.data();s.approved_manifest_sha256=g_pack_draft.digest.data();
        pc::Sanitize(s);changed=true;SyncPackDraft(s);
    }
    ImGui::EndDisabled();
    Help("Use the digest of an independently reviewed materials-v1.toml. Choosing a folder does not approve its contents or convert textures automatically.");
    int quality=int(s.quality);constexpr const char* qualities[]={"Low","Medium","High","Ultra"};
    if(ImGui::Combo("PBR quality",&quality,qualities,4)){s.quality=pc::Quality(quality);changed=true;}
    const bool current=catalog&&pc::DigestEqual(catalog->manifest_sha256,s.approved_manifest_sha256);
    const pc::Capabilities caps=current?catalog->capabilities:pc::Capabilities{};
    const bool any_height=current&&std::any_of(catalog->materials.begin(),catalog->materials.end(),[](const auto& m){return m.has_height;});
    if(ImGui::CollapsingHeader("Global material defaults",ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::PushID("global_pbr");changed|=Parameters(s.parameters,caps,any_height,true);ImGui::PopID();
    }
    ImGui::Separator();ImGui::TextUnformatted("Per-texture material assignments");
    if(!current||catalog->materials.empty()) {
        Help("Load the selected reviewed pack to list exact texture/shader assignments. Saved overrides remain stored and are never retargeted to another pack.");
        return changed;
    }
    const size_t count=std::min(catalog->materials.size(),pc::kMaxMaterials);
    const pc::CatalogMaterial* selected=nullptr;
    for(size_t i=0;i<count;++i)if(pc::KeyEqual(g_selected,catalog->materials[i].key))selected=&catalog->materials[i];
    if(!selected){selected=&catalog->materials[0];g_selected=selected->key;}
    const auto preview=MaterialLabel(selected->key);
    if(ImGui::BeginCombo("Texture / exact assignment",preview.c_str())) {
        g_material_filter.Draw("Filter texture / shader");
        std::vector<size_t> matches;matches.reserve(count);
        for(size_t i=0;i<count;++i)
            if(!g_material_filter.IsActive()||g_material_filter.PassFilter(MaterialLabel(catalog->materials[i].key).c_str()))matches.push_back(i);
        ImGuiListClipper clipper;clipper.Begin(int(matches.size()));
        while(clipper.Step())for(int row=clipper.DisplayStart;row<clipper.DisplayEnd;++row) {
            const size_t i=matches[size_t(row)];const auto& item=catalog->materials[i];ImGui::PushID(int(i));
            if(ImGui::Selectable(MaterialLabel(item.key).c_str(),pc::KeyEqual(g_selected,item.key))){g_selected=item.key;selected=&item;}
            ImGui::PopID();
        }
        if(matches.empty())ImGui::TextDisabled("No assignments match the filter.");
        ImGui::EndCombo();
    }
    ImGui::TextWrapped("%s",MaterialLabel(selected->key).c_str());
    ImGui::TextWrapped("Assignment: %s",pc::ReasonText(selected->reason));
    ImGui::Text("Observed draws: %llu; PBR draws: %llu",static_cast<unsigned long long>(selected->observed_draws),static_cast<unsigned long long>(selected->rendered_draws));
    const bool exact=pc::ValidAssignment(selected->key.assignment)&&pc::DigestEqual(selected->key.approved_manifest_sha256,s.approved_manifest_sha256);
    const auto ensure=[&]() -> pc::MaterialOverride* {
        if(!exact)return nullptr;
        if(auto* row=pc::FindOverride(s,selected->key))return row;
        if(s.materials.size()>=pc::kMaxMaterials)return nullptr;
        s.materials.push_back({selected->key,true,true,s.parameters});return &s.materials.back();
    };
    const auto* existing=pc::FindOverride(s,selected->key);
    ImGui::BeginDisabled(!exact);
    bool enabled=!existing||existing->enabled;
    if(ImGui::Checkbox("Enable this texture",&enabled)&&exact)if(auto* row=ensure()){row->enabled=enabled;changed=true;}
    existing=pc::FindOverride(s,selected->key);
    bool use_global=!existing||existing->use_global_parameters;
    if(ImGui::Checkbox("Use global parameters for this texture",&use_global)&&exact)
        if(auto* row=ensure()){row->use_global_parameters=use_global;changed=true;}
    existing=pc::FindOverride(s,selected->key);
    if(exact&&existing&&!existing->use_global_parameters) {
        ImGui::PushID("texture_pbr");
        if(auto* row=pc::FindOverride(s,selected->key))changed|=Parameters(row->parameters,caps,selected->has_height,selected->has_emission);
        ImGui::PopID();
    }
    if(ImGui::Button("Reset this texture to global defaults")&&exact) {
        const auto before=s.materials.size();
        std::erase_if(s.materials,[&](const auto& row){return pc::KeyEqual(row.key,selected->key);});
        changed|=before!=s.materials.size();
    }
    ImGui::EndDisabled();
    if(s.materials.size()>=pc::kMaxMaterials)ImGui::TextWrapped("The %u-assignment preference limit is reached. Reset an unused override before adding another.",unsigned(pc::kMaxMaterials));
    return changed;
}
}
