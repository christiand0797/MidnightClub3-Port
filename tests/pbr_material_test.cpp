#include "gpu/pbr_material.h"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincodec.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <cassert>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iostream>
#include <sstream>
#include <tuple>

using namespace gpu::pbr;
using Microsoft::WRL::ComPtr;
namespace fs=std::filesystem;
namespace {
constexpr float pi=3.14159265358979323846f;
void Near(float a,float b,float tolerance=0.00001f){assert(std::isfinite(a)&&std::abs(a-b)<tolerance);}
std::vector<uint8_t> Read(const fs::path& path){std::ifstream f(path,std::ios::binary);return {std::istreambuf_iterator<char>(f),{}};}
void Write(const fs::path& path,const std::string& text){std::ofstream f(path,std::ios::binary);f<<text;assert(bool(f));}
void Png(const fs::path& file,const Mip& image,bool sixteen=false){
    ComPtr<IWICImagingFactory> wic;ComPtr<IWICStream> stream;ComPtr<IWICBitmapEncoder> encoder;ComPtr<IWICBitmapFrameEncode> frame;
    assert(SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&wic))));
    assert(SUCCEEDED(wic->CreateStream(&stream))&&SUCCEEDED(stream->InitializeFromFilename(file.c_str(),GENERIC_WRITE)));
    assert(SUCCEEDED(wic->CreateEncoder(GUID_ContainerFormatPng,nullptr,&encoder))&&SUCCEEDED(encoder->Initialize(stream.Get(),WICBitmapEncoderNoCache)));
    assert(SUCCEEDED(encoder->CreateNewFrame(&frame,nullptr))&&SUCCEEDED(frame->Initialize(nullptr))&&SUCCEEDED(frame->SetSize(image.width,image.height)));
    // The Windows PNG encoder natively accepts BGRA; preserve RGBA fixture meaning while writing it.
    GUID format=sixteen?GUID_WICPixelFormat16bppGray:GUID_WICPixelFormat32bppBGRA;const GUID expected=format;
    assert(SUCCEEDED(frame->SetPixelFormat(&format))&&format==expected);
    auto encoded_pixels = image.pixels;
    if (!sixteen) for (size_t p=0;p<encoded_pixels.size();p+=4) std::swap(encoded_pixels[p],encoded_pixels[p+2]);
    assert(SUCCEEDED(frame->WritePixels(image.height,image.width*(sixteen?2:4),UINT(encoded_pixels.size()),encoded_pixels.data())));
    assert(SUCCEEDED(frame->Commit())&&SUCCEEDED(encoder->Commit()));
}
struct Fixture {
    fs::path root;
    Mip color{2,2,{0,0,0,0,255,255,255,128,0,0,0,255,255,255,255,64}};
    Mip normal{2,2,{128,128,255,255,128,128,255,255,128,128,255,255,128,128,255,255}};
    Mip orm{2,2,{255,0,0,255,255,255,0,255,255,0,0,255,255,255,0,255}};
    std::string record;
    Fixture(){
        root=fs::temp_directory_path()/("mc3-pbr-fixture-"+std::to_string(GetCurrentProcessId())+"-"+
              std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        assert(!fs::exists(root)&&fs::create_directory(root));root=fs::canonical(root);
        Png(root/"albedo.png",color);Png(root/"normal.png",normal);Png(root/"orm.png",orm);
        record="[[materials]]\ntexture='0123456789ABCDEF'\nvertex_shader='1111111111111111'\npixel_shader='2222222222222222'\n"
               "stage=0\nuv_set=0\nsurface='world'\ncutout=false\nnormal_convention='opengl'\nreview_sha256='"+std::string(64,'a')+
               "'\nbaseline_alpha_sha256='"+AlphaDigest(color)+"'\n";
        for(auto [name,file,space]:{std::tuple{"albedo","albedo.png","srgb"},std::tuple{"normal","normal.png","linear"},std::tuple{"orm","orm.png","linear"}})
            record+="[materials."+std::string(name)+"]\nfile='"+file+"'\nsha256='"+Sha256(Read(root/file))+"'\ncolor_space='"+space+"'\n";
    }
    ~Fixture(){
        // Delete only this uniquely created, verified Temp child, never a computed project/user directory.
        const auto parent=fs::canonical(fs::temp_directory_path());
        assert(root.is_absolute()&&root.parent_path()==parent&&root.filename().string().starts_with("mc3-pbr-fixture-"));
        fs::remove_all(root);
    }
    LoadPolicy Policy(const std::string& records=""){
        Write(root/"materials-v1.toml","schema=1\n"+(records.empty()?record:records));
        LoadPolicy p;p.enabled=true;p.approved_manifest_sha256=Sha256(Read(root/"materials-v1.toml"));return p;
    }
};
void Replace(std::string& s,const std::string& from,const std::string& to){auto p=s.find(from);assert(p!=std::string::npos);s.replace(p,from.size(),to);}
void Rejected(Fixture& f,const std::string& record){auto library=LoadReviewedManifest(f.root,f.Policy(record));assert(library.status==LoadStatus::Rejected&&library.materials.empty()&&!library.errors.empty());}
}

int main(){
    assert(SUCCEEDED(CoInitializeEx(nullptr,COINIT_MULTITHREADED)));
    assert(Sha256({'a','b','c'})=="ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    assert(LoadReviewedManifest(L"Z:\\does-not-exist",{}).status==LoadStatus::Disabled);
    LoadPolicy untrusted;untrusted.enabled=true;
    auto no_trust=LoadReviewedManifest(L"Z:\\does-not-exist",untrusted);
    assert(no_trust.status==LoadStatus::Rejected&&no_trust.errors.front().find("approved")!=std::string::npos);
    Fixture f;auto policy=f.Policy();auto library=LoadReviewedManifest(f.root,policy);
    if(library.status!=LoadStatus::Ready)for(auto& error:library.errors)std::cerr<<error<<'\n';
    assert(library.status==LoadStatus::Ready&&library.materials.size()==1&&library.errors.empty());
    const auto& material=library.materials.front();
    assert(material.albedo.levels.size()==2&&material.normal.levels.size()==2&&material.orm.levels.size()==2);
    assert(material.albedo.levels[0].pixels==f.color.pixels); // Original level-0 RGBA/alpha bytes retained.
    assert(material.albedo.levels[1].pixels[0]>=187&&material.albedo.levels[1].pixels[0]<=188); // Linear-light colour filter.
    assert(material.orm.levels[1].pixels[1]==180); // sqrt(mean(perceptual_roughness^2)), no sRGB conversion.
    assert(material.normal.levels[1].pixels[2]==255);
    auto draw=SurfaceEvidence{};draw.assignment=material.assignment;draw.texture_2d=true;
    draw.color_width=2;draw.color_height=2;draw.current_alpha_sha256=AlphaDigest(f.color);
    draw.geometric_position=draw.geometric_normal=draw.tangent_basis=draw.linear_lighting=true;
    assert(Select(library,draw).material==&material);
    auto bad=draw;bad.assignment.vertex_shader++;assert(Select(library,bad).reason==SelectionReason::NoAssignment);
    for(auto flag:{&SurfaceEvidence::pretransformed,&SurfaceEvidence::screen_space,&SurfaceEvidence::dynamic_texture,
                  &SurfaceEvidence::render_target,&SurfaceEvidence::depth_texture,&SurfaceEvidence::additive,&SurfaceEvidence::blended}){
        bad=draw;bad.*flag=true;assert(Select(library,bad).reason==SelectionReason::UnsupportedDraw);
    }
    bad=draw;bad.geometric_position=false;assert(Select(library,bad).reason==SelectionReason::MissingGeometry);
    bad=draw;bad.linear_lighting=false;assert(Select(library,bad).reason==SelectionReason::MissingLinearLighting);
    bad=draw;bad.current_alpha_sha256=std::string(64,'0');assert(Select(library,bad).reason==SelectionReason::AlphaMismatch);
    bad=draw;bad.color_width=4;assert(Select(library,bad).reason==SelectionReason::AlphaMismatch);
    auto wrong_digest=policy;wrong_digest.approved_manifest_sha256=std::string(64,'0');
    assert(LoadReviewedManifest(f.root,wrong_digest).status==LoadStatus::Rejected);
    for(const std::string path:{"../albedo.png","/albedo.png","C:/albedo.png","x:albedo.png","a\\albedo.png","./albedo.png","http://server/image.png"}){
        auto text=f.record;Replace(text,"file='albedo.png'","file='"+path+"'");Rejected(f,text);
    }
    auto text=f.record;Replace(text,"surface='world'","surface='ui'");Rejected(f,text);
    text=f.record;Replace(text,"color_space='srgb'","color_space='linear'");Rejected(f,text);
    text=f.record;Replace(text,"stage=0","stage=4");Rejected(f,text);
    text=f.record;Replace(text,"baseline_alpha_sha256='"+AlphaDigest(f.color)+"'","baseline_alpha_sha256='"+std::string(64,'0')+"'");Rejected(f,text);
    Rejected(f,f.record+f.record);Rejected(f,"unknown=1\n"+f.record);
    // A city material: texture alone ("any" shaders and UV set) with the game's own albedo, no albedo file.
    {
        std::string city="[[materials]]\ntexture='0123456789ABCDEF'\nvertex_shader='any'\npixel_shader='any'\nstage=0\n"
            "uv_set='any'\nsurface='world'\ncutout=false\nnormal_convention='opengl'\nreview_sha256='"+std::string(64,'a')+
            "'\nbaseline_alpha_sha256='"+std::string(64,'b')+"'\nalbedo='game'\n";
        for(auto [name,file]:{std::pair{"normal","normal.png"},std::pair{"orm","orm.png"}})
            city+="[materials."+std::string(name)+"]\nfile='"+file+"'\nsha256='"+Sha256(Read(f.root/file))+"'\ncolor_space='linear'\n";
        auto cities=LoadReviewedManifest(f.root,f.Policy(city));
        if(cities.status!=LoadStatus::Ready)for(auto& error:cities.errors)std::cerr<<error<<'\n';
        assert(cities.status==LoadStatus::Ready&&cities.materials.size()==1);
        const auto& m=cities.materials.front();
        assert(m.game_albedo&&m.albedo.levels.empty()&&m.assignment.vertex_shader==kAnyShader&&m.assignment.pixel_shader==kAnyShader&&
               m.assignment.uv_set==kAnyUv&&cities.by_texture.at(0x0123456789ABCDEFull)==std::vector<uint32_t>{0});
        auto any=draw;any.assignment=m.assignment;any.color_width=999;any.current_alpha_sha256=std::string(64,'0');
        assert(Select(cities,any).material==&m); // nothing to match against: the game's own texture is the albedo
        auto other=city;Replace(other,"vertex_shader='any'","vertex_shader='anything'");Rejected(f,other);
        other=city;Replace(other,"albedo='game'","albedo='mine'");Rejected(f,other);
        Rejected(f,city+city); // duplicates still collide through the texture index
    }
    assert(library.by_texture.at(0x0123456789ABCDEFull)==std::vector<uint32_t>{0});
    auto budget=f.Policy();budget.max_decoded_bytes=20;assert(LoadReviewedManifest(f.root,budget).status==LoadStatus::Rejected);
    budget=f.Policy();budget.max_dimension=1;assert(LoadReviewedManifest(f.root,budget).status==LoadStatus::Rejected);
    policy=f.Policy();Write(f.root/"albedo.png","corrupt");assert(LoadReviewedManifest(f.root,policy).status==LoadStatus::Rejected);
    Png(f.root/"albedo.png",f.color);
    // A digest-pinned corrupt image also fails decode; the digest is not an image-validity substitute.
    text=f.record;Write(f.root/"invalid.png","not PNG");Replace(text,"file='albedo.png'","file='invalid.png'");
    Replace(text,Sha256(Read(f.root/"albedo.png")),Sha256(Read(f.root/"invalid.png")));Rejected(f,text);
    Mip height{2,2,{0,0,255,255,0,128,1,0}};Png(f.root/"height.png",height,true);
    text=f.record+"[materials.height]\nfile='height.png'\nsha256='"+Sha256(Read(f.root/"height.png"))+"'\ncolor_space='linear'\n"
         "[materials.emission]\nfile='albedo.png'\nsha256='"+Sha256(Read(f.root/"albedo.png"))+"'\ncolor_space='srgb'\n";
    auto all=LoadReviewedManifest(f.root,f.Policy(text));assert(all.status==LoadStatus::Ready);
    assert(all.materials[0].has_height&&all.materials[0].has_emission);
    assert(all.materials[0].height.storage==Storage::R16Unorm&&all.materials[0].height.levels[0].pixels==height.pixels);

    Near(SrgbToLinear(0.5f),0.21404114f);Near(LinearToSrgb(SrgbToLinear(0.5f)),0.5f);
    Near(FresnelSchlick({0.04f,0.2f,0.8f},1).x,0.04f);Near(FresnelSchlick({0.04f,0.2f,0.8f},0).x,1);
    Near(DistributionGgx(1,1),1/pi);Near(VisibilitySmith(1,1,1),0.25f);
    auto dielectric=EvaluateDirect({1,1,1},0,1,{0,0,1},{0,0,1},{0,0,1},{1,1,1});Near(dielectric.x,0.97f/pi);
    auto metal=EvaluateDirect({0.8f,0.4f,0.2f},1,1,{0,0,1},{0,0,1},{0,0,1},{1,1,1});Near(metal.x,0.8f/(4*pi));
    assert(EvaluateDirect({1,1,1},0,0,{0,0,1},{0,0,1},{0,0,-1},{1,1,1}).x==0);
    assert(EvaluateDirect({1,1,1},0,0,{},{0,0,1},{0,0,1},{1,1,1}).x==0);
    auto zero_normal=TangentNormal({0.5f,0.5f,1},1,NormalConvention::OpenGl,{},{},{});
    assert(zero_normal.x==0 && zero_normal.y==0 && zero_normal.z==0);
    bool bad_alpha_rejected=false;
    try { AlphaDigest(Mip{UINT32_MAX,UINT32_MAX,{}}); } catch(const std::exception&) { bad_alpha_rejected=true; }
    assert(bad_alpha_rejected);
    for(float roughness:{0.0f,0.1f,0.5f,1.0f})for(float cosine:{0.000001f,0.01f,0.5f,1.0f}){
        assert(std::isfinite(DistributionGgx(cosine,roughness)));assert(std::isfinite(VisibilitySmith(cosine,cosine,roughness)));
    }
    // Deterministic white-furnace integration at resolvable lobe widths: no direct-energy gain.
    for(float roughness:{0.3f,0.6f,1.0f})for(float metallic:{0.0f,1.0f}){
        double energy=0;constexpr unsigned polar=256,azimuth=32;
        for(unsigned i=0;i<polar;++i)for(unsigned j=0;j<azimuth;++j){
            float z=(i+0.5f)/polar,phi=2*pi*(j+0.5f)/azimuth,r=std::sqrt(1-z*z);
            energy+=EvaluateDirect({1,1,1},metallic,roughness,{0,0,1},{0,0,1},{r*std::cos(phi),r*std::sin(phi),z},{1,1,1}).x;
        }
        energy*=2*pi/(polar*azimuth);assert(energy>=0&&energy<=1.005);
    }
    assert(EvaluateIndirectDiffuse({1,1,1},0,0,{1,1,1}).x==0);
    auto up=TangentNormal({0.5f,0.75f,1},1,NormalConvention::OpenGl,{1,0,0},{0,1,0},{0,0,1});
    auto down=TangentNormal({0.5f,0.75f,1},1,NormalConvention::DirectX,{1,0,0},{0,1,0},{0,0,1});assert(up.y<0&&down.y>0);Near(up.y,-down.y);

    // Pure CPU shader compilation: proves actual HLSL is usable without launching a device/game/GPU worker.
    const auto shader=std::string(BrdfHlsl())+R"(
float4 main(float4 p:SV_Position):SV_Target {
 float3 n=Mc3PbrTangentNormal(float3(0.5,0.5,1),1,true,float3(1,0,0),float3(0,1,0),float3(0,0,1));
 float3 invalidNormal=Mc3PbrTangentNormal(float3(0.5,0.5,1),1,true,0.0.xxx,0.0.xxx,0.0.xxx);
 float3 c=Mc3PbrDirect(float3(Mc3PbrSrgbToLinear(p.x*0.001),0.2,0.5),0,0.5,n,float3(0,0,1),float3(0,0,1),1.0.xxx);
 return float4(c+Mc3PbrIndirectDiffuse(0.5.xxx,0,1,1.0.xxx)+invalidNormal,0.37);
})";
    ComPtr<ID3DBlob> compiled,errors;
    const auto hr=D3DCompile(shader.data(),shader.size(),"pbr-brdf-cpu-test",nullptr,nullptr,"main","ps_5_0",D3DCOMPILE_ENABLE_STRICTNESS,0,&compiled,&errors);
    if(FAILED(hr)&&errors)std::cerr<<static_cast<const char*>(errors->GetBufferPointer());
    assert(SUCCEEDED(hr)&&compiled&&compiled->GetBufferSize());
    CoUninitialize();
    std::cout<<"pbr_material: approved manifest/PNG/alpha/binding guards, linear mips, CPU BRDF and HLSL compilation passed\n";
}
