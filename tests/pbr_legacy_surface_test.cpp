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
}
