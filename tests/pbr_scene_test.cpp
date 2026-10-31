#include "gpu/pbr_scene.h"

#include <bit>
#include <cassert>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>

using namespace gpu;
using namespace gpu::pbr;
using namespace gpu::pbr::scene;
namespace {
constexpr float pi=3.14159265358979323846f;
constexpr float nan=std::numeric_limits<float>::quiet_NaN();
void Near(float a,float b){assert(std::isfinite(a)&&std::abs(a-b)<0.0001f);}
void F(State& s,size_t field,float value,size_t slot=0){s.lights[slot][field]=std::bit_cast<uint32_t>(value);}
void Rgb(State& s,size_t field,Vec3 value,size_t slot=0){F(s,field,value.x,slot);F(s,field+1,value.y,slot);F(s,field+2,value.z,slot);}
struct Fixture {
    State state;
    Proof proof;
    pipeline::SceneConstants scene;
    Fixture() {
        proof.light_writes_after_success=proof.light_index_coverage=proof.stateblock_valid=true;
        proof.fixed_function_lighting_proven=proof.rigid_view_proven=proof.perspective_camera_proven=true;
        proof.opaque_no_cutout_proven=proof.api_colors_linear=proof.output_policy_proven=true;
        state.transforms[2]=geometry::RowMatrix4::Identity().values;
        state.transforms[3]={1,0,0,0,0,1,0,0,0,0,1,1,0,0,-0.1f,0};
        for(size_t i=0;i<12;++i)state.material[i]=1;state.material[16]=16;
        state.rs[kRsLighting]=1;state.light_enable=1;state.lights[0][0]=3;
        Rgb(state,1,{2,1,0.5f});Rgb(state,5,{2,1,0.5f});Rgb(state,16,{0,0,-1});
    }
    Status Build(pipeline::OutputPolicy output=pipeline::OutputPolicy::ToneMappedSrgb) {
        scene.irradiance_exposure[0]=100;return scene::Build(state,proof,output,scene);
    }
    void Cleared() {pipeline::SceneConstants zero{};assert(std::memcmp(&scene,&zero,sizeof(zero))==0);}
    void Expect(Status expected){assert(Build()==expected);if(expected!=Status::Ready)Cleared();}
};
void ProofAndOutput() {
    Fixture f;f.Expect(Status::Ready);assert(f.scene.info[0]==1&&f.scene.info[1]==1);
    Near(f.scene.lights[0].radiance[0],2);Near(f.scene.lights[0].radiance[1],1);Near(f.scene.lights[0].radiance[2],0.5f);
    for(int field=0;field<9;++field) {
        f=Fixture{};
        switch(field){case 0:f.proof.light_writes_after_success=false;break;case 1:f.proof.light_index_coverage=false;break;
        case 2:f.proof.stateblock_valid=false;break;case 3:f.proof.fixed_function_lighting_proven=false;break;
        case 4:f.proof.rigid_view_proven=false;break;case 5:f.proof.perspective_camera_proven=false;break;
        case 6:f.proof.opaque_no_cutout_proven=false;break;case 7:f.proof.api_colors_linear=false;break;case 8:f.proof.output_policy_proven=false;break;}
        f.Expect(field<7?Status::MissingProof:field==7?Status::MissingColorPolicy:Status::MissingOutputPolicy);
    }
    f=Fixture{};f.proof={};f.Expect(Status::MissingProof);
    f=Fixture{};f.proof.exposure=nan;f.Expect(Status::InvalidConstants);
    f=Fixture{};f.proof.exposure=65;f.Expect(Status::InvalidConstants);
    f=Fixture{};assert(f.Build(static_cast<pipeline::OutputPolicy>(2))==Status::InvalidConstants);f.Cleared();
    f=Fixture{};assert(f.Build(pipeline::OutputPolicy::LinearHdr)==Status::Ready);assert(f.scene.info[1]==0);
    // HDR target truth is external proof, not established by this CPU builder.
}
void DrawAndMaterialGuards() {
    Fixture f;f.state.rs[kRsLighting]=0;f.Expect(Status::UnsupportedDraw);
    f=Fixture{};f.state.rs[kRsFogEnable]=1;f.Expect(Status::UnsupportedDraw);
    f=Fixture{};f.state.rs[kRsVertexBlend]=1;f.Expect(Status::UnsupportedDraw);
    f=Fixture{};f.state.simple[kMBlendEnable>>2]=1;f.Expect(Status::UnsupportedDraw);
    f=Fixture{};f.state.simple[kMAlphaTestEnable>>2]=1;f.state.simple[kMAlphaFunc>>2]=0x204;f.Expect(Status::UnsupportedDraw);
    // An enabled ALWAYS test (or a value the legacy Cmp maps to ALWAYS) never discards.
    f.state.simple[kMAlphaFunc>>2]=0x207;f.Expect(Status::Ready);f.state.simple[kMAlphaFunc>>2]=0;f.Expect(Status::Ready);
    f.state.simple[kMAlphaFunc>>2]=0x200;f.Expect(Status::UnsupportedDraw);
    f=Fixture{};f.state.tss[2][kTssAlphaKill]=1;f.Expect(Status::UnsupportedDraw);
    f=Fixture{};f.state.vs=std::make_shared<VertexShader>();f.state.vs->program={1,2,3,4};f.Expect(Status::UnsupportedDraw);
    f=Fixture{};f.state.rs[kRsDiffuseMaterialSource]=1;f.Expect(Status::Ready); // COLORVERTEX off ignores those source selectors.
    for(auto field:{kRsDiffuseMaterialSource,kRsAmbientMaterialSource,kRsSpecularMaterialSource,kRsEmissiveMaterialSource}) {
        f=Fixture{};f.state.rs[kRsColorVertex]=1;f.state.rs[field]=1;f.Expect(Status::UnsupportedMaterialSource);
    }
    f=Fixture{};f.state.rs[kRsColorVertex]=1;f.Expect(Status::Ready);
    for(size_t field:{size_t(0),size_t(4),size_t(3),size_t(12)}) {
        f=Fixture{};f.state.material[field]=0.3f;f.Expect(Status::UnsupportedMaterial);
    }
    f=Fixture{};f.state.rs[kRsSpecularEnable]=1;f.state.material[8]=0.5f;f.Expect(Status::UnsupportedMaterial);
    f=Fixture{};f.state.material[16]=nan;f.Expect(Status::UnsupportedMaterial);
}
void ViewAndCamera() {
    Fixture f;f.state.transforms[2][0]=2;f.Expect(Status::InvalidView);
    f=Fixture{};f.state.transforms[2][4]=0.25f;f.Expect(Status::InvalidView);
    f=Fixture{};f.state.transforms[2][0]=-1;f.Expect(Status::InvalidView);
    f=Fixture{};f.state.transforms[2][12]=nan;f.Expect(Status::InvalidView);
    f=Fixture{};f.state.transforms[2][3]=0.01f;f.Expect(Status::InvalidView);
    f=Fixture{};f.state.transforms[3]=geometry::RowMatrix4::Identity().values;f.Expect(Status::InvalidProjection);
    f=Fixture{};f.state.transforms[3][14]=0;f.Expect(Status::InvalidProjection);
    f=Fixture{};f.state.transforms[3][1]=1;f.state.transforms[3][4]=1;f.Expect(Status::InvalidProjection);
    f=Fixture{};f.state.transforms[3][0]=nan;f.Expect(Status::InvalidProjection);
    f=Fixture{};f.state.transforms[2]={0,1,0,0,-1,0,0,0,0,0,1,0,3,4,5,1};
    Rgb(f.state,16,{1,0,0});f.Expect(Status::Ready);
    Near(f.scene.lights[0].direction_type[0],0);Near(f.scene.lights[0].direction_type[1],1);Near(f.scene.lights[0].direction_type[2],0);
    assert(f.scene.lights[0].direction_type[3]==float(pipeline::LightType::Directional));
    f.state.lights[0][0]=1;Rgb(f.state,13,{2,0,0});F(f.state,19,100);F(f.state,21,1);F(f.state,22,0.5f);F(f.state,23,0.1f);
    f.Expect(Status::Ready);Near(f.scene.lights[0].position_range[0],3);Near(f.scene.lights[0].position_range[1],6);
    Near(f.scene.lights[0].position_range[2],5);Near(f.scene.lights[0].position_range[3],100);
    assert(f.scene.lights[0].direction_type[3]==float(pipeline::LightType::Point));
    Near(f.scene.lights[0].attenuation_falloff[1],0.5f);Near(f.scene.lights[0].attenuation_falloff[2],0.1f);
    f.state.lights[0][0]=2;F(f.state,20,2);F(f.state,24,0.5f);F(f.state,25,1);f.Expect(Status::Ready);
    assert(f.scene.lights[0].direction_type[3]==float(pipeline::LightType::Spot));
    Near(f.scene.lights[0].cone[0],std::cos(0.25f));Near(f.scene.lights[0].cone[1],std::cos(0.5f));
}
void ActualLightRules() {
    Fixture f;f.state.light_enable=1u<<8;f.Expect(Status::InvalidLight);
    f=Fixture{};f.state.lights[0][0]=5;f.Expect(Status::InvalidLight); // Do not mask unknown types into known ones.
    f=Fixture{};Rgb(f.state,1,{nan,0,0});f.Expect(Status::InvalidLight);
    f=Fixture{};Rgb(f.state,16,{0,0,0});f.Expect(Status::InvalidLight);
    f=Fixture{};f.state.rs[kRsSpecularEnable]=1;Rgb(f.state,5,{3,1,0.5f});f.Expect(Status::UnsupportedLightSpecular);
    f=Fixture{};f.state.rs[kRsAmbient]=0xFF804020;Rgb(f.state,9,{0.1f,0.2f,0.3f});f.Expect(Status::Ready);
    Near(f.scene.irradiance_exposure[0],(128/255.0f+0.1f)*pi);
    Near(f.scene.irradiance_exposure[1],(64/255.0f+0.2f)*pi);
    Near(f.scene.irradiance_exposure[2],(32/255.0f+0.3f)*pi);
    f=Fixture{};f.state.light_enable=0;f.Expect(Status::MissingLighting);
    f.state.rs[kRsAmbient]=0xFF010000;f.Expect(Status::Ready);assert(f.scene.info[0]==0);Near(f.scene.irradiance_exposure[0],pi/255);
    f=Fixture{};f.state.lights[0][0]=1;F(f.state,19,10);F(f.state,21,1);Rgb(f.state,9,{0.1f,0,0});f.Expect(Status::UnsupportedLightAmbient);
    Rgb(f.state,9,{0,0,0});F(f.state,19,0);f.Expect(Status::InvalidLight);
    F(f.state,19,10);F(f.state,21,-1);f.Expect(Status::InvalidLight);
    F(f.state,21,0);f.Expect(Status::InvalidLight);
    F(f.state,21,1);f.state.lights[0][0]=2;F(f.state,20,nan);F(f.state,24,0.5f);F(f.state,25,1);f.Expect(Status::InvalidLight);
    F(f.state,20,1);F(f.state,24,1);F(f.state,25,0.5f);f.Expect(Status::InvalidLight);
    F(f.state,24,0.5f);F(f.state,25,0.5f);f.Expect(Status::InvalidLight);
    F(f.state,25,pi+0.1f);f.Expect(Status::InvalidLight);
    // Disabled stale records, including NaNs, are not invented active illumination.
    f=Fixture{};f.state.lights[7][0]=99;F(f.state,1,nan,7);f.Expect(Status::Ready);
}
struct BakedFixture {
    State state;
    BakedProof proof;
    pipeline::SceneConstants scene;
    BakedFixture() {
        proof.immutable_draw_proven=proof.fixed_world_source_proven=proof.vertex_diffuse_stream_proven=true;
        proof.vertex_diffuse_forwarded_proven=proof.vertex_specular_zero_proven=proof.albedo_diffuse_combiner_proven=true;
        proof.rigid_view_proven=proof.perspective_camera_proven=proof.opaque_no_cutout_proven=true;
        proof.coefficient_colors_linear=proof.output_policy_proven=true;
        state.vs_handle=0x142; // actual supported XYZ+DIFFUSE+TEX1 shape, not a model identity.
        state.transforms[2]=geometry::RowMatrix4::Identity().values;
        state.transforms[3]={1,0,0,0,0,1,0,0,0,0,1,1,0,0,-0.1f,0};
        state.textures[0]=0x1234;
        auto& t=state.tss[0];t[kTssColorOp]=4;t[kTssColorArg1]=2;t[kTssColorArg2]=0;
        t[kTssAlphaOp]=2;t[kTssAlphaArg1]=2;t[kTssResultArg]=1;
        state.tss[1][kTssColorOp]=1;
    }
    Status Build(pipeline::OutputPolicy output=pipeline::OutputPolicy::ToneMappedSrgb) {
        scene.lights[0].radiance={1,2,3,4};scene.info={8,7,6,5};
        return BuildBakedVertexDiffuse(state,proof,output,scene);
    }
    void Expect(Status expected) {
        assert(Build()==expected);
        if(expected!=Status::Ready){pipeline::SceneConstants zero{};assert(std::memcmp(&scene,&zero,sizeof(zero))==0);}
    }
};
void BakedCoefficientProof() {
    BakedFixture f;f.Expect(Status::Ready);
    assert(f.scene.info[0]==0&&f.scene.info[1]==1&&f.scene.info[2]==1&&f.scene.info[3]==0);
    Near(f.scene.irradiance_exposure[0],0);Near(f.scene.irradiance_exposure[1],0);Near(f.scene.irradiance_exposure[2],0);
    for(const auto& l:f.scene.lights){pipeline::LightConstants zero{};assert(std::memcmp(&l,&zero,sizeof(zero))==0);}
    // This mode consumes no API-light shadow, legacy material, global ambient or material source.
    f.state.light_enable=0xffffffff;f.state.lights[0][0]=99;F(f.state,1,nan);
    f.state.material.fill(nan);f.state.rs[kRsAmbient]=0xffffffff;f.state.rs[kRsColorVertex]=1;
    f.state.rs[kRsDiffuseMaterialSource]=2;f.state.rs[kRsEmissiveMaterialSource]=2;f.Expect(Status::Ready);
    const std::array members{&BakedProof::immutable_draw_proven,&BakedProof::fixed_world_source_proven,
        &BakedProof::vertex_diffuse_stream_proven,&BakedProof::vertex_diffuse_forwarded_proven,
        &BakedProof::vertex_specular_zero_proven,&BakedProof::albedo_diffuse_combiner_proven,
        &BakedProof::rigid_view_proven,&BakedProof::perspective_camera_proven,&BakedProof::opaque_no_cutout_proven};
    for(auto member:members){f=BakedFixture{};f.proof.*member=false;f.Expect(Status::MissingProof);}
    f=BakedFixture{};f.proof={};f.Expect(Status::MissingProof);
    f=BakedFixture{};f.proof.coefficient_colors_linear=false;f.Expect(Status::MissingColorPolicy);
    f=BakedFixture{};f.proof.output_policy_proven=false;f.Expect(Status::MissingOutputPolicy);
    f=BakedFixture{};f.proof.exposure=nan;f.Expect(Status::InvalidConstants);
    f=BakedFixture{};f.proof.exposure=-1;f.Expect(Status::InvalidConstants);
    f=BakedFixture{};f.proof.exposure=65;f.Expect(Status::InvalidConstants);
    f=BakedFixture{};assert(f.Build(static_cast<pipeline::OutputPolicy>(2))==Status::InvalidConstants);
    f=BakedFixture{};f.proof.exposure=2;assert(f.Build(pipeline::OutputPolicy::LinearHdr)==Status::Ready);
    Near(f.scene.irradiance_exposure[3],2);assert(f.scene.info[1]==0&&f.scene.info[2]==1);
}
void BakedDrawAndCombiner() {
    BakedFixture f;
    for(auto field:{kRsLighting,kRsFogEnable,kRsVertexBlend,kRsTwoSidedLighting}) {
        f=BakedFixture{};f.state.rs[field]=1;f.Expect(Status::UnsupportedDraw);
    }
    for(auto field:{kMBlendEnable,kMAlphaTestEnable}) {
        f=BakedFixture{};f.state.simple[field>>2]=1;f.state.simple[kMAlphaFunc>>2]=0x205;f.Expect(Status::UnsupportedDraw);
    }
    f=BakedFixture{};f.state.simple[kMAlphaTestEnable>>2]=1;f.state.simple[kMAlphaFunc>>2]=0x207;f.Expect(Status::Ready);
    // Kept coverage (cutouts): a GREATER/GEQUAL test and blending need the pipeline's clip/blend and the packed alpha.
    for(uint32_t func:{0x204u,0x206u}) {
        f=BakedFixture{};f.state.simple[kMAlphaTestEnable>>2]=1;f.state.simple[kMAlphaFunc>>2]=func;f.Expect(Status::UnsupportedDraw);
        f.proof.coverage_kept=true;f.Expect(Status::UnsupportedDraw); // the written alpha is not the original's
        f.proof.combiner_alpha_packed=true;f.Expect(Status::Ready);
        f.state.simple[kMBlendEnable>>2]=1;f.Expect(Status::Ready);
    }
    f=BakedFixture{};f.proof.coverage_kept=f.proof.combiner_alpha_packed=true;f.state.simple[kMAlphaTestEnable>>2]=1;
    f.state.simple[kMAlphaFunc>>2]=0x201;f.Expect(Status::UnsupportedDraw); // LESS keeps the transparent part: legacy
    f=BakedFixture{};f.state.simple[kMShadeMode>>2]=0x1D00;f.Expect(Status::UnsupportedDraw);
    f=BakedFixture{};f.state.vs_handle=0x144;f.Expect(Status::UnsupportedDraw); // pretransformed
    f=BakedFixture{};f.state.vs_handle=0x1235;f.Expect(Status::UnsupportedDraw); // missing declaration
    f=BakedFixture{};f.state.vs_handle=0x1235;f.state.vs=std::make_shared<VertexShader>();f.state.vs->program={1};f.Expect(Status::UnsupportedDraw);
    f.state.vs->program.clear();f.Expect(Status::Ready); // declaration-only, actual stream proof still required.
    for(auto field:{kTssColorSign,kTssColorKeyOp,kTssAlphaKill}) {
        f=BakedFixture{};f.state.tss[3][field]=1;f.Expect(Status::UnsupportedDraw);
    }
    f=BakedFixture{};f.state.transforms[2][0]=2;f.Expect(Status::InvalidView);
    f=BakedFixture{};f.state.transforms[3]=geometry::RowMatrix4::Identity().values;f.Expect(Status::InvalidProjection);
    f=BakedFixture{};f.state.textures[0]=0;f.Expect(Status::UnsupportedBakedCombiner);
    f=BakedFixture{};f.state.ps=std::make_shared<PixelShader>();f.Expect(Status::UnsupportedBakedCombiner);
    f=BakedFixture{};f.state.tss[1][kTssColorOp]=4;f.Expect(Status::UnsupportedBakedCombiner);
    f=BakedFixture{};f.state.tss[0][kTssColorOp]=2;f.Expect(Status::UnsupportedBakedCombiner);
    f=BakedFixture{};f.state.tss[0][kTssResultArg]=5;f.Expect(Status::UnsupportedBakedCombiner);
    f=BakedFixture{};f.state.tss[0][kTssColorArg1]=2|0x10;f.Expect(Status::UnsupportedBakedCombiner); // complement
    f=BakedFixture{};f.state.tss[0][kTssColorArg2]=1;f.Expect(Status::Ready); // At stage 0 CURRENT is the diffuse.
    f=BakedFixture{};f.state.tss[0][kTssColorArg2]=3;f.Expect(Status::UnsupportedBakedCombiner); // TFACTOR at stage 0
    f=BakedFixture{};f.state.tss[0][kTssColorArg1]=0;f.state.tss[0][kTssColorArg2]=2;f.Expect(Status::Ready);
    f=BakedFixture{};f.state.tss[0][kTssAlphaOp]=4;f.state.tss[0][kTssAlphaArg1]=2;f.state.tss[0][kTssAlphaArg2]=0;
    f.Expect(Status::UnsupportedBakedCombiner);f.proof.vertex_diffuse_alpha_one_proven=true;f.Expect(Status::Ready);
    f.state.tss[0][kTssAlphaArg1]=0;f.state.tss[0][kTssAlphaArg2]=2;f.Expect(Status::Ready);
    f.state.tss[0][kTssAlphaArg2]=1;f.Expect(Status::UnsupportedBakedCombiner);
    f=BakedFixture{};f.state.tss[0][kTssAlphaOp]=0;f.Expect(Status::UnsupportedBakedCombiner);
}
// Fog is accepted only when the backend states the dedicated shader reproduces it, then packed from the SAME
// State exactly like FillFogConstants' fixed-function branch.
template<class T> void FogFor(T fixture) {
    const auto fog=[](State& s,uint32_t mode,float start,float end,float density,uint32_t color) {
        s.rs[kRsFogEnable]=1;s.rs[kRsFogTableMode]=mode;s.rs[kRsFogStart]=std::bit_cast<uint32_t>(start);
        s.rs[kRsFogEnd]=std::bit_cast<uint32_t>(end);s.rs[kRsFogDensity]=std::bit_cast<uint32_t>(density);s.rs[kRsFogColor]=color;
    };
    T f=fixture;f.Expect(Status::Ready);assert((f.scene.fog==std::array<float,4>{})); // Fog off: nothing packed.
    f=fixture;fog(f.state,3,25,400,0,0xFF4080FF);f.Expect(Status::UnsupportedDraw);
    f.proof.fog_reproduced=true;f.Expect(Status::Ready);
    assert((f.scene.fog==std::array<float,4>{25,400,0,3}));
    Near(f.scene.fog_color[0],float(0x40)/255);Near(f.scene.fog_color[1],float(0x80)/255);Near(f.scene.fog_color[2],1);
    assert(f.scene.fog_color[3]==0);
    f=fixture;f.proof.fog_reproduced=true;fog(f.state,0,25,400,.5f,0xFFFFFFFF);f.Expect(Status::Ready); // Table none: factor 1.
    assert((f.scene.fog==std::array<float,4>{}&&f.scene.fog_color==std::array<float,4>{}));
    f=fixture;f.proof.fog_reproduced=true;fog(f.state,1,0,0,.002f,0);f.Expect(Status::Ready);assert(f.scene.fog[3]==1);
    f=fixture;f.proof.fog_reproduced=true;fog(f.state,3,50,50,0,0);f.Expect(Status::InvalidFog);
    f=fixture;f.proof.fog_reproduced=true;fog(f.state,2,0,0,nan,0);f.Expect(Status::InvalidFog);
    f=fixture;f.proof.fog_reproduced=true;fog(f.state,7,0,1,0,0);f.Expect(Status::InvalidFog);
}
void Fog() {FogFor(Fixture{});FogFor(BakedFixture{});}
// The world's second stage: CURRENT*TFACTOR, folded into the packed diffuse by the backend.
void TextureFactorStage() {
    const auto tint=[](BakedFixture& f) {
        auto& t=f.state.tss[1];t[kTssColorOp]=4;t[kTssColorArg1]=1;t[kTssColorArg2]=3;t[kTssResultArg]=1;
        t[kTssAlphaOp]=4;t[kTssAlphaArg1]=1;t[kTssAlphaArg2]=3;f.state.tss[2][kTssColorOp]=1;
        f.state.rs[kRsTextureFactor]=0xFF808080;
    };
    BakedFixture f;tint(f);f.Expect(Status::UnsupportedBakedCombiner); // Not folded in: no proof.
    f.proof.texture_factor_applied=true;f.Expect(Status::Ready);
    f.state.tss[1][kTssColorArg1]=3;f.state.tss[1][kTssColorArg2]=1;f.Expect(Status::Ready); // Either order.
    f.state.rs[kRsTextureFactor]=0x80808080;f.Expect(Status::UnsupportedBakedCombiner); // Alpha would change.
    f.proof.combiner_alpha_packed=true;f.Expect(Status::Ready); // ... unless that alpha is packed and written.
    f.proof.combiner_alpha_packed=false;
    f.state.tss[1][kTssAlphaOp]=2;f.state.tss[1][kTssAlphaArg1]=1;f.Expect(Status::Ready); // Alpha keeps CURRENT.
    f=BakedFixture{};tint(f);f.proof.texture_factor_applied=true;f.state.tss[2][kTssColorOp]=4;f.Expect(Status::UnsupportedBakedCombiner);
    f.state.tss[2][kTssColorArg1]=2;f.Expect(Status::Ready); // Selects a missing texture: the cascade ends there.
    f.state.textures[2]=0x77;f.Expect(Status::UnsupportedBakedCombiner);
    f=BakedFixture{};tint(f);f.proof.texture_factor_applied=true;f.state.textures[1]=0x99;f.Expect(Status::UnsupportedBakedCombiner);
    f=BakedFixture{};tint(f);f.proof.texture_factor_applied=true;f.state.tss[1][kTssColorArg2]=2;f.Expect(Status::UnsupportedBakedCombiner);
    f=BakedFixture{};tint(f);f.proof.texture_factor_applied=true;f.state.tss[1][kTssColorOp]=7;f.Expect(Status::UnsupportedBakedCombiner);
    f=BakedFixture{};tint(f);f.proof.texture_factor_applied=true;f.state.tss[1][kTssResultArg]=5;f.Expect(Status::UnsupportedBakedCombiner);
}
}
int main() {
    ProofAndOutput();DrawAndMaterialGuards();ViewAndCamera();ActualLightRules();BakedCoefficientProof();BakedDrawAndCombiner();Fog();TextureFactorStage();
    std::cout<<"PBR scene: strict typed/view lights plus explicit baked vertex-diffuse coefficient mode, no invented lights, exact combiner/alpha/proof guards passed\n";
}
