#pragma once
#include "gpu/vertex.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <dxgiformat.h>

namespace gpu {
// Diagnostic recognition of this title's indexed three-influence skin program (original XBE 0x4B0ED0).
// Its first instruction multiplies v12 by c9.w, followed by relative bone-palette reads. No draw behavior changes.
inline bool IsIndexedSkinProgram(const std::vector<uint32_t>& p) {
    if(p.size()<4) return false;
    const bool indices=(p[1]>>21&15)==2 && (p[1]>>9&15)==12 && (p[1]>>13&255)==105 &&
                       (p[2]>>26&3)==2 && (p[2]>>11&3)==3;
    if(!indices) return false;
    for(size_t i=3;i<p.size();i+=4) if(p[i]&2) return true;
    return false;
}

// Record every bone row actually present in the assembled vertex range, rather than inferring animation from
// unrelated nonzero matrices. Unknown index layouts are deliberately left unclassified.
inline std::array<bool,192> SkinBoneRows(const Assembled& a,float scale) {
    std::array<bool,192> rows{};
    if(!(a.regs&(1u<<12)) || !std::isfinite(scale)) return rows;
    const auto& e=a.elements[12];
    if(e.buffer>=a.buffer_count) return rows;
    const auto& b=a.buffers[e.buffer];
    if(!b.src || !b.stride) return rows;
    for(uint32_t v=0;v<a.vertex_count;++v) {
        const size_t at=size_t(v)*b.stride+e.offset;
        float values[3]{};
        if(e.format==DXGI_FORMAT_B8G8R8A8_UNORM || e.format==DXGI_FORMAT_R8G8B8A8_UNORM) {
            if(at+4>b.bytes) return {};
            for(unsigned i=0;i<3;++i) values[i]=b.src[at+(e.format==DXGI_FORMAT_B8G8R8A8_UNORM?2-i:i)]/255.0f;
        } else if(e.format==DXGI_FORMAT_R32G32B32A32_FLOAT) {
            if(at+16>b.bytes) return {};
            std::memcpy(values,b.src+at,12);
        } else return {};
        for(float f:values) {
            const double row=std::floor(double(f*scale)+.001);
            if(std::isfinite(row) && row>=0 && row<192) rows[size_t(row)]=true;
        }
    }
    return rows;
}
}  // namespace gpu
