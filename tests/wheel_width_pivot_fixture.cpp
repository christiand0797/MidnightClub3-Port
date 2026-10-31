// Real generated FB4E0 and F7170, including the original matrix operations. Explicit four-wheel chassis
// inputs establish width/translation coupling; this does not simulate a road or load an actual tire mesh.
#include "cpu/x86_ops.h"
#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <vector>

#undef X86_FUNC
#undef X86_WEAK_FUNC
#define X86_FUNC(name) static void name(X86Context& __restrict ctx, uint8_t* base)
#define X86_WEAK_FUNC(name, impl)
#include "x86_recomp.14.cpp"
#include "x86_recomp.27.cpp"
#include "wheel_camber.h"

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
// As production wheel_camber.cpp: the two F7170 camber calls take the rigid tilt unless a check asks for the original.
bool original_camber=false;
extern "C" void sub_000869D0(X86Context& c,uint8_t* b) {
    const uint32_t caller=W(c.esp);
    std::array<float,6> rows;std::memcpy(rows.data(),b+c.ecx,24);
    if(!original_camber&&(caller==0xF7719||caller==0xF7D3E)&&wheel_camber::Rotate(rows,F(c.esp+4))) {
        std::memcpy(b+c.ecx,rows.data(),24);c.esp+=8;return;
    }
    __imp__sub_000869D0(c,b);
}
extern "C" void sub_00087860(X86Context& c,uint8_t* b) {__imp__sub_00087860(c,b);}
extern "C" void sub_000898C0(X86Context& c,uint8_t* b) {__imp__sub_000898C0(c,b);}
extern "C" void sub_00120A80(X86Context& c,uint8_t*) {assert(c.ecx==body);c.eax=sim;c.esp+=4;}
extern "C" void sub_000A65D0(X86Context&,uint8_t*) {assert(false&&"arch correction deliberately disabled");}
extern "C" void sub_002DC452(X86Context&,uint8_t*) {assert(false&&"wheel spin integration deliberately disabled");}
void x86_icall(X86Context&,uint8_t*,uint32_t) {assert(false&&"unexpected virtual call");}

int main() {
#ifdef _WIN32
    _set_abort_behavior(0,_WRITE_ABORT_MSG|_CALL_REPORTFAULT);  // a failed assert exits instead of waiting on a dialog
#endif
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
    // Camber tilts the scaled tire rigidly: axle and vertical axes stay perpendicular and keep 2*width and the
    // diameter, on both sides, while the original rotation of the already-scaled rows shears them.
    auto Dot=[](const Wheel& w){return w.matrix[0]*w.matrix[3]+w.matrix[1]*w.matrix[4]+w.matrix[2]*w.matrix[5];};
    auto Len=[](const Wheel& w,int r){const float* m=w.matrix.data()+3*r;return std::sqrt(m[0]*m[0]+m[1]*m[1]+m[2]*m[2]);};
    auto Cos=[&](const Wheel& a,const Wheel& b,int r){const float* p=a.matrix.data()+3*r;const float* q=b.matrix.data()+3*r;
        return (p[0]*q[0]+p[1]*q[1]+p[2]*q[2])/(Len(a,r)*Len(b,r));};
    // F75F1 cambers the rear only when the rear tune's +0x28 is non-zero.
    F(rear_tune+0x28,1);
    const auto flat=Render(.755f,.655f,true,0,0,0,0);
    for(float camber:{-30.f,20.f}) {
        auto tilted=Render(.755f,.655f,true,0,0,camber,camber);
        const float expect_cos=std::cos(camber*.01744999922811985f);
        for(unsigned i=0;i<4;++i) {
            const float width=2*(i%2?.655f:.755f);
            assert(Near(Dot(tilted[i])/(Len(tilted[i],0)*Len(tilted[i],1)),0,.0001f));
            assert(Near(Len(tilted[i],0),width,.0001f)&&Near(Len(tilted[i],1),.8f,.0001f));
            // the axle turned by exactly the camber away from its untilted direction
            if(!Near(Cos(tilted[i],flat[i],0),expect_cos,.0001f)) {
                std::cerr<<"camber "<<camber<<" wheel "<<i<<" cos "<<Cos(tilted[i],flat[i],0)<<" expected "<<expect_cos<<"\n flat";
                for(float v:flat[i].matrix)std::cerr<<' '<<v;
                std::cerr<<"\n tilted";
                for(float v:tilted[i].matrix)std::cerr<<' '<<v;
                std::cerr<<'\n';
                assert(false);
            }
        }
        original_camber=true;
        auto sheared=Render(.755f,.655f,true,0,0,camber,camber);
        original_camber=false;
        for(unsigned i=0;i<4;++i) assert(std::fabs(Dot(sheared[i])/(Len(sheared[i],0)*Len(sheared[i],1)))>.1f);
    }
    // At the stock -2.5 degrees with unit-length rows the tilt is the original rotation.
    std::array<float,6> unit={1,0,0,0,1,0},stock=unit;
    wheel_camber::Rotate(unit,-2.5f*.01744999922811985f);
    wheel_camber::RotateOriginal(stock,-2.5f*.01744999922811985f);
    for(unsigned i=0;i<6;++i) assert(Near(unit[i],stock[i],1e-6f));
    std::cout<<"actual FB4E0/F7170 width pivot: four-wheel outer-edge anchoring reproduced; "
        "width-independent centre yields two-sided growth for both axles/camber/explicit-offset sweeps; "
        "camber tilts the scaled tire rigidly where the original sheared it: PASS\n";
}
