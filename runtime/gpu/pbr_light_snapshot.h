#pragma once
// Exact original-XDK table comparison. Read-only, bounded; never reconstructs or writes legacy lights.
#include <array>
#include <cstdint>

namespace gpu::pbr::light_snapshot {
struct Table {
    uint32_t base=0,capacity=0,packed_slot_types=0;
    std::array<uint32_t,8> active{};
};
constexpr uint32_t kStride=0x90,kRawBytes=0x68,kFlags=0x68,kSlot=0x8c;
// Callbacks must read the SAME locked original device epoch. Bounds/pointer/slot/type/raw-record and
// enabled coverage all must agree; a before-original shadow cannot approve itself. Capacity4096 is a
// conservative observation budget, never a claimed XDK/API limit. Unknown API indices remain excluded.
template<class Readable,class Read32>
bool Matches(const Table& t,const std::array<std::array<uint32_t,26>,8>& shadow,uint32_t enabled,
             Readable readable,Read32 read) {
    if(enabled&~0xffu||t.packed_slot_types&0xffff0000u)return false;
    if(!t.base||!t.capacity) {
        if(t.base||t.capacity||enabled||t.packed_slot_types)return false;
        for(uint32_t p:t.active)if(p)return false;return true;
    }
    if(t.base<0x10000||t.capacity>4096||uint64_t(t.base)+uint64_t(t.capacity)*kStride>(uint64_t(1)<<32))return false;
    uint32_t found=0;
    for(unsigned slot=0;slot<8;++slot) {
        const uint32_t pointer=t.active[slot],type=(t.packed_slot_types>>(slot*2))&3;
        if(!pointer){if(type)return false;continue;}
        if(!type||pointer<t.base)return false;
        const uint64_t offset=uint64_t(pointer)-t.base;
        if(offset%kStride||offset/kStride>=t.capacity||offset/kStride>=8)return false;
        const uint32_t index=uint32_t(offset/kStride),bit=1u<<index;
        if(!(enabled&bit)||(found&bit)||!readable(pointer,kStride))return false;
        if(read(pointer+kSlot)!=slot||!(read(pointer+kFlags)&1u))return false;
        const uint32_t api_type=shadow[index][0];
        const uint32_t expected=api_type==3?1:api_type==1?2:api_type==2?3:0;
        if(!expected||type!=expected)return false;
        for(unsigned i=0;i<26;++i)if(read(pointer+i*4)!=shadow[index][i])return false;
        found|=bit;
    }
    return found==enabled;
}
} // namespace gpu::pbr::light_snapshot
