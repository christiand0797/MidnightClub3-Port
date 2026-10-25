// Real generated FB4E0 and F7170, including the original matrix operations. Explicit four-wheel chassis
// inputs establish width/translation coupling; this does not simulate a road or load an actual tire mesh.
#include "cpu/x86_ops.h"
#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstring>
#include <iostream>
#include <vector>

#undef X86_FUNC
#undef X86_WEAK_FUNC
#define X86_FUNC(name) static void name(X86Context& __restrict ctx, uint8_t* base)
#define X86_WEAK_FUNC(name, impl)
#include "x86_recomp.14.cpp"
#include "x86_recomp.27.cpp"

namespace {
std::vector<uint8_t> guest(0x600000);
constexpr uint32_t model=0x10000, render=0x20000, resources=0x30000, tune=0x40000;
constexpr uint32_t link=0x50000, body=0x51000, sim=0x52000, rear_tune=0x53000;
constexpr uint32_t skeleton=0x60000, bones=0x61000, spins=0x62000, stack=0x70000;
constexpr std::array<uint32_t,4> wheels={0x80000,0x81000,0x82000,0x83000};
constexpr std::array<uint32_t,4> infos={0x90000,0x90100,0x90200,0x90300};
constexpr std::array<uint32_t,4> output={bones,bones+2*0x3C,bones+4*0x3C,bones+6*0x3C};
void W(uint32_t p,uint32_t v) { std::memcpy(guest.data()+p,&v,4); }
uint32_t W(uint32_t p) { uint32_t v; std::memcpy(&v,guest.data()+p,4); return v; }
void F(uint32_t p,float v) { std::memcpy(guest.data()+p,&v,4); }
float F(uint32_t p) { float v; std::memcpy(&v,guest.data()+p,4); return v; }
uint32_t Bits(float v) { uint32_t b; std::memcpy(&b,&v,4); return b; }
bool Near(float a,float b,float e=.00002f) { return std::fabs(a-b)<e; }
void Identity(uint32_t p) { for(unsigned i=0;i<12;++i) F(p+4*i,(i==0||i==4||i==8)?1.f:0.f); }
void Invoke(X86Func* fn,uint32_t self,std::initializer_list<uint32_t> args={}) {
    X86Context c{}; c.ecx=self;c.esp=stack;c.fcw=0x37F;c.top=3;c.st[3]=123.5;
    c.ebx=0x11223344;c.ebp=0x22334455;c.esi=0x33445566;c.edi=0x44556677;
    W(stack,0xC0DE); unsigned i=1;for(auto a:args)W(stack+4*i++,a);
    fn(c,guest.data());
    assert(c.esp==stack+4*i && c.top==3 && c.st[3]==123.5);
    assert(c.ebx==0x11223344 && c.ebp==0x22334455 && c.esi==0x33445566 && c.edi==0x44556677);
}
float RetailOffset(float width,bool front,float ignored=0) {
    X86Context c{};c.ecx=model;c.esp=stack;c.fcw=0x37F;
    W(stack,0xC0DE);W(stack+4,Bits(width));W(stack+8,Bits(ignored));W(stack+12,front);
    __imp__sub_000FB4E0(c,guest.data());assert(c.esp==stack+16);
    return float(c.st[c.top]);
}
void Setup() {
    std::fill(guest.begin(),guest.end(),0);
    W(model+4,link);W(link+8,body);W(model+0x18,resources);W(model+0x300,tune);
    W(model+0x2F0,render);W(model+0xD0,skeleton);W(skeleton+8,bones);W(model+0x128,spins);
    W(body+0xB4,sim);W(sim+0x58,rear_tune);
    Identity(model+0xA0);F(0x447C18,.5f);F(0x447C1C,1);F(0x449004,2);
    F(0x44B090,.01744999922811985f);F(0x448DF4,0);F(0x448990,0);
    F(tune+0x1C,.16f);F(tune+0x20,.14f);
    W(resources+0x3818,infos[0]);W(resources+0x381C,infos[1]);
    W(resources+0x3838,infos[2]);W(resources+0x383C,infos[3]);
    W(resources+0xC2,0xFFFFFFFF);W(resources+0xC6,0xFFFFFFFF);
    const std::array<uint32_t,4> slots={0xD8,0xDC,0xE0,0xE4};
    for(unsigned i=0;i<4;++i) {
        W(body+slots[i],wheels[i]);Identity(wheels[i]+0x10);
        // First side at negative chassis X, opposite at positive X; both original physics X bases +X.
        F(wheels[i]+0x34,i<2?-1.f:1.f);F(wheels[i]+0x38,.5f);F(wheels[i]+0x3C,i%2?-1.5f:1.5f);
        F(wheels[i]+0x100,.4f);W(infos[i]+4,2*i);W(infos[i]+8,2*i+1);W(infos[i],i);
        Identity(bones+2*i*0x3C);Identity(bones+(2*i+1)*0x3C);
    }
}
struct Wheel { std::array<float,12> matrix; };
std::array<Wheel,4> Render(float front,float rear,bool neutralize,float offsetF=0,float offsetR=0,
                           float camberF=0,float camberR=0) {
    F(render+0x12D0,front);F(render+0x12C8,rear);F(render+0x12D4,.8f);F(render+0x12CC,.8f);
    F(render+0x12E8,camberF);F(render+0x12EC,camberR);
    F(render+0x12D8,(neutralize?0:RetailOffset(front,true))-offsetF);
    F(render+0x12DC,(neutralize?0:RetailOffset(rear,false))-offsetR);
    Invoke(__imp__sub_000F7170,model);
    std::array<Wheel,4> result{};
    for(unsigned i=0;i<4;++i)std::memcpy(result[i].matrix.data(),guest.data()+output[i],48);
    return result;
}
float CenterX(const Wheel& w) {return w.matrix[9];}
float PointX(const Wheel& w,float x) {return w.matrix[9]+w.matrix[0]*x;}
}

// Exact original matrix routines; the only unavailable surrounding-world queries are explicit boundaries.
extern "C" void sub_00085930(X86Context& c,uint8_t* b) {__imp__sub_00085930(c,b);}
extern "C" void sub_00086900(X86Context& c,uint8_t* b) {__imp__sub_00086900(c,b);}
extern "C" void sub_000869D0(X86Context& c,uint8_t* b) {__imp__sub_000869D0(c,b);}
extern "C" void sub_00087860(X86Context& c,uint8_t* b) {__imp__sub_00087860(c,b);}
extern "C" void sub_000898C0(X86Context& c,uint8_t* b) {__imp__sub_000898C0(c,b);}
extern "C" void sub_00120A80(X86Context& c,uint8_t*) {assert(c.ecx==body);c.eax=sim;c.esp+=4;}
extern "C" void sub_000A65D0(X86Context&,uint8_t*) {assert(false&&"arch correction deliberately disabled");}
extern "C" void sub_002DC452(X86Context&,uint8_t*) {assert(false&&"wheel spin integration deliberately disabled");}
void x86_icall(X86Context&,uint8_t*,uint32_t) {assert(false&&"unexpected virtual call");}

int main() {
    Setup();
    assert(Near(RetailOffset(.195f,true),0));assert(Near(RetailOffset(.755f,true),-.2175f));
    assert(Near(RetailOffset(.755f,false),-.2375f));
    assert(RetailOffset(.755f,true,-100)==RetailOffset(.755f,true,100));
    // The signed automatic shift changes the centre by 200mm for a 400mm width increase when both are above
    // the fit threshold. The outer nominal edge stays put while the inside edge consumes the whole increase.
    auto narrow=Render(.355f,.355f,false),wide=Render(.755f,.755f,false);
    for(unsigned i=0;i<4;++i) {
        const float direction=i<2?-1.f:1.f;
        assert(Near((CenterX(wide[i])-CenterX(narrow[i]))*direction,-.2f));
        float nmin=std::min(PointX(narrow[i],-.25f),PointX(narrow[i],.25f));
        float nmax=std::max(PointX(narrow[i],-.25f),PointX(narrow[i],.25f));
        float wmin=std::min(PointX(wide[i],-.25f),PointX(wide[i],.25f));
        float wmax=std::max(PointX(wide[i],-.25f),PointX(wide[i],.25f));
        assert(Near(i<2?wmin:wmax,i<2?nmin:nmax));
    }
    for(float camber:{-30.f,0.f,20.f})for(float user_offset:{-.2f,0.f,.2f}) {
        auto a=Render(.105f,.195f,true,user_offset,user_offset,camber,camber);
        auto b=Render(.755f,.655f,true,user_offset,user_offset,camber,camber);
        for(unsigned i=0;i<4;++i) {
            assert(Near(CenterX(a[i]),CenterX(b[i])));
            assert(Near(PointX(b[i],.25f)-PointX(a[i],.25f),
                        -(PointX(b[i],-.25f)-PointX(a[i],-.25f))));
        }
    }
    std::cout<<"actual FB4E0/F7170 width pivot: four-wheel outer-edge anchoring reproduced; "
        "width-independent centre yields two-sided growth for both axles/camber/explicit-offset sweeps: PASS\n";
}
