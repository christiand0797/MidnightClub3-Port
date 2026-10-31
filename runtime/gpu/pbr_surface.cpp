#include "gpu/pbr_surface.h"
#include <array>
#include <limits>

namespace gpu::pbr {
namespace {
uint16_t U16(const uint8_t* p){return uint16_t(p[0])|(uint16_t(p[1])<<8);}
uint64_t Little(const uint8_t* p,unsigned bytes) {
    uint64_t v=0;for(unsigned i=0;i<bytes;++i)v|=uint64_t(p[i])<<(i*8);return v;
}
}
ColorProof ProveUploadedColor(Host host,uint32_t w,uint32_t h,uint32_t pitch,
                             std::span<const uint8_t> bytes) noexcept {
    try {
        if(!w||!h||w>8192||h>8192)return {};
        const bool bgra=host==Host::BGRA8;
        const uint32_t block_bytes=host==Host::BC1?8:16;
        if(!bgra&&host!=Host::BC1&&host!=Host::BC2&&host!=Host::BC3)return {};
        const uint64_t row_bytes=bgra?uint64_t(w)*4:uint64_t((w+3)/4)*block_bytes;
        const uint64_t rows=bgra?h:(h+3)/4;
        if(pitch<row_bytes||uint64_t(pitch)*rows!=bytes.size())return {};
        // BC textures with padded host dimensions require separate accepted-image proof.
        if(!bgra&&((w&3)||(h&3)))return {};
        std::vector<uint8_t> alpha(8+size_t(w)*h);
        for(unsigned i=0;i<4;++i){alpha[i]=uint8_t(w>>(i*8));alpha[4+i]=uint8_t(h>>(i*8));}
        if(bgra) {
            for(uint32_t y=0;y<h;++y)for(uint32_t x=0;x<w;++x)
                alpha[8+size_t(y)*w+x]=bytes[size_t(y)*pitch+size_t(x)*4+3];
        } else for(uint32_t by=0;by<h/4;++by)for(uint32_t bx=0;bx<w/4;++bx) {
            const uint8_t* b=bytes.data()+size_t(by)*pitch+size_t(bx)*block_bytes;
            std::array<uint8_t,8> palette{};
            if(host==Host::BC3) {
                palette[0]=b[0];palette[1]=b[1];
                if(b[0]>b[1])for(unsigned i=1;i<=6;++i)palette[i+1]=uint8_t(((7-i)*b[0]+i*b[1])/7);
                else {for(unsigned i=1;i<=4;++i)palette[i+1]=uint8_t(((5-i)*b[0]+i*b[1])/5);palette[6]=0;palette[7]=255;}
            }
            for(unsigned p=0;p<16;++p) {
                uint8_t a=255;
                if(host==Host::BC1) {if(U16(b)<=U16(b+2)&&((Little(b+4,4)>>(p*2))&3)==3)a=0;}
                else if(host==Host::BC2)a=uint8_t(((Little(b,8)>>(p*4))&15)*17);
                else a=palette[(Little(b+2,6)>>(p*3))&7];
                alpha[8+size_t(by*4+p/4)*w+bx*4+p%4]=a;
            }
        }
        const uint8_t first=alpha[8];
        bool constant=true;for(size_t i=9;i<alpha.size();++i)if(alpha[i]!=first){constant=false;break;}
        return {w,h,Sha256(alpha),constant,first};
    } catch(...) {return {};}
}
} // namespace gpu::pbr
