#include "xdk/hle.h"
#undef X86_FUNC
#define X86_FUNC(name) static void name(X86Context& __restrict ctx, uint8_t* base)
#include "../runtime/xdk/d3d8.cpp"
#include <cassert>
#include <iostream>

extern "C" void FixtureOriginalSetOne(X86Context&,uint8_t*);
extern "C" void FixtureOriginalSetOneFast(X86Context&,uint8_t*);
extern "C" void FixtureOriginalSetFour(X86Context&,uint8_t*);
extern "C" void FixtureOriginalSetBulkFast(X86Context&,uint8_t*);
extern "C" void FixtureOriginalSetBulk(X86Context&,uint8_t*);
extern "C" void FixtureOriginalCaptureState(X86Context&,uint8_t*);

namespace fixture {
std::vector<uint8_t> memory(0x1000000);
constexpr uint32_t stack=0x10000, source=0x30000, block=0x40000, scratch=0x600000, shadow=0x421CC0;
constexpr size_t table_bytes=192*16;
struct Snapshot { std::array<uint8_t,table_bytes> gpu,guest; uint32_t load,part; };
void Setup() {
    std::fill(memory.begin(),memory.end(),0);
    std::memset(d3d::g_state.vsc.data(),0x37,table_bytes);
    std::memset(memory.data()+shadow,0x37,table_bytes);
    // Finite, negative-zero and NaN payloads must be copied bit-for-bit, rather than converted as floats.
    for(uint32_t i=0;i<80;++i) kernel::W32(source+4*i, i%3==0?0x80000000:i%3==1?0x7FC00000+i:0x3E800000+i);
    d3d::g_scratch_push=scratch;d3d::g_scratch_size=0x10000;d3d::g_walk=scratch;
    d3d::g_const_load=0;d3d::g_const_part=0;
    d3d::SetDev(d3d::kDevPush,scratch);d3d::SetDev(d3d::kDevThreshold,scratch+0xF000);
    kernel::W32(SYM(D3D_pDevice),SYM(D3D_Device));
}
void Call(X86Func* fn,uint32_t reg,uint32_t src,uint32_t words=0,bool bulk=false) {
    X86Context c{};c.esp=stack;c.ecx=reg;c.edx=src;
    kernel::W32(stack+4,words);
    fn(c,memory.data());assert(c.esp==stack+(bulk?8:4));
}
Snapshot Take() {
    Snapshot s{};std::memcpy(s.gpu.data(),d3d::g_state.vsc.data(),table_bytes);
    std::memcpy(s.guest.data(),memory.data()+shadow,table_bytes);
    s.load=d3d::g_const_load;s.part=d3d::g_const_part;return s;
}
void Same(X86Func* original,X86Func* host,uint32_t reg,uint32_t words=0,bool bulk=false) {
    Setup();Call(original,reg,source,words,bulk);d3d::Walk(d3d::Dev(d3d::kDevPush));const auto expected=Take();
    Setup();Call(host,reg,source,words,bulk);const auto actual=Take();
    assert(actual.gpu==expected.gpu && actual.guest==expected.guest);
    assert(actual.load==expected.load && actual.part==expected.part);
}
void Capture(uint32_t reg) {
    // A state block containing exactly one vertex-constant record; the original reader copies guest shadow.
    std::memset(memory.data()+block,0,0x100);kernel::W32(block+0x18,1);kernel::W32(block+0x3C,reg);
    X86Context c{};c.esp=stack;kernel::W32(stack+4,block);
    FixtureOriginalCaptureState(c,memory.data());assert(c.esp==stack+8);
    assert(std::memcmp(memory.data()+block+0x40,d3d::g_state.vsc[reg].data(),16)==0);
}
}
namespace kernel { uint8_t* g_base=fixture::memory.data(); }
namespace logx { void Write(Level,const char*,...) {} }
namespace mem {
bool IsCommitted(uint32_t va) { return va < fixture::memory.size(); }
uint32_t AllocSystem(uint32_t,uint32_t) { assert(false);return 0; }
uint32_t AllocContiguous(uint32_t,uint32_t,uint32_t,uint32_t) { assert(false);return 0; }
uint32_t PhysicalFromVirtual(uint32_t va) { return va; }
uint32_t VirtualFromPhysical(uint32_t pa) { return pa; }
}

int main() {
    using namespace fixture;
    static_assert(HAS_D3D_VertexShaderConstants && SYM(D3D_VertexShaderConstants)==shadow);
    for(uint32_t reg:{0u,96u,105u,191u}) {
        Same(FixtureOriginalSetOne,sub_00414EB0,reg);
        Same(FixtureOriginalSetOneFast,sub_00414F10,reg);
    }
    for(uint32_t reg:{0u,96u,105u,188u}) Same(FixtureOriginalSetFour,sub_00414F60,reg);
    for(uint32_t words:{0u,1u,3u,4u,5u,15u,16u,17u,64u})
        Same(FixtureOriginalSetBulkFast,sub_00415010,12,words,true);
    Setup();Call(sub_00415010,105,0,0,true);
    assert(d3d::g_const_load==105 && d3d::g_const_part==0);

    Setup();Call(sub_00414EB0,105,source);Capture(105);
    Setup();Call(sub_00414F60,104,source);Capture(105);Capture(107);
    // Fast setters leave the shadow unchanged, independently of the device's pure-device flag.
    for(uint32_t flags:{0u,0x10u}) {
        Setup();d3d::SetDev(XDK_CDevice_Flags,flags);auto before=Take();
        Call(sub_00414F10,105,source);assert(Take().guest==before.guest);
        Call(sub_00415010,105,source,5,true);assert(Take().guest==before.guest);
    }
    // The original non-Fast bulk wrapper mirrors only when the pure-device flag is clear.
    for(uint32_t flags:{0u,0x10u}) {
        Setup();d3d::SetDev(XDK_CDevice_Flags,flags);auto before=Take();
        Call(FixtureOriginalSetBulk,105,source,5,true);d3d::Walk(d3d::Dev(d3d::kDevPush));
        assert(std::memcmp(d3d::g_state.vsc[105].data(),memory.data()+source,20)==0);
        if(!flags) assert(std::memcmp(memory.data()+shadow+105*16,memory.data()+source,20)==0);
        else assert(Take().guest==before.guest);
    }
    // Pending older inline constants cannot overwrite a later HLE call.
    Setup();uint32_t commands[]={0x41EA4,105,0x100B80,0,0,0,0};
    std::memcpy(memory.data()+scratch,commands,sizeof(commands));d3d::SetDev(d3d::kDevPush,scratch+sizeof(commands));
    Call(sub_00414EB0,105,source);Capture(105);
    assert(std::memcmp(d3d::g_state.vsc[105].data(),memory.data()+source,16)==0);
    // Continue the exact hardware cursor with raw data and no LOAD, including a partial-register Fast write.
    Setup();Call(sub_00415010,105,source,5,true);
    uint32_t continuation[]={0x000C0B84,0x3F800000,0x40000000,0x40400000};
    std::memcpy(memory.data()+scratch,continuation,sizeof(continuation));
    d3d::SetDev(d3d::kDevPush,scratch+sizeof(continuation));d3d::Walk(d3d::Dev(d3d::kDevPush));
    assert(std::bit_cast<uint32_t>(d3d::g_state.vsc[106][0])==kernel::R32(source+16));
    assert(d3d::g_state.vsc[106][1]==1 && d3d::g_state.vsc[106][2]==2 && d3d::g_state.vsc[106][3]==3);
    assert(d3d::g_const_load==107 && d3d::g_const_part==0);
    // Component selection is encoded by the method, not by a software sequential counter. Repeated Y writes
    // keep writing Y, a W write advances only the register pointer, and a following Z write selects Z again.
    Setup();uint32_t component_writes[]={0x41EA4,105,0x40080B84,0x3F800000,0x40000000,
                                        0x00040B8C,0x40400000,0x00040B88,0x40800000};
    std::memcpy(memory.data()+scratch,component_writes,sizeof(component_writes));
    d3d::SetDev(d3d::kDevPush,scratch+sizeof(component_writes));d3d::Walk(d3d::Dev(d3d::kDevPush));
    assert(std::bit_cast<uint32_t>(d3d::g_state.vsc[105][0])==0x37373737);
    assert(d3d::g_state.vsc[105][1]==2 && d3d::g_state.vsc[105][3]==3);
    assert(d3d::g_state.vsc[106][2]==4 && d3d::g_const_load==106 && d3d::g_const_part==3);
    // Bounds retain the preexisting fail-closed behavior; partial final registers remain within both tables.
    Setup();const auto before=Take();Call(sub_00414F60,192,source);Call(sub_00414EB0,105,0);
    Call(sub_00415010,0xFFFFFFFF,source,0xFFFFFFFF,true);assert(Take().gpu==before.gpu && Take().guest==before.guest);
    Call(sub_00414F60,191,source);assert(std::memcmp(d3d::g_state.vsc[191].data(),memory.data()+source,16)==0);
    assert(kernel::R32(shadow+table_bytes)==0);
    Setup();Call(sub_00415010,191,source,3,true);
    assert(std::memcmp(d3d::g_state.vsc[191].data(),memory.data()+source,12)==0);
    assert(std::bit_cast<uint32_t>(d3d::g_state.vsc[191][3])==0x37373737);
    // All valid bone row indices survive normalized-byte input, the title's scale256 and the NV2A ARL floor.
    for(uint32_t row=0;row<192;++row) assert(uint32_t(std::floor((row/255.0f)*256.0f+.001f))==row);
    std::cout<<"Original XDK constant/shadow parity and real state-block reader passed\n";
}
