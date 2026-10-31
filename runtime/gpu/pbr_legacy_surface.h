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
// The facades' lit night windows, exactly: stage 0 tmp = texture*diffuse with alpha the texture's (the window mask),
// stage 1 cur = texture*TFACTOR, stage 2 MODULATEALPHA_ADDCOLOR(tmp, cur) = tmp.rgb + cur.rgb*tmp.a with alpha tmp.a,
// then the cascade ends. The dedicated path draws stage 0 as the surface and adds stage 1 as its window layer.
inline bool WindowLayerSurface(const State& state,const PixelKey& pixel,uint32_t stage) {
    constexpr uint32_t disable=1,select_arg1=2,modulate=4,modulate_alpha_add_color=18;
    constexpr uint32_t diffuse=0,current=1,texture=2,tfactor=3,temp=5;
    if(stage||pixel.programmable||pixel.flat||pixel.two_sided||pixel.quad||pixel.car_paint||pixel.pbr||
       pixel.proj||pixel.alphakill)return false;
    for(unsigned i=0;i<2;++i)
        if(pixel.tex[i].kind!=kTex2D||pixel.tex[i].rect||pixel.tex[i].shadow||pixel.tex[i].flags)return false;
    for(unsigned i=2;i<4;++i)if(pixel.tex[i].kind!=kTexNone)return false;
    const auto pair=[](uint32_t a,uint32_t b,uint32_t x,uint32_t y){return (a==x&&b==y)||(a==y&&b==x);};
    const auto& s0=pixel.ff[0];const auto& s1=pixel.ff[1];const auto& s2=pixel.ff[2];
    if(!(s0[0]==modulate&&pair(s0[1],s0[2],texture,diffuse)&&s0[4]==select_arg1&&s0[5]==texture&&s0[8]==temp))return false;
    if(!(s1[0]==modulate&&pair(s1[1],s1[2],texture,tfactor)&&s1[4]==select_arg1&&s1[5]==texture&&s1[8]==current))return false;
    if(!(s2[0]==modulate_alpha_add_color&&s2[1]==temp&&s2[2]==current&&s2[4]==select_arg1&&s2[5]==temp&&s2[8]==current))
        return false;
    if(pixel.ff[3][0]>disable)return false;
    for(const auto& s:state.tss)if(s[kTssColorSign]||s[kTssColorKeyOp]||s[kTssAlphaKill])return false;
    return true;
}
} // namespace gpu::pbr
