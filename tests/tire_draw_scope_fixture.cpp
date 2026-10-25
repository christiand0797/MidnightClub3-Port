// Production ownership/resource guards and scoped queued metadata, with the actual original 1B5F20 dispatcher.
#define main UnusedGarageFixtureMain
#include "garage_test.cpp"
#undef main
#include "../templates/game/src/game/garage.h"
#include "gpu/state.h"
#include "cpu/x86_ops.h"
#include <array>
#include <fstream>
#include <iostream>
#include <mutex>
#include <stdexcept>

#undef X86_FUNC
#undef X86_WEAK_FUNC
#define X86_FUNC(name) static void name(X86Context& __restrict ctx, uint8_t* base)
#define X86_WEAK_FUNC(name, impl)
#define __imp__sub_001B5F20 OriginalTireDispatch
#include "x86_recomp.49.cpp"
#undef __imp__sub_001B5F20
extern "C" void __imp__sub_001B5F20(X86Context& ctx, uint8_t* base) { OriginalTireDispatch(ctx, base); }
X86_EXTERN_FUNC(sub_001B5F20);
X86_EXTERN_FUNC(sub_00106F90);

namespace scope {
gpu::State state;
std::recursive_mutex lock;
std::vector<gpu::State::TireFit> observed;
bool fail = false;
constexpr uint32_t car=0x1D0000, sim=0x1D1000, cfg=0x1D2000, model=0x1E0000, render=0x1E1000;
constexpr uint32_t resources=0x1E4000, factory=0x1E5000, db=0x210000, table=0x211000;
constexpr uint32_t descriptor=0x212000, items=0x213000, handle=0x214000, record=0x220000;
constexpr uint32_t shape=record+0x80, shapes=record+0x90, geom=record+0xA0, vb=record+0xD0;
constexpr uint32_t vertices=record+0x100, ib=vertices+3*28, count_word=ib+4, index_desc=ib+12, indices=ib+16;
constexpr std::array<float,8> ratios={.415532f,.47585f,.538852f,.6f,.66f,.725f,.782f,.83f};
void F(uint32_t p,float v){std::memcpy(fixture::memory.data()+p,&v,4);}
void Setup() {
    using kernel::W32;
    for(unsigned i=0;i<8;++i)F(0x471958+4*i,ratios[i]);
    for(unsigned r=12;r<=28;++r)F(0x4B3910+4*r,float(r+3)*.0254f);
    W32(factory+0x44,car); W32(car+0x50,sim); W32(car+0x64,cfg); W32(car+0x68,render);
    fixture::Call(sub_002CBDE0,factory);
    W32(model+4,car);W32(model+0x18,resources);W32(render+0x1800,model);
    fixture::memory[cfg+0x1A]=20;fixture::memory[cfg+0x1B]=20;
    fixture::memory[cfg+0x1C]=33;fixture::memory[cfg+0x1D]=32;
    W32(render+0x11B0,0);W32(render+0x11B4,33);W32(render+0x11B8,32);
    W32(render+0x11BC,20);W32(render+0x11C0,20);
    F(render+0x12D4,23*.0254f/.975f);F(render+0x12CC,23*.0254f/.95f);
    W32(resources+0xC4,handle);W32(resources+0xC8,handle);
    W32(0x509E1C,db);W32(db+0x2C,table);W32(db+0x44,8);
    for(unsigned i=0;i<8;++i)W32(table+4*i,descriptor);
    W32(descriptor,items);W32(descriptor+4,11);for(unsigned b=0;b<11;++b)W32(items+4*b,handle);
    W32(handle+4,record);W32(record+0xC,0x400);W32(record+0x20,0x230000);W32(0x230018,0x123456);
    W32(record+0x30,shape);W32(shape+4,1<<16);W32(shape+8,shapes);W32(shapes,geom);
    W32(geom+4,28u<<24);W32(geom+0x10,index_desc);W32(geom+0x18,vb);W32(geom+0x1C,indices);
    W32(vb,1);W32(vb+4,vertices);W32(index_desc,count_word);W32(ib,1);W32(count_word,3u<<16);
    for(unsigned i=0;i<3;++i){F(vertices+28*i,i==1?-.263825f:.25f);F(vertices+28*i+4,i==0?.415f:.5f);F(vertices+28*i+8,0);kernel::W16(indices+2*i,uint16_t(i));}
}
void UnscopedTireCall(unsigned axle,uint32_t caller=0x1080C5) {
    X86Context ctx{};ctx.esp=fixture::stack;ctx.ecx=handle;ctx.ebx=resources;
    kernel::W32(ctx.esp,caller);kernel::W32(ctx.esp+12,0);kernel::W32(ctx.esp+0x34,axle);
    sub_001B5F20(ctx,kernel::g_base);assert(ctx.esp==fixture::stack+28);
}
unsigned selected_axle=0;uint32_t selected_caller=0x1080C5,selected_pass=0,selected_handle=handle;
void Draw(unsigned axle,uint32_t caller=0x1080C5) {
    selected_axle=axle;selected_caller=caller;
    X86Context ctx{};ctx.esp=fixture::stack+0x1000;ctx.ecx=resources;
    kernel::W32(ctx.esp+0x10,render);
    sub_00106F90(ctx,kernel::g_base);
}
bool Active(const gpu::State::TireFit& f){return f.vertex_data[0]==vertices;}
}
extern "C" void __imp__sub_00106F90(X86Context& ctx,uint8_t*base) {
    // Execute the original caller's relevant prologue and six pushes, rather than inventing its ABI.
    using kernel::R32;using kernel::W32;
    const uint32_t entry=ctx.esp;
    ctx.esp-=0x288;X86_PUSH32(ctx.ebx);X86_PUSH32(ctx.esi);
    ctx.esi=R32(ctx.esp+0x2A0);assert(ctx.esi==scope::render);ctx.ebx=ctx.ecx;
    X86_PUSH32(ctx.ebp);X86_PUSH32(ctx.edi);
    const uint32_t frame=ctx.esp;W32(frame+0x18,scope::selected_axle);
    ctx.esi=scope::selected_pass;ctx.ecx=scope::selected_handle;
    X86_PUSH32(0);X86_PUSH32(0);X86_PUSH32(0);X86_PUSH32(ctx.esi);X86_PUSH32(frame+0x128);X86_PUSH32(1);
    X86_PUSH32(scope::selected_caller);sub_001B5F20(ctx,base);assert(ctx.esp==frame);
    ctx.esp=entry+4;
}
namespace d3d { gpu::State& State(){return scope::state;} std::recursive_mutex& Lock(){return scope::lock;} }
namespace mem {uint32_t VirtualFromPhysical(uint32_t data){return data;}}
void x86_icall(X86Context&ctx,uint8_t*,uint32_t target){
    assert(target==0x123456);assert(ctx.ecx==scope::record+0x20);
    scope::observed.push_back(scope::state.tire_fit);
    if(scope::fail)throw std::runtime_error("test original draw failure");
    ctx.esp+=28;
}
void UnitTests(){
    using namespace scope;using kernel::W32;Setup();
    Draw(0);assert(Active(observed.back())&&observed.back().source_ratio==.83f&&observed.back().target_ratio==.975f);
    assert(!Active(state.tire_fit));
    UnscopedTireCall(0);assert(!Active(observed.back()));
    selected_pass=4;Draw(0);assert(Active(observed.back()));selected_pass=0;
    Draw(1,0x109A9C);assert(Active(observed.back())&&observed.back().target_ratio==.95f);
    // Both axles deliberately share one nearest-stock handle; the retained loop index chooses the exact ratio.
    Draw(0,0x108081);assert(!Active(observed.back()));
    Draw(2);assert(!Active(observed.back()));
    fixture::memory[cfg+0x1C]=7;Draw(0);assert(!Active(observed.back()));fixture::memory[cfg+0x1C]=33;
    F(render+0x12D4,.01f);Draw(0);assert(!Active(observed.back()));F(render+0x12D4,23*.0254f/.975f);
    W32(resources+0xC4,handle+4);Draw(0);assert(!Active(observed.back()));W32(resources+0xC4,handle);
    W32(model+4,car+4);Draw(0);assert(!Active(observed.back()));W32(model+4,car);
    W32(geom+4,24u<<24);Draw(0);assert(!Active(observed.back()));W32(geom+4,28u<<24);
    kernel::W16(indices,3);Draw(0);assert(!Active(observed.back()));kernel::W16(indices,0);
    F(vertices+4,.2f);Draw(0);assert(!Active(observed.back()));F(vertices+4,.415f);
    const auto original=fixture::memory;
    fail=true;try{Draw(0);assert(false);}catch(const std::runtime_error&){}fail=false;
    assert(!Active(state.tire_fit));assert(std::equal(original.begin()+record,original.begin()+record+0x400,fixture::memory.begin()+record));
    // Concrete AI registration takes precedence over a shared cfg, render and tire resource.
    fixture::Call(sub_001FAE10,factory);Draw(0);assert(!Active(observed.back()));
}

namespace original_scope {
template<class T> void Read(std::istream& input,T& value){
    input.read(reinterpret_cast<char*>(&value),sizeof(value));assert(input.good());
}
void Run(const char* path){
    using namespace scope;using kernel::W32;
    Setup();
    std::ifstream input(path,std::ios::binary);assert(input);
    char magic[8];input.read(magic,8);assert(std::memcmp(magic,"MC3TSCP1",8)==0);
    std::array<float,8> stock{};std::array<float,34> requests{};
    Read(input,stock);Read(input,requests);assert(stock==ratios);
    uint32_t models=0;Read(input,models);assert(models==88);
    auto ResourceHandle=[](unsigned profile,unsigned brand){return handle+16*(profile*11+brand);};
    // Distinct nearest-profile/brand wrappers ensure an incorrect selection cannot pass merely because
    // the simplified unit fixture points every database entry at one shared test handle.
    for(unsigned profile=0;profile<8;++profile){
        const uint32_t desc=descriptor+16*profile, list=items+0x100*profile;
        W32(table+4*profile,desc);W32(desc,list);W32(desc+4,11);
        for(unsigned brand=0;brand<11;++brand){
            const uint32_t resource=ResourceHandle(profile,brand);
            W32(list+4*brand,resource);W32(resource+4,record);
        }
    }
    std::array<std::array<unsigned,11>,8> coverage{};
    uint64_t extended_draws=0,stock_draws=0,wrong_brand_checks=0;
    for(unsigned model_index=0;model_index<models;++model_index){
        uint32_t brand=0,profile=0,bytes=0,relocations=0;
        Read(input,brand);Read(input,profile);Read(input,bytes);Read(input,relocations);
        assert(brand<11&&profile<8&&bytes==8752&&relocations>0&&relocations<=128);
        coverage[profile][brand]++;
        std::vector<std::array<uint32_t,2>> pointers(relocations);
        for(auto& pair:pointers)Read(input,pair);
        std::array<uint32_t,2> vertex_offsets{};Read(input,vertex_offsets);
        std::vector<uint8_t> original(bytes);
        input.read(reinterpret_cast<char*>(original.data()),bytes);assert(input.good());
        assert(record+bytes<0x230000&&record+bytes<fixture::memory.size());
        std::memcpy(fixture::memory.data()+record,original.data(),bytes);
        // Relocate only the explicit pointer-bearing resource fields the bounded original pool decoder found.
        // Never scan and replace plausible bit patterns inside positions, normals, UVs or color values.
        for(const auto& pair:pointers){
            assert(pair[0]>=0x20&&pair[0]+4<=bytes&&pair[1]>=0x20&&pair[1]<bytes);
            W32(record+pair[0],record+pair[1]);
        }
        // Only the real original dispatch's unrelated final virtual shader body is a narrow recorder.
        W32(record+0x20,0x230000);W32(0x230018,0x123456);
        selected_handle=ResourceHandle(profile,brand);
        W32(resources+0xC4,selected_handle);W32(resources+0xC8,selected_handle);
        const std::vector<uint8_t> rebased(fixture::memory.begin()+record,fixture::memory.begin()+record+bytes);
        const std::array<uint32_t,4> expected{record+vertex_offsets[0],record+vertex_offsets[1],0,0};
        assert(expected[0]&&expected[1]&&expected[0]!=expected[1]);
        W32(render+0x11B0,brand);
        for(unsigned request=0;request<requests.size();++request){
            unsigned nearest=request;
            if(request>=8){
                nearest=0;
                for(unsigned i=1;i<8;++i)
                    if(std::fabs(requests[request]-stock[i])<std::fabs(requests[request]-stock[nearest]))nearest=i;
            }
            if(nearest!=profile)continue;
            for(unsigned axle=0;axle<2;++axle){
                fixture::memory[cfg+0x1C+axle]=uint8_t(request);
                F(render+(axle?0x12CC:0x12D4),23*.0254f/requests[request]);
                // Exercise full saved/render codes and the checked nearest-code allocation-failure fallback.
                for(unsigned fallback=0;fallback<2;++fallback){
                    W32(render+0x11B4+4*axle,fallback?nearest:request);
                    float source=0,target=0;
                    const bool allowed=GarageTireFitForDraw(render,resources,selected_handle,axle,source,target);
                    assert(allowed==(request>=8));
                    if(allowed){
                        assert(source==stock[profile]&&target==requests[request]);
                        const uint32_t wrong=ResourceHandle(profile,(brand+1)%11);
                        W32(resources+0xC4+4*axle,wrong);
                        assert(!GarageTireFitForDraw(render,resources,wrong,axle,source,target));
                        W32(resources+0xC4+4*axle,selected_handle);++wrong_brand_checks;
                    }
                    for(uint32_t caller:{0x1080C5u,0x109A9Cu}){
                        Draw(axle,caller);
                        const auto& fit=observed.back();
                        if(request>=8){
                            const bool matches=fit.vertex_data==expected&&fit.source_ratio==stock[profile]&&fit.target_ratio==requests[request];
                            if(!matches)std::cerr<<"Original tire rejected: model "<<model_index<<", brand "<<brand
                                <<", stock "<<profile<<", requested "<<request<<", axle "<<axle
                                <<", caller "<<std::hex<<caller<<std::dec<<", fallback "<<fallback<<std::endl;
                            assert(matches);
                            ++extended_draws;
                        }else{assert((fit.vertex_data==std::array<uint32_t,4>{}));++stock_draws;}
                        assert((state.tire_fit.vertex_data==std::array<uint32_t,4>{}));
                        assert(std::equal(rebased.begin(),rebased.end(),fixture::memory.begin()+record));
                    }
                }
            }
        }
    }
    for(const auto& row:coverage)for(unsigned count:row)assert(count==1);
    assert(input.peek()==std::char_traits<char>::eof());
    assert(extended_draws==2288&&stock_draws==704&&wrong_brand_checks==1144);
    std::cout<<"Original tire scope: 88 records / 176 draw geometries; "<<extended_draws
             <<" extended draws, "<<stock_draws<<" stock negative controls, "<<wrong_brand_checks
             <<" wrong-brand guards passed; guest resource bytes unchanged\n";
}
}
int main(int argc,char**argv){
    UnitTests();
    if(argc==3&&std::strcmp(argv[1],"--original")==0)original_scope::Run(argv[2]);
    else assert(argc==1);
}
