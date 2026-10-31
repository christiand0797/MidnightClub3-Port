#include "gpu/pbr_scene.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>

namespace gpu::pbr::scene {
namespace {
constexpr float kPi=3.14159265358979323846f;
constexpr size_t kView=2,kProjection=3;
bool Finite(float x){return std::isfinite(x);}
bool Finite(Vec3 v){return Finite(v.x)&&Finite(v.y)&&Finite(v.z);}
bool Nonnegative(Vec3 v){return Finite(v)&&v.x>=0&&v.y>=0&&v.z>=0;}
bool Nonzero(Vec3 v){return v.x!=0||v.y!=0||v.z!=0;}
bool Same(Vec3 a,Vec3 b){return a.x==b.x&&a.y==b.y&&a.z==b.z;}
float LightFloat(const std::array<uint32_t,26>& l,size_t n){return std::bit_cast<float>(l[n]);}
Vec3 LightRgb(const std::array<uint32_t,26>& l,size_t n){return {LightFloat(l,n),LightFloat(l,n+1),LightFloat(l,n+2)};}
Vec3 Transform(const std::array<float,16>& m,Vec3 v,bool position) {
    return {v.x*m[0]+v.y*m[4]+v.z*m[8]+(position?m[12]:0),
            v.x*m[1]+v.y*m[5]+v.z*m[9]+(position?m[13]:0),
            v.x*m[2]+v.y*m[6]+v.z*m[10]+(position?m[14]:0)};
}
bool RigidView(const std::array<float,16>& m) {
    for(float x:m)if(!Finite(x))return false;
    if(m[3]!=0||m[7]!=0||m[11]!=0||m[15]!=1)return false;
    for(size_t r=0;r<3;++r)for(size_t s=0;s<3;++s) {
        double dot=0;for(size_t k=0;k<3;++k)dot+=double(m[r*4+k])*m[s*4+k];
        if(std::abs(dot-(r==s?1.0:0.0))>1e-4)return false;
    }
    double det=double(m[0])*(double(m[5])*m[10]-double(m[6])*m[9])-
        double(m[1])*(double(m[4])*m[10]-double(m[6])*m[8])+double(m[2])*(double(m[4])*m[9]-double(m[5])*m[8]);
    return det>0&&std::abs(det-1)<2e-4;
}
bool Perspective(const std::array<float,16>& p) {
    for(float x:p)if(!Finite(x))return false;
    // A bounded ordinary off-axis perspective subset, with camera at the established view origin.
    // Orthographic/oblique projections are not replaced by a guessed camera direction.
    const double xy=double(p[0])*p[5]-double(p[1])*p[4];
    const double length=std::sqrt((double(p[0])*p[0]+double(p[1])*p[1])*(double(p[4])*p[4]+double(p[5])*p[5]));
    return p[3]==0&&p[7]==0&&p[15]==0&&p[12]==0&&p[13]==0&&p[11]!=0&&p[14]!=0&&
        p[0]!=0&&p[5]!=0&&length>0&&std::abs(xy)>1e-12*length;
}
// The legacy alphatest(): Cmp maps 0x200..0x207 to NEVER..ALWAYS and anything else to ALWAYS, which never discards.
bool AlphaTestCanReject(const State& s) {
    const uint32_t f=s.simple[kMAlphaFunc>>2];
    return s.simple[kMAlphaTestEnable>>2]&&f>=0x200&&f<0x207;
}
bool MaterialWhite(const State& s) {
    for(float x:s.material)if(!Finite(x))return false;
    for(size_t i=0;i<3;++i)if(s.material[i]!=1||s.material[4+i]!=1||s.material[12+i]!=0)return false;
    if(s.material[3]!=1||s.material[16]<0)return false;
    if(s.rs[kRsSpecularEnable])for(size_t i=0;i<3;++i)if(s.material[8+i]!=1)return false;
    return true;
}
bool BakedCombiner(const State& s,bool alpha_one,bool factor_applied,bool alpha_packed) {
    constexpr uint32_t disable=1,select_arg1=2,select_arg2=3,modulate=4,texture=2,diffuse=0,current=1,tfactor=3;
    if(s.ps||!s.textures[0])return false;
    // As MakePixelKey: DISABLE, or a stage selecting a missing texture, ends the cascade.
    const auto ended=[&](size_t i){return s.tss[i][kTssColorOp]<=disable||(!s.textures[i]&&(s.tss[i][kTssColorArg1]&0xF)==texture);};
    if(!ended(1)) {
        // Only a texture-less CURRENT*TFACTOR tint whose factor the backend folded into the diffuse. Its alpha may
        // keep CURRENT, or multiply by a TFACTOR alpha of exactly one.
        const auto& f=s.tss[1];
        const auto by_factor=[](uint32_t a,uint32_t b){return (a==current&&b==tfactor)||(a==tfactor&&b==current);};
        const bool keep_alpha=(f[kTssAlphaOp]==select_arg1&&f[kTssAlphaArg1]==current)||
                              (f[kTssAlphaOp]==select_arg2&&f[kTssAlphaArg2]==current)||
                              (f[kTssAlphaOp]==modulate&&by_factor(f[kTssAlphaArg1],f[kTssAlphaArg2])&&
                               (alpha_packed||(s.rs[kRsTextureFactor]>>24)==255));
        if(!factor_applied||s.textures[1]||f[kTssColorOp]!=modulate||!by_factor(f[kTssColorArg1],f[kTssColorArg2])||
           f[kTssResultArg]!=current||!keep_alpha||!ended(2))return false;
    }
    const auto& t=s.tss[0];
    // At stage 0 CURRENT is the diffuse.
    const auto product=[](uint32_t a,uint32_t b){return (a==texture&&(b==diffuse||b==current))||((a==diffuse||a==current)&&b==texture);};
    if(t[kTssResultArg]!=current||t[kTssColorOp]!=modulate||!product(t[kTssColorArg1],t[kTssColorArg2]))return false;
    const bool texture_alpha=t[kTssAlphaOp]==select_arg1&&t[kTssAlphaArg1]==texture;
    const bool product_alpha=t[kTssAlphaOp]==modulate&&product(t[kTssAlphaArg1],t[kTssAlphaArg2])&&(alpha_one||alpha_packed);
    return texture_alpha||product_alpha;
}
// The window-layer cascade (pbr::WindowLayerSurface) on the actual state: stage 0 texture*diffuse to TEMP with the
// texture's alpha, stage 1 texture*TFACTOR, stage 2 MODULATEALPHA_ADDCOLOR(TEMP, CURRENT) with TEMP's alpha, then ended.
bool WindowLayerCombiner(const State& s) {
    constexpr uint32_t disable=1,select_arg1=2,modulate=4,add=18,diffuse=0,current=1,texture=2,tfactor=3,temp=5;
    if(s.ps||!s.textures[0]||!s.textures[1]||s.textures[2])return false;
    const auto pair=[](uint32_t a,uint32_t b,uint32_t x,uint32_t y){return (a==x&&b==y)||(a==y&&b==x);};
    const auto& t0=s.tss[0];const auto& t1=s.tss[1];const auto& t2=s.tss[2];const auto& t3=s.tss[3];
    return t0[kTssColorOp]==modulate&&pair(t0[kTssColorArg1],t0[kTssColorArg2],texture,diffuse)&&
           t0[kTssAlphaOp]==select_arg1&&t0[kTssAlphaArg1]==texture&&t0[kTssResultArg]==temp&&
           t1[kTssColorOp]==modulate&&pair(t1[kTssColorArg1],t1[kTssColorArg2],texture,tfactor)&&
           t1[kTssAlphaOp]==select_arg1&&t1[kTssAlphaArg1]==texture&&t1[kTssResultArg]==current&&
           t2[kTssColorOp]==add&&t2[kTssColorArg1]==temp&&t2[kTssColorArg2]==current&&
           t2[kTssAlphaOp]==select_arg1&&t2[kTssAlphaArg1]==temp&&t2[kTssResultArg]==current&&
           (t3[kTssColorOp]<=disable||(!s.textures[3]&&(t3[kTssColorArg1]&0xF)==texture));
}
// The fixed-function (no vertex program) half of FillFogConstants, packed after the scene is built.
Status PackFog(const State& s,pipeline::SceneConstants& result) {
    if(!s.rs[kRsFogEnable])return Status::Ready;
    const uint32_t c=s.rs[kRsFogColor];
    const std::array<float,3> color{float((c>>16)&255)/255.0f,float((c>>8)&255)/255.0f,float(c&255)/255.0f};
    if(pipeline::SetFog(s.rs[kRsFogTableMode],std::bit_cast<float>(s.rs[kRsFogStart]),std::bit_cast<float>(s.rs[kRsFogEnd]),
                        std::bit_cast<float>(s.rs[kRsFogDensity]),color,result)!=pipeline::Status::Ready) {
        result={};return Status::InvalidFog;
    }
    return Status::Ready;
}
} // namespace
const char* StatusText(Status s) {
    switch(s) {
    case Status::Ready:return "original-state scene ready under explicit normalized-linear policy";
    case Status::MissingProof:return "missing post-success/index/state/draw/view/camera/coverage proof";
    case Status::MissingColorPolicy:return "API color interpretation has not been explicitly approved";
    case Status::MissingOutputPolicy:return "actual output encoding policy has not been proven";
    case Status::UnsupportedDraw:return "only opaque lit fixed-function non-fogged draws supported";
    case Status::InvalidView:return "VIEW must be finite, affine, proper rigid";
    case Status::InvalidProjection:return "perspective camera-at-view-origin projection required";
    case Status::UnsupportedMaterialSource:return "vertex material color sources are not in the dedicated vertex layout";
    case Status::UnsupportedMaterial:return "legacy material tint/emission/coverage cannot be represented by this scene subset";
    case Status::InvalidLight:return "unknown or invalid actual typed light";
    case Status::UnsupportedLightAmbient:return "positionally attenuated light ambient is not a constant irradiance";
    case Status::UnsupportedLightSpecular:return "different diffuse/specular incident colors need a wider light contract";
    case Status::MissingLighting:return "no nonzero actual light or ambient input";
    case Status::InvalidConstants:return "scene builder rejected constants";
    case Status::UnsupportedBakedCombiner:return "baked coefficients require exact one-stage albedo*diffuse and preserved alpha";
    case Status::InvalidFog:return "fog table mode/range/colour cannot be reproduced";
    }
    return "unknown scene state";
}
Status Build(const State& s,const Proof& proof,pipeline::OutputPolicy output,pipeline::SceneConstants& result) {
    result={};
    if(!proof.light_writes_after_success||!proof.light_index_coverage||!proof.stateblock_valid||
       !proof.fixed_function_lighting_proven||!proof.rigid_view_proven||!proof.perspective_camera_proven||
       !proof.opaque_no_cutout_proven)return Status::MissingProof;
    if(!proof.api_colors_linear)return Status::MissingColorPolicy;
    if(!proof.output_policy_proven)return Status::MissingOutputPolicy;
    if(!Finite(proof.exposure)||proof.exposure<0||proof.exposure>64||static_cast<uint32_t>(output)>1)return Status::InvalidConstants;
    if(!s.rs[kRsLighting]||(s.rs[kRsFogEnable]&&!proof.fog_reproduced)||(s.rs[kRsVertexBlend]&7)||s.simple[kMBlendEnable>>2]||
       AlphaTestCanReject(s)||(s.vs&&!s.vs->program.empty()))return Status::UnsupportedDraw;
    for(const auto& stage:s.tss)if(stage[kTssAlphaKill])return Status::UnsupportedDraw;
    if(!RigidView(s.transforms[kView]))return Status::InvalidView;
    if(!Perspective(s.transforms[kProjection]))return Status::InvalidProjection;
    if(s.rs[kRsColorVertex])for(auto key:{kRsDiffuseMaterialSource,kRsAmbientMaterialSource,kRsSpecularMaterialSource,kRsEmissiveMaterialSource})
        if(s.rs[key]!=0)return Status::UnsupportedMaterialSource;
    if(!MaterialWhite(s))return Status::UnsupportedMaterial;
    if(s.light_enable&~0xffu)return Status::InvalidLight;
    const uint32_t ambient=s.rs[kRsAmbient];
    Vec3 uniform{float((ambient>>16)&255)/255.0f,float((ambient>>8)&255)/255.0f,float(ambient&255)/255.0f};
    std::array<pipeline::Light,pipeline::kLightCount> lights{};size_t count=0;
    bool incident=Nonzero(uniform);
    const auto& view=s.transforms[kView];
    for(size_t i=0;i<8;++i) {
        if(!(s.light_enable&(1u<<i)))continue;
        const auto& raw=s.lights[i];const uint32_t type=raw[0];
        if(type<1||type>3)return Status::InvalidLight;
        const Vec3 diffuse=LightRgb(raw,1),specular=LightRgb(raw,5),amb=LightRgb(raw,9);
        if(!Nonnegative(diffuse)||!Nonnegative(amb))return Status::InvalidLight;
        if(s.rs[kRsSpecularEnable]&&(!Nonnegative(specular)||!Same(diffuse,specular)))return Status::UnsupportedLightSpecular;
        pipeline::Light light;light.linear_radiance=diffuse;incident|=Nonzero(diffuse);
        if(type==3) {
            light.type=pipeline::LightType::Directional;
            light.direction=Transform(view,LightRgb(raw,16),false);
            uniform.x+=amb.x;uniform.y+=amb.y;uniform.z+=amb.z;
        }else {
            if(Nonzero(amb))return Status::UnsupportedLightAmbient;
            light.type=type==1?pipeline::LightType::Point:pipeline::LightType::Spot;
            light.position=Transform(view,LightRgb(raw,13),true);light.range=LightFloat(raw,19);
            light.attenuation0=LightFloat(raw,21);light.attenuation1=LightFloat(raw,22);light.attenuation2=LightFloat(raw,23);
            if(type==2) {
                light.direction=Transform(view,LightRgb(raw,16),false);light.falloff=LightFloat(raw,20);
                const float theta=LightFloat(raw,24),phi=LightFloat(raw,25);
                if(!Finite(theta)||!Finite(phi)||theta<0||!(theta<phi)||phi>kPi)return Status::InvalidLight;
                light.cos_inner=std::cos(theta*0.5f);light.cos_outer=std::cos(phi*0.5f);
                if(!(light.cos_inner>light.cos_outer))return Status::InvalidLight;
            }
        }
        if(!Finite(light.position)||!Finite(light.direction))return Status::InvalidLight;
        lights[count++]=light;
    }
    if(!Finite(uniform)||!Nonnegative(uniform))return Status::InvalidLight;
    if(!incident&&!Nonzero(uniform))return Status::MissingLighting;
    // Original ambient is a coefficient. This explicit compatibility normalization is not measured sky irradiance.
    const Vec3 irradiance{uniform.x*kPi,uniform.y*kPi,uniform.z*kPi};
    if(pipeline::BuildScene({lights.data(),count},irradiance,proof.exposure,output,result)!=pipeline::Status::Ready) {
        result={};return Status::InvalidLight;
    }
    return PackFog(s,result);
}
Status BuildBakedVertexDiffuse(const State& s,const BakedProof& proof,pipeline::OutputPolicy output,
                             pipeline::SceneConstants& result) {
    result={};
    if(!proof.immutable_draw_proven||!proof.fixed_world_source_proven||!proof.vertex_diffuse_stream_proven||
       !proof.vertex_diffuse_forwarded_proven||!proof.vertex_specular_zero_proven||!proof.albedo_diffuse_combiner_proven||
       !proof.rigid_view_proven||!proof.perspective_camera_proven||!proof.opaque_no_cutout_proven)return Status::MissingProof;
    if(!proof.coefficient_colors_linear)return Status::MissingColorPolicy;
    if(!proof.output_policy_proven)return Status::MissingOutputPolicy;
    if(!Finite(proof.exposure)||proof.exposure<0||proof.exposure>64||static_cast<uint32_t>(output)>1)return Status::InvalidConstants;
    const uint32_t alpha_func=s.simple[kMAlphaFunc>>2];
    const bool kept_test=proof.coverage_kept&&proof.combiner_alpha_packed&&(alpha_func==0x204||alpha_func==0x206);
    if(s.rs[kRsLighting]||(s.rs[kRsFogEnable]&&!proof.fog_reproduced)||(s.rs[kRsVertexBlend]&7)||
       (s.simple[kMBlendEnable>>2]&&!(proof.coverage_kept&&proof.combiner_alpha_packed))||
       (AlphaTestCanReject(s)&&!kept_test)||s.rs[kRsTwoSidedLighting]||s.simple[kMShadeMode>>2]==0x1D00||
       ((s.vs_handle&1)&&(!s.vs||!s.vs->program.empty()))||
       (!(s.vs_handle&1)&&(s.vs_handle&0xE)!=0x2))return Status::UnsupportedDraw;
    for(const auto& t:s.tss)if(t[kTssColorSign]||t[kTssColorKeyOp]||t[kTssAlphaKill])return Status::UnsupportedDraw;
    if(!RigidView(s.transforms[kView]))return Status::InvalidView;
    if(!Perspective(s.transforms[kProjection]))return Status::InvalidProjection;
    if(!(proof.window_layer_kept?WindowLayerCombiner(s):
         BakedCombiner(s,proof.vertex_diffuse_alpha_one_proven,proof.texture_factor_applied,proof.combiner_alpha_packed)))
        return Status::UnsupportedBakedCombiner;
    // BuildBakedScene packs a mode, not a uniform white light/irradiance approximation. Actual bounded
    // coefficients are supplied per vertex by the independently validated geometry/stream bridge.
    if(pipeline::BuildBakedScene(proof.exposure,output,result)!=pipeline::Status::Ready) {
        result={};return Status::InvalidConstants;
    }
    return PackFog(s,result);
}
} // namespace gpu::pbr::scene
