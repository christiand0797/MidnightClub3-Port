#include "gpu/skinning_capture.h"
#include <cassert>
#include <limits>

int main() {
    // Original MC3 skin shader first instruction at XBE4B0ED4; a later instruction has indirect constants.
    std::vector<uint32_t> p={0,0x004D381B,0x09FE186C,0x2E200FF8,0,0,0,2};
    assert(gpu::IsIndexedSkinProgram(p));
    auto changed=p;changed[1]^=1u<<9;assert(!gpu::IsIndexedSkinProgram(changed));
    changed=p;changed[1]^=1u<<13;assert(!gpu::IsIndexedSkinProgram(changed));
    changed=p;changed[2]^=1u<<26;assert(!gpu::IsIndexedSkinProgram(changed));
    changed=p;changed[7]=0;assert(!gpu::IsIndexedSkinProgram(changed));
    assert(!gpu::IsIndexedSkinProgram({}));
    // The exact raw BGRA declaration's XYZ is R,G,B, irrespective of alpha255.
    uint8_t raw[]={0,12,18,255,191,0,3,255};
    gpu::Assembled a{};a.regs=1u<<12;a.vertex_count=2;a.buffer_count=1;
    a.elements[12]={DXGI_FORMAT_B8G8R8A8_UNORM,0,0};a.buffers[0]={raw,sizeof(raw),4};
    const auto rows=gpu::SkinBoneRows(a,256);
    for(unsigned i=0;i<192;++i) assert(rows[i]==(i==0||i==3||i==12||i==18||i==191));
    const float converted[]={18/255.0f,12/255.0f,0,1,3/255.0f,0,191/255.0f,1};
    a.elements[12].format=DXGI_FORMAT_R32G32B32A32_FLOAT;
    a.buffers[0]={reinterpret_cast<const uint8_t*>(converted),sizeof(converted),16};
    assert(gpu::SkinBoneRows(a,256)==rows);
    // RGBA unsigned-byte layouts use their original component order, preserving the same set of rows.
    a.elements[12].format=DXGI_FORMAT_R8G8B8A8_UNORM;a.buffers[0]={raw,sizeof(raw),4};
    assert(gpu::SkinBoneRows(a,256)==rows);
    const std::array<bool,192> empty{};
    a.buffers[0].bytes=7;assert(gpu::SkinBoneRows(a,256)==empty);
    a.buffers[0].bytes=8;a.elements[12].format=DXGI_FORMAT_R16_SINT;
    assert(gpu::SkinBoneRows(a,256)==empty);
    a.elements[12].format=DXGI_FORMAT_B8G8R8A8_UNORM;
    assert(gpu::SkinBoneRows(a,std::numeric_limits<float>::quiet_NaN())==empty);
    a.regs=0;assert(gpu::SkinBoneRows(a,256)==empty);
}
