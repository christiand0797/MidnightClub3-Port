#include "gpu/pbr_surface.h"
#include <cassert>
#include <cstring>
using namespace gpu;
using namespace gpu::pbr;
int main() {
    Mip m{4,4,std::vector<uint8_t>(64,255)};
    auto check=[&](Host host,const std::vector<uint8_t>& bytes,uint32_t pitch){
        auto proof=ProveUploadedColor(host,4,4,pitch,bytes);assert(proof.ready()&&proof.alpha_sha256==AlphaDigest(m));
    };
    check(Host::BGRA8,m.pixels,16);
    assert(ProveUploadedColor(Host::BGRA8,4,4,16,m.pixels).constant_alpha);
    std::vector<uint8_t> bc1(8);bc1[0]=255;check(Host::BC1,bc1,8);
    bc1.assign(8,0);bc1[2]=255;bc1[4]=3;m.pixels[3]=0;check(Host::BC1,bc1,8);
    assert(!ProveUploadedColor(Host::BC1,4,4,8,bc1).constant_alpha);
    bc1[0]=255;bc1[2]=0;m.pixels[3]=255;check(Host::BC1,bc1,8); // color-index3 opaque in four-color mode
    std::vector<uint8_t> bc2(16,0);for(unsigned p=0;p<16;++p){bc2[p/2]|=uint8_t(p<<((p%2)*4));m.pixels[p*4+3]=uint8_t(p*17);}
    check(Host::BC2,bc2,16);
    std::vector<uint8_t> bc3(16,0);bc3[0]=210;bc3[1]=70;
    const uint8_t p7[8]={210,70,190,170,150,130,110,90};
    uint64_t bits=0;for(unsigned p=0;p<16;++p){bits|=uint64_t(p%8)<<(p*3);m.pixels[p*4+3]=p7[p%8];}
    for(unsigned i=0;i<6;++i)bc3[i+2]=uint8_t(bits>>(i*8));check(Host::BC3,bc3,16);
    bc3[0]=10;bc3[1]=110;const uint8_t p5[8]={10,110,30,50,70,90,0,255};
    for(unsigned p=0;p<16;++p)m.pixels[p*4+3]=p5[p%8];check(Host::BC3,bc3,16);
    std::vector<uint8_t> padded(80,0);for(unsigned y=0;y<4;++y)std::memcpy(padded.data()+y*20,m.pixels.data()+y*16,16);
    check(Host::BGRA8,padded,20);
    assert(!ProveUploadedColor(Host::BGRA8,4,4,15,padded).ready());
    assert(!ProveUploadedColor(Host::BC3,3,4,16,bc3).ready());
    assert(!ProveUploadedColor(Host::R32F,4,4,16,m.pixels).ready());
    assert(!ProveUploadedColor(Host::BGRA8,8193,1,0,{}).ready());
    bc3.pop_back();assert(!ProveUploadedColor(Host::BC3,4,4,16,bc3).ready());
}
