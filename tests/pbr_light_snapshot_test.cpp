#include "gpu/pbr_light_snapshot.h"
#include <cassert>
#include <cstring>
#include <vector>
using namespace gpu::pbr::light_snapshot;
int main() {
    constexpr uint32_t base=0x10000;
    std::vector<uint32_t> memory(16*kStride/4);
    Table t;t.base=base;t.capacity=16;
    std::array<std::array<uint32_t,26>,8> shadow{};
    auto read=[&](uint32_t p){assert(p>=base&&(p-base)/4<memory.size());return memory[(p-base)/4];};
    auto valid=[&](uint32_t p,uint32_t bytes){return p>=base&&uint64_t(p-base)+bytes<=memory.size()*4;};
    auto matches=[&](uint32_t enabled){return Matches(t,shadow,enabled,valid,read);};
    assert(matches(0));
    shadow[2][0]=3;shadow[2][1]=0x3f800000; // actual API directional index2, hardware slot5
    t.active[5]=base+2*kStride;t.packed_slot_types=1u<<(5*2);
    std::memcpy(memory.data()+2*kStride/4,shadow[2].data(),kRawBytes);
    memory[(2*kStride+kFlags)/4]=1;memory[(2*kStride+kSlot)/4]=5;
    assert(matches(4));assert(!matches(0));
    memory[(2*kStride+4)/4]^=1;assert(!matches(4));memory[(2*kStride+4)/4]^=1;
    memory[(2*kStride+kSlot)/4]=4;assert(!matches(4));memory[(2*kStride+kSlot)/4]=5;
    t.active[4]=t.active[5];t.packed_slot_types|=1u<<8;assert(!matches(4));t.active[4]=0;t.packed_slot_types&=~(3u<<8);
    t.active[5]+=4;assert(!matches(4));t.active[5]-=4;
    t.active[5]=base+9*kStride;assert(!matches(4));t.active[5]=base+2*kStride;
    t.packed_slot_types=2u<<10;assert(!matches(4));t.packed_slot_types=1u<<10;
    memory[(2*kStride+kFlags)/4]=0;assert(!matches(4));memory[(2*kStride+kFlags)/4]=1;
    t.capacity=4097;assert(!matches(4));t.capacity=16;
    t.base=0xfffffff0;assert(!matches(4));t={};assert(matches(0));assert(!matches(4));
    t.active[0]=base;assert(!matches(0));
}
