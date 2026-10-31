#pragma once
#include "gpu/shaders.h"
namespace gpu::pbr {
// Initial supported replacement contract: one ordinary normalized albedo, material-only fixed
// lighting replaced by the dedicated BRDF, and unchanged albedo alpha. A shader key alone cannot
// approve dropping detail/light/shadow/reflection/constant-alpha/temporary-register effects.
// Stage 1 may only multiply by TFACTOR (a uniform tint, no texture): colour CURRENT*TFACTOR, alpha the same or
// CURRENT kept. The backend folds that factor into the packed diffuse; stage 2 must then be disabled.
inline bool TextureFactorStage(const uint32_t* f) {
    constexpr uint32_t select_arg1=2,select_arg2=3,modulate=4,current=1,tfactor=3;
    const auto product=[](uint32_t a,uint32_t b){return (a==current&&b==tfactor)||(a==tfactor&&b==current);};
    return f[0]==modulate&&product(f[1],f[2])&&f[8]==current&&
           ((f[4]==modulate&&product(f[5],f[6]))||(f[4]==select_arg1&&f[5]==current)||(f[4]==select_arg2&&f[6]==current));
}
inline bool SimpleAlbedoSurface(const State& state,const PixelKey& pixel,uint32_t stage) {
    constexpr uint32_t disable=1,select_arg1=2,modulate=4,texture=2,diffuse=0,current=1;
    if(stage||pixel.programmable||pixel.flat||pixel.two_sided||pixel.quad||pixel.car_paint||pixel.pbr||
       pixel.proj||pixel.alphakill||pixel.tex[0].kind!=kTex2D||pixel.tex[0].rect||
       pixel.tex[0].shadow||pixel.tex[0].flags)return false;
    for(unsigned i=1;i<4;++i)if(pixel.tex[i].kind!=kTexNone)return false;
    // MakePixelKey stops recording at the stage that ends the cascade: 0 there means ended, like DISABLE.
    const auto ended=[&](unsigned i){return pixel.ff[i][0]<=disable;};
    if(!ended(1)&&!(TextureFactorStage(pixel.ff[1])&&ended(2)))return false;
    const auto& ff=pixel.ff[0];
    if(ff[8]!=current)return false;
    // At stage 0 CURRENT is the diffuse (the generated shader starts with cur = v0).
    const auto with_diffuse=[](uint32_t a,uint32_t b){return (a==texture&&(b==diffuse||b==current))||((a==diffuse||a==current)&&b==texture);};
    const bool rgb=ff[0]==select_arg1&&ff[1]==texture;
    const bool lit_rgb=ff[0]==modulate&&with_diffuse(ff[1],ff[2]);
    const bool alpha=ff[4]==select_arg1&&ff[5]==texture;
    const bool unit_alpha=ff[4]==modulate&&with_diffuse(ff[5],ff[6]);
    if(!(rgb||lit_rgb)||!(alpha||unit_alpha))return false;
    // Scene builder independently requires material diffuse alpha1 and excludes vertex-color sources.
    for(const auto& s:state.tss)if(s[kTssColorSign]||s[kTssColorKeyOp]||s[kTssAlphaKill])return false;
    return true;
}
} // namespace gpu::pbr
