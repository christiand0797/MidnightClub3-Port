#include "gpu/pbr_legacy_surface.h"
#include <cassert>
using namespace gpu;
int main() {
    State s;PixelKey p;p.tex[0].kind=kTex2D;
    p.ff[0][0]=4;p.ff[0][1]=2;p.ff[0][2]=0;p.ff[0][4]=2;p.ff[0][5]=2;p.ff[0][8]=1;p.ff[1][0]=1;
    assert(pbr::SimpleAlbedoSurface(s,p,0));assert(!pbr::SimpleAlbedoSurface(s,p,1));
    auto changed=p;changed.programmable=1;assert(!pbr::SimpleAlbedoSurface(s,changed,0));
    changed=p;changed.tex[1].kind=kTex2D;assert(!pbr::SimpleAlbedoSurface(s,changed,0));
    changed=p;changed.ff[1][0]=4;assert(!pbr::SimpleAlbedoSurface(s,changed,0));
    // The world's stage-1 CURRENT*TFACTOR tint (no texture), with stage 2 off; stage 0 may read CURRENT.
    changed=p;changed.ff[0][2]=1;assert(pbr::SimpleAlbedoSurface(s,changed,0));
    changed.ff[0][2]=3;assert(!pbr::SimpleAlbedoSurface(s,changed,0));
    changed=p;{auto* f=changed.ff[1];f[0]=4;f[1]=1;f[2]=3;f[4]=4;f[5]=1;f[6]=3;f[8]=1;}changed.ff[2][0]=1;
    assert(pbr::SimpleAlbedoSurface(s,changed,0));
    changed.ff[1][4]=2;changed.ff[1][5]=1;assert(pbr::SimpleAlbedoSurface(s,changed,0));
    changed.ff[2][0]=4;assert(!pbr::SimpleAlbedoSurface(s,changed,0));changed.ff[2][0]=1;
    changed.ff[1][2]=2;assert(!pbr::SimpleAlbedoSurface(s,changed,0));changed.ff[1][2]=3;
    changed.ff[1][8]=5;assert(!pbr::SimpleAlbedoSurface(s,changed,0));changed.ff[1][8]=1;
    // A cascade that ENDED (key stage left 0) is the same as DISABLE.
    changed.ff[2][0]=0;assert(pbr::SimpleAlbedoSurface(s,changed,0));
    changed=p;changed.ff[1][0]=0;assert(pbr::SimpleAlbedoSurface(s,changed,0));
    changed=p;changed.ff[0][5]=3;assert(!pbr::SimpleAlbedoSurface(s,changed,0)); // Constant alpha is not albedo alpha.
    changed=p;changed.ff[0][1]|=0x10;assert(!pbr::SimpleAlbedoSurface(s,changed,0)); // Complemented albedo.
    changed=p;changed.ff[0][8]=5;assert(!pbr::SimpleAlbedoSurface(s,changed,0));
    changed=p;changed.ff[0][4]=4;changed.ff[0][6]=0;assert(pbr::SimpleAlbedoSurface(s,changed,0));
    s.tss[0][kTssColorSign]=1;assert(!pbr::SimpleAlbedoSurface(s,p,0));
    s={};p.flat=1;assert(!pbr::SimpleAlbedoSurface(s,p,0));
    // The facades' night-window cascade, exactly as captured: s0 4 2 0|2 2 r5, s1 4 2 3|2 2 r1, s2 18 5 1|2 5 r1.
    PixelKey w;w.tex[0].kind=w.tex[1].kind=kTex2D;
    {auto* f=w.ff[0];f[0]=4;f[1]=2;f[2]=0;f[4]=2;f[5]=2;f[8]=5;}
    {auto* f=w.ff[1];f[0]=4;f[1]=2;f[2]=3;f[4]=2;f[5]=2;f[8]=1;}
    {auto* f=w.ff[2];f[0]=18;f[1]=5;f[2]=1;f[4]=2;f[5]=5;f[8]=1;}
    s={};assert(pbr::WindowLayerSurface(s,w,0)&&!pbr::WindowLayerSurface(s,w,1)&&!pbr::SimpleAlbedoSurface(s,w,0));
    auto v=w;v.ff[2][1]=1;v.ff[2][2]=5;assert(!pbr::WindowLayerSurface(s,v,0)); // CURRENT + TEMP*CURRENT.a: not the windows
    v=w;v.ff[0][8]=1;assert(!pbr::WindowLayerSurface(s,v,0));
    v=w;v.ff[1][2]=0;assert(!pbr::WindowLayerSurface(s,v,0)); // a diffuse-lit second texture
    v=w;v.ff[2][5]=2;assert(!pbr::WindowLayerSurface(s,v,0)); // alpha other than the window mask
    v=w;v.ff[3][0]=4;assert(!pbr::WindowLayerSurface(s,v,0));
    v=w;v.tex[1].kind=kTexNone;assert(!pbr::WindowLayerSurface(s,v,0));
    v=w;v.tex[2].kind=kTex2D;assert(!pbr::WindowLayerSurface(s,v,0));
    v=w;v.alphakill=1;assert(!pbr::WindowLayerSurface(s,v,0));
    // The world's blend-layer pixel shader, word for word as captured.
    PixelKey b;b.programmable=1;b.count=0x11103;b.tex_modes=0x21;b.tex[0].kind=b.tex[1].kind=kTex2D;
    const uint32_t ri[3]={0xC8C40000,0xC9C40000,0x1DCDCC3D},ro[3]={0xC0,0xD0,0xC00},ai[3]={0xD8D41010,0xD9D41010,0},ao[3]={0xC0,0xD0,0};
    for(unsigned i=0;i<3;++i){b.rgb_in[i]=ri[i];b.rgb_out[i]=ro[i];b.a_in[i]=ai[i];b.a_out[i]=ao[i];}
    s={};assert(pbr::BlendLayerProgram(s,b,0)&&!pbr::BlendLayerProgram(s,b,1)&&!pbr::SimpleAlbedoSurface(s,b,0));
    auto q=b;q.rgb_in[2]^=1;assert(!pbr::BlendLayerProgram(s,q,0));
    q=b;q.a_out[2]=0xC0;assert(!pbr::BlendLayerProgram(s,q,0)); // a stage-2 alpha write changes the output alpha
    q=b;q.final_abcd=1;assert(!pbr::BlendLayerProgram(s,q,0));
    q=b;q.rgb_in[3]=1;assert(!pbr::BlendLayerProgram(s,q,0));
    q=b;q.tex_modes=0x41;assert(!pbr::BlendLayerProgram(s,q,0));
    q=b;q.programmable=0;assert(!pbr::BlendLayerProgram(s,q,0));
}
