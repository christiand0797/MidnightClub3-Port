#include "gpu/radial_fit.h"

#include <array>
#include <cassert>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <vector>

namespace {
using gpu::radial_fit::Vertex;
constexpr float epsilon = 2.e-6f;

uint32_t Pack(double x, double y, double z) {
    auto quantize = [](double v, int scale, unsigned bits) {
        return uint32_t(std::lround(v * scale)) & ((1u << bits) - 1);
    };
    return quantize(x,1023,11) | quantize(y,1023,11)<<11 | quantize(z,511,10)<<22;
}
std::array<double,3> Normal(uint32_t value) {
    return {std::max(-1.,gpu::radial_fit::Signed(value,11)/1023.),
            std::max(-1.,gpu::radial_fit::Signed(value>>11,11)/1023.),
            std::max(-1.,gpu::radial_fit::Signed(value>>22,10)/511.)};
}
double Radius(const float* p) { return std::hypot(double(p[1]),double(p[2])); }
void Near(double a, double b, double e = epsilon) { assert(std::abs(a-b) <= e); }

void UnitTests() {
    // Independently known endpoints: a 60% mesh bead is at r=.3. The 90% request needs r=.45,
    // while the outside remains r=.5 and axle position never moves.
    float bead[]{-.25f,.3f,0}; uint32_t normal=Pack(0,1,0);
    assert(Vertex(bead,normal,.6f,.9f)); Near(bead[0],-.25); Near(bead[1],.45); Near(bead[2],0);
    const auto repaired=Normal(normal); Near(repaired[0],0); Near(repaired[1],1); Near(repaired[2],0);
    float midpoint[]{.17f,0,.4f}; normal=Pack(0,0,1);
    assert(Vertex(midpoint,normal,.6f,.9f)); Near(midpoint[0],.17); Near(midpoint[2],.475);
    float smaller[]{.25f,0,.45f}; normal=Pack(0,0,1);
    assert(Vertex(smaller,normal,.9f,.6f)); Near(smaller[2],.3);

    // A general shoulder normal must stay perpendicular to both transformed surface tangents. The
    // point map is differentiated numerically, independently of the helper's normal calculation.
    constexpr double root = .7071067811865475244;
    const std::array<float,3> original{.1f,float(.4*root),float(.4*root)};
    uint32_t original_normal=Pack(root,root,0), transformed_normal=original_normal;
    float shoulder[]{original[0],original[1],original[2]};
    assert(Vertex(shoulder,transformed_normal,.6f,.9f));
    auto n=Normal(original_normal); auto out=Normal(transformed_normal);
    const std::array<std::array<double,3>,2> tangents{{{0,0,1},{n[1],-n[0],0}}};
    for (const auto& tangent:tangents) {
        constexpr double h=1.e-4;
        float low[3],high[3];
        for (int k=0;k<3;k++) { low[k]=float(original[k]-h*tangent[k]); high[k]=float(original[k]+h*tangent[k]); }
        auto nl=original_normal,nh=original_normal;
        assert(Vertex(low,nl,.6f,.9f) && Vertex(high,nh,.6f,.9f));
        std::array<double,3> derivative{};
        for(int k=0;k<3;k++) derivative[k]=(high[k]-low[k])/(2*h);
        const double dot=out[0]*derivative[0]+out[1]*derivative[1]+out[2]*derivative[2];
        const double length=std::hypot(derivative[0],derivative[1],derivative[2]);
        assert(std::abs(dot)/length < .004);
    }

    // No new quantization for exact identity. Degenerate or nonfinite candidates are rejected,
    // and positions/normals remain untouched whenever a candidate cannot be represented.
    for (float radius:{.3f,.5f,.50022f}) {
        float p[]{-.17f,radius,-0.f},saved[3];std::memcpy(saved,p,sizeof(p));normal=0x800003ffu;
        const auto old=normal; assert(Vertex(p,normal,.6f,.6f));
        assert(std::memcmp(saved,p,sizeof(p))==0 && normal==old);
    }
    for (float radius:{.5f,.50022f}) {
        float p[]{.25f,radius,0},saved[3];std::memcpy(saved,p,sizeof(p));normal=Pack(.6,.8,0);
        const auto old=normal;assert(Vertex(p,normal,.6f,.975f));
        assert(std::memcmp(saved,p,sizeof(p))==0 && normal==old);
    }
    for (float invalid:{0.f,-.1f,1.f,std::numeric_limits<float>::infinity(),std::numeric_limits<float>::quiet_NaN()}) {
        float p[]{.25f,.4f,0},saved[3];std::memcpy(saved,p,sizeof(p));normal=Pack(0,1,0);const auto old=normal;
        assert(!Vertex(p,normal,invalid,.9f));assert(std::memcmp(saved,p,sizeof(p))==0 && normal==old);
        assert(!Vertex(p,normal,.6f,invalid));assert(std::memcmp(saved,p,sizeof(p))==0 && normal==old);
    }
    for (const std::array<float,3> invalid:{std::array<float,3>{0,0,0}, {0,.01f,0},
            {std::numeric_limits<float>::quiet_NaN(),.4f,0}, {0,std::numeric_limits<float>::infinity(),0}}) {
        float p[3];std::memcpy(p,invalid.data(),sizeof(p));float saved[3];std::memcpy(saved,p,sizeof(p));
        normal=Pack(0,1,0);const auto old=normal;assert(!Vertex(p,normal,.83f,.3f));
        assert(std::memcmp(saved,p,sizeof(p))==0 && normal==old);
    }
    float p[]{0,.4f,0},saved[3];std::memcpy(saved,p,sizeof(p));normal=0;
    assert(!Vertex(p,normal,.6f,.9f));assert(std::memcmp(saved,p,sizeof(p))==0 && normal==0);
    assert(!Vertex(nullptr,normal,.6f,.9f));
    assert(gpu::radial_fit::Signed(0x7ff,11)==-1 && gpu::radial_fit::Signed(0x400,11)==-1024);
    assert(gpu::radial_fit::Signed(0x3ff,10)==-1 && gpu::radial_fit::Signed(0x200,10)==-512);
    std::cout << "Radial-fit endpoint, inverse-normal, identity and atomic rejection checks passed\n";
}

template<class T> void Read(std::istream& stream,T& value) {
    stream.read(reinterpret_cast<char*>(&value),sizeof(value));assert(stream.good());
}
void OriginalTests(const char* path) {
    std::ifstream stream(path,std::ios::binary);assert(stream);
    char magic[8];stream.read(magic,8);assert(std::memcmp(magic,"MC3RFIT1",8)==0);
    std::array<float,8> stock{};std::array<float,34> target{};Read(stream,stock);Read(stream,target);
    uint32_t count=0;Read(stream,count);assert(count==176);
    std::array<std::array<unsigned,2>,8> coverage{};
    uint64_t checked=0,stock_checked=0,extended_checked=0;
    double worst_bead_error=0;
    for(uint32_t geometry=0;geometry<count;geometry++) {
        uint32_t source=0,slot=0,vertices=0;Read(stream,source);Read(stream,slot);Read(stream,vertices);
        assert(source<8 && slot<2 && (vertices==180 || vertices==60));coverage[source][slot]++;
        std::vector<std::array<uint8_t,28>> raw(vertices);
        stream.read(reinterpret_cast<char*>(raw.data()),raw.size()*28);assert(stream.good());
        for(unsigned request=0;request<target.size();request++) {
            unsigned nearest=request;
            if(request>=8) {
                nearest=0;
                for(unsigned i=1;i<8;i++) if(std::abs(target[request]-stock[i])<std::abs(target[request]-stock[nearest])) nearest=i;
            }
            if(nearest!=source) continue;
            double bead_before=1,bead_after=1;
            for(const auto& vertex:raw) {
                auto output=vertex;float position[3];uint32_t normal;
                std::memcpy(position,vertex.data(),12);std::memcpy(&normal,vertex.data()+12,4);
                const double before=Radius(position);bead_before=std::min(bead_before,before);
                assert(Vertex(position,normal,stock[source],target[request]));
                std::memcpy(output.data(),position,12);std::memcpy(output.data()+12,&normal,4);
                const double after=Radius(position);bead_after=std::min(bead_after,after);
                assert(std::memcmp(vertex.data(),output.data(),4)==0); // axle coordinate
                assert(std::memcmp(vertex.data()+16,output.data()+16,12)==0); // color and both UVs
                if(request<8 || before>=.5) assert(vertex==output);
                else {
                    assert(after>0 && after<.500001);
                    auto n=Normal(normal);Near(std::hypot(n[0],n[1],n[2]),1,.003);
                }
                checked++;if(request<8)stock_checked++;else extended_checked++;
            }
            assert(std::abs(bead_before-stock[source]*.5)<1.e-5);
            const double bead_error=std::abs(bead_after-target[request]*.5);
            worst_bead_error=std::max(worst_bead_error,bead_error);assert(bead_error<2.e-5);
        }
    }
    assert(stream.peek()==std::char_traits<char>::eof());
    for(const auto& profile:coverage) for(unsigned brands:profile) assert(brands==11);
    assert(checked==89760 && stock_checked==21120 && extended_checked==68640);
    std::cout << "Original assets: 176 geometries, " << checked << " vertex cases (" << stock_checked
              << " stock identity, " << extended_checked << " extended), worst normalized bead error "
              << worst_bead_error << '\n';
}
}
int main(int argc,char** argv) {
    UnitTests();
    if(argc==3 && std::strcmp(argv[1],"--original")==0) OriginalTests(argv[2]);
    else assert(argc==1);
}
