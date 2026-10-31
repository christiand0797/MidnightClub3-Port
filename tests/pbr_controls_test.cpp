#include "gpu/pbr_controls.h"

#include <atomic>
#include <cassert>
#include <cmath>
#include <iostream>
#include <limits>
#include <thread>

using namespace gpu::pbr;
using namespace gpu::pbr::controls;
namespace {
constexpr float nan=std::numeric_limits<float>::quiet_NaN();
constexpr float infinity=std::numeric_limits<float>::infinity();
void Near(float a,float b) {assert(std::isfinite(a)&&std::abs(a-b)<0.00001f);}
Assignment AssignmentOf(uint64_t texture=1,uint64_t vs=2,uint64_t ps=3) {return {texture,vs,ps,0,0,SurfaceClass::World,false};}
struct Fixture {
    Settings settings;
    Library library;
    SurfaceEvidence draw;
    Capabilities bindings;
    Fixture() {
        settings.enabled=true;settings.pack_path="synthetic-reviewed-pack";settings.approved_manifest_sha256=std::string(64,'a');
        library.status=LoadStatus::Ready;library.manifest_sha256=settings.approved_manifest_sha256;
        Material m;m.assignment=AssignmentOf();m.review_sha256=std::string(64,'b');
        m.albedo.levels.push_back({1,1,{150,100,50,255}});m.normal.levels.push_back({1,1,{128,128,255,255}});
        m.orm.levels.push_back({1,1,{255,128,20,255}});m.baseline_alpha_sha256=AlphaDigest(m.albedo.levels[0]);
        library.materials.push_back(m);
        draw.assignment=m.assignment;draw.color_width=draw.color_height=1;draw.current_alpha_sha256=m.baseline_alpha_sha256;
        draw.texture_2d=draw.geometric_position=draw.geometric_normal=draw.tangent_basis=draw.linear_lighting=true;
        bindings.pipeline=bindings.geometry=bindings.view=bindings.lighting=bindings.linear_output=bindings.material_bindings=true;
    }
    Resolution Get() {return Resolve(settings,library,draw,bindings);}
};
void SettingsPolicy() {
    Settings s;assert(!s.enabled&&s.materials.empty());assert(s.parameters==Parameters{});
    s.parameters.base_color={nan,-5,10};s.parameters.emission_color={infinity,2,3};
    s.parameters.roughness_factor=nan;s.parameters.metallic_factor=100;
    s.parameters.roughness=-3;s.parameters.metallic=infinity;s.parameters.normal_strength=-1;
    s.parameters.ao_strength=3;s.parameters.emission_strength=nan;s.parameters.parallax_height=infinity;
    s.parameters.displacement_height=20;s.parameters.displacement_midpoint=nan;s.parameters.tessellation=0;
    s.parameters.height_mode=static_cast<HeightMode>(30);s.quality=static_cast<Quality>(30);
    s.pack_path=std::string(kMaxPackPathBytes+1,'x');s.approved_manifest_sha256="not-approved";Sanitize(s);
    assert(!s.enabled&&s.pack_path.empty()&&s.approved_manifest_sha256.empty());
    assert(s.quality==Quality::High&&s.parameters.height_mode==HeightMode::None);
    assert((s.parameters.base_color==std::array<float,3>{1,0,4}));
    assert((s.parameters.emission_color==std::array<float,3>{1,2,3}));
    Near(s.parameters.roughness_factor,1);Near(s.parameters.metallic_factor,4);Near(s.parameters.roughness,0);
    Near(s.parameters.metallic,0);Near(s.parameters.normal_strength,0);Near(s.parameters.ao_strength,2);
    Near(s.parameters.emission_strength,1);Near(s.parameters.parallax_height,0);Near(s.parameters.displacement_height,0.25f);
    Near(s.parameters.displacement_midpoint,0.5f);assert(s.parameters.tessellation==1);
    s.pack_path="valid/path";s.approved_manifest_sha256=std::string(64,'A');Sanitize(s);
    assert(s.approved_manifest_sha256==std::string(64,'a')&&s.pack_path=="valid/path");
    s.pack_path=std::string("invalid\0path",12);Sanitize(s);assert(s.pack_path.empty());
    assert(!ValidDigest(std::string(63,'a'))&&!ValidDigest(std::string(64,'g')));

    auto key=ExactKey(AssignmentOf(),std::string(64,'A'));
    MaterialOverride first;first.key=key;first.enabled=false;first.parameters.roughness_factor=nan;
    auto duplicate=first;duplicate.enabled=true;
    auto invalid=first;invalid.key.assignment.original_texture=0;
    auto another=first;another.key.assignment.vertex_shader=8;
    s.materials={invalid,first,duplicate,another};Sanitize(s);
    assert(s.materials.size()==2&&!s.materials[0].enabled&&s.materials[1].key.assignment.vertex_shader==8);
    Near(s.materials[0].parameters.roughness_factor,1);assert(FindOverride(s,key)==&s.materials[0]);
    assert(FindOverride(s,key.assignment,key.approved_manifest_sha256)==&s.materials[0]);
    assert(!FindOverride(s,key.assignment,std::string(64,'b')));
    for(int field=0;field<7;++field) {
        auto changed=key;
        switch(field){case 0:++changed.assignment.original_texture;break;case 1:++changed.assignment.vertex_shader;break;
        case 2:++changed.assignment.pixel_shader;break;case 3:++changed.assignment.stage;break;case 4:++changed.assignment.uv_set;break;
        case 5:changed.assignment.surface=SurfaceClass::Vehicle;break;case 6:changed.assignment.cutout=true;break;}
        assert(!KeyEqual(key,changed));
    }
    s.materials.clear();for(size_t i=0;i<kMaxMaterials+5;++i){auto row=first;row.key.assignment.original_texture=i+1;s.materials.push_back(row);}
    Sanitize(s);assert(s.materials.size()==kMaxMaterials);
    assert(kMaxMaterials==8192&&LoadPolicy{}.max_materials==4096&&LoadPolicy{}.max_decoded_bytes==(1536ull<<20));
    PreparedSettings index(s);assert(index.settings().materials.size()==8192);
    for(size_t i:{size_t(0),size_t(4096),size_t(8191)}) {
        auto assignment=AssignmentOf(i+1);auto found=index.Find(assignment,std::string(64,'A'));assert(found&&!found->enabled);
        assert(!index.Find(assignment,std::string(64,'b')));
    }
    assert(!index.Find(AssignmentOf(8193),std::string(64,'a')));
    s.materials[8191].parameters.roughness_factor=4;
    Near(index.Find(AssignmentOf(8192),std::string(64,'a'))->parameters.roughness_factor,1);
}
void ResolutionPolicy() {
    Fixture f;auto r=f.Get();assert(r.active()&&r.material==&f.library.materials[0]);
    f.settings.enabled=false;f.library.materials.clear();assert(f.Get().reason==Reason::Disabled); // Disabled never asks Select to read malformed library.
    f=Fixture{};f.settings.pack_path.clear();assert(f.Get().reason==Reason::InvalidSettings);
    f=Fixture{};f.library.status=LoadStatus::Rejected;assert(f.Get().reason==Reason::LibraryUnavailable);
    f=Fixture{};f.library.manifest_sha256=std::string(64,'b');assert(f.Get().reason==Reason::UnapprovedManifest);
    f=Fixture{};f.library.materials[0].albedo.levels.clear();assert(f.Get().reason==Reason::LibraryUnavailable);
    f=Fixture{};f.library.materials.push_back(f.library.materials[0]);assert(f.Get().reason==Reason::NoAssignment);
    f=Fixture{};f.draw.assignment.vertex_shader=4;assert(f.Get().reason==Reason::NoAssignment);
    f=Fixture{};f.draw.blended=true;assert(f.Get().reason==Reason::UnsupportedDraw);
    f=Fixture{};f.draw.tangent_basis=false;assert(f.Get().reason==Reason::MissingGeometry);
    f=Fixture{};f.draw.linear_lighting=false;assert(f.Get().reason==Reason::MissingLinearLighting);
    f=Fixture{};f.draw.current_alpha_sha256=std::string(64,'b');assert(f.Get().reason==Reason::AlphaMismatch);
    for(int field=0;field<6;++field) {
        f=Fixture{};
        switch(field){case 0:f.bindings.pipeline=false;break;case 1:f.bindings.geometry=false;break;case 2:f.bindings.view=false;break;
        case 3:f.bindings.lighting=false;break;case 4:f.bindings.linear_output=false;break;case 5:f.bindings.material_bindings=false;break;}
        assert(f.Get().reason==Reason::RendererBindingsUnavailable&&!f.Get().active());
    }
    f=Fixture{};f.settings.parameters.roughness_factor=2;
    MaterialOverride override;override.key=ExactKey(f.draw.assignment,f.library.manifest_sha256);override.enabled=false;
    f.settings.materials.push_back(override);assert(f.Get().reason==Reason::MaterialDisabled);
    f.settings.materials[0].enabled=true;Near(f.Get().parameters.roughness_factor,2);
    f.settings.materials[0].use_global_parameters=false;f.settings.materials[0].parameters.roughness_factor=3;
    Near(f.Get().parameters.roughness_factor,3);
    PreparedSettings prepared(f.settings);f.settings.materials[0].parameters.roughness_factor=4;
    auto indexed=Resolve(prepared,f.library,f.draw,f.bindings);assert(indexed.active());Near(indexed.parameters.roughness_factor,3);
    f.settings.materials[0].key.approved_manifest_sha256=std::string(64,'b');Near(f.Get().parameters.roughness_factor,2);
    // Same texture with a different exact shader assignment does not inherit another material's override.
    auto second=f.library.materials[0];second.assignment.vertex_shader=20;f.library.materials.push_back(second);
    f.draw.assignment=second.assignment;Near(f.Get().parameters.roughness_factor,2);
}
void HeightPolicy() {
    Fixture f;f.settings.parameters.height_mode=HeightMode::Parallax;
    assert(f.Get().reason==Reason::MissingHeightMap);
    auto& m=f.library.materials[0];m.has_height=true;m.height.storage=Storage::R16Unorm;m.height.levels.push_back({1,1,{0,128}});
    assert(f.Get().reason==Reason::ParallaxUnavailable);f.bindings.parallax=true;assert(f.Get().reason==Reason::ParallaxUnavailable);
    f.bindings.height_units=true;assert(f.Get().active()&&f.Get().parameters.height_mode==HeightMode::Parallax);
    f.settings.parameters.height_mode=HeightMode::Displacement;assert(f.Get().reason==Reason::DisplacementUnavailable);
    f.bindings.displacement=true;assert(f.Get().reason==Reason::DisplacementUnavailable);
    f.bindings.tessellation=true;assert(f.Get().active()&&f.Get().parameters.height_mode==HeightMode::Displacement);
    f.bindings.height_units=false;assert(!f.Get().active());
    f.settings.parameters.height_mode=HeightMode::None;assert(f.Get().active());
}
void CatalogConcurrency() {
    auto initial=GetCatalog();assert(initial&&initial->status==RuntimeStatus::Disabled&&initial->ready_materials==0);
    Fixture f;CatalogSnapshot snapshot;snapshot.manifest_sha256=f.library.manifest_sha256;snapshot.status=RuntimeStatus::Ready;
    CatalogMaterial material;material.key=ExactKey(f.draw.assignment,snapshot.manifest_sha256);material.reason=Reason::Ready;
    material.ready=true;snapshot.materials={material};PublishCatalog(snapshot);
    auto no_draw=GetCatalog();assert(no_draw->ready_materials==0&&no_draw->status==RuntimeStatus::BindingsUnavailable);
    snapshot.capabilities=f.bindings;snapshot.materials[0].observed_draws=3;snapshot.materials[0].rendered_draws=2;
    PublishCatalog(snapshot);auto stable=GetCatalog();assert(stable->ready_materials==1&&stable->materials[0].ready);
    snapshot.materials[0].key.assignment.original_texture=900;assert(stable->materials[0].key.assignment.original_texture==1);
    snapshot.status=RuntimeStatus::Disabled;PublishCatalog(snapshot);
    assert(GetCatalog()->ready_materials==0&&!GetCatalog()->materials[0].ready);
    assert(GetCatalog()->materials[0].reason==Reason::Disabled&&stable->ready_materials==1);
    std::atomic<bool> done=false;std::thread writer([&]{for(uint64_t i=1;i<=200;++i){snapshot.revision=i;PublishCatalog(snapshot);}done=true;});
    uint64_t last=0;while(!done){auto view=GetCatalog();assert(view&&view->revision>=last);last=view->revision;assert(view->ready_materials==0);}
    writer.join();assert(GetCatalog()->revision==200&&stable->materials[0].key.assignment.original_texture==1);
    CatalogSnapshot large;large.manifest_sha256=f.library.manifest_sha256;large.status=RuntimeStatus::Loading;
    for(size_t i=0;i<kMaxMaterials+5;++i){auto row=material;row.key.assignment.original_texture=i+1;large.materials.push_back(row);}
    PublishCatalog(std::move(large));assert(GetCatalog()->materials.size()==8192&&GetCatalog()->ready_materials==0);
}
}
int main() {
    SettingsPolicy();ResolutionPolicy();HeightPolicy();CatalogConcurrency();
    std::cout<<"PBR controls: finite bounds, exact manifest/assignment isolation, true height capability gates and immutable concurrent catalog passed\n";
}
