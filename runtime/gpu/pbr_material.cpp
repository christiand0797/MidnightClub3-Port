#include "gpu/pbr_material.h"
#include "gpu/pbr_brdf_shader.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <toml++/toml.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <set>
#include <stdexcept>
#include <tuple>

namespace gpu::pbr {
namespace {
using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;
constexpr float kPi = 3.14159265358979323846f;
void Require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
float Unit(float x) { return std::isfinite(x) ? std::clamp(x,0.0f,1.0f) : 0; }
Vec3 Unit(Vec3 v) { return {Unit(v.x),Unit(v.y),Unit(v.z)}; }
Vec3 Add(Vec3 a, Vec3 b) { return {a.x+b.x,a.y+b.y,a.z+b.z}; }
Vec3 Mul(Vec3 a, float b) { return {a.x*b,a.y*b,a.z*b}; }
Vec3 Mul(Vec3 a, Vec3 b) { return {a.x*b.x,a.y*b.y,a.z*b.z}; }
float Dot(Vec3 a, Vec3 b) { return a.x*b.x+a.y*b.y+a.z*b.z; }
Vec3 Normalize(Vec3 v) {
    const float length2=Dot(v,v);
    return std::isfinite(length2) && length2>1e-12f ? Mul(v,1/std::sqrt(length2)) : Vec3{};
}
uint8_t Byte(float x) { return uint8_t(std::lround(Unit(x)*255)); }
bool Hex(std::string_view text, size_t length) {
    return text.size()==length && std::all_of(text.begin(),text.end(),[](char c) {
        return (c>='0'&&c<='9')||(c>='a'&&c<='f')||(c>='A'&&c<='F');
    });
}
std::string Lower(std::string text) {
    for (char& c:text) if(c>='A'&&c<='Z') c=char(c-'A'+'a');
    return text;
}
uint64_t HashKey(const std::string& text,bool any_allowed=false) {
    if(any_allowed&&text=="any")return kAnyShader;
    Require(Hex(text,16),"Texture/shader identity must be exactly sixteen hexadecimal digits");
    uint64_t value=0;
    for(char c:Lower(text)) value=(value<<4)|uint64_t(c<='9'?c-'0':c-'a'+10);
    Require(value!=0,"Zero is an unknown texture/shader identity");
    return value;
}
void Keys(const toml::table& table,std::initializer_list<std::string_view> allowed) {
    for(const auto& [key,value]:table) {
        (void)value;
        Require(std::find(allowed.begin(),allowed.end(),key.str())!=allowed.end(),"Unknown material manifest field");
    }
}
std::string Text(const toml::table& table,std::string_view key) {
    auto value=table[key].value<std::string>();
    Require(value.has_value(),"Missing/string-typed material manifest field");
    return *value;
}
uint32_t Number(const toml::table& table,std::string_view key,uint32_t max) {
    auto value=table[key].value<int64_t>();
    Require(value && *value>=0 && uint64_t(*value)<=max,"Missing/out-of-range integer material field");
    return uint32_t(*value);
}
bool Flag(const toml::table& table,std::string_view key) {
    auto value=table[key].value<bool>();
    Require(value.has_value(),"Missing/bool-typed material field");
    return *value;
}

// No URLs, absolute/device/drive paths, dot components, backslashes, alternate streams or reparse points.
fs::path SafeFile(const fs::path& root,const std::string& name) {
    Require(!name.empty() && name.size()<240 && name.front()!='/' && name.back()!='/',"Invalid material path");
    for(char c:name) Require((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||
                            c=='_'||c=='-'||c=='.'||c=='/',"Material paths must be plain relative ASCII paths");
    fs::path candidate=root;
    for(const auto& part:fs::path(name)) {
        const auto component=part.string();
        Require(component!="." && component!=".." && component.back()!='.',"Dot path components are forbidden");
        candidate/=part;
        const auto attributes=GetFileAttributesW(candidate.c_str());
        Require(attributes!=INVALID_FILE_ATTRIBUTES && !(attributes&FILE_ATTRIBUTE_REPARSE_POINT),"Missing/reparse material path");
    }
    std::error_code ec;
    const auto resolved=fs::canonical(candidate,ec);
    Require(!ec && fs::is_regular_file(resolved),"Material asset is not a regular file");
    const auto relative=resolved.lexically_relative(root);
    Require(!relative.empty() && !relative.is_absolute() && *relative.begin()!=fs::path(".."),"Material asset escaped its root");
    return resolved;
}
std::vector<uint8_t> Read(const fs::path& file,uint64_t limit) {
    std::error_code ec;const auto size=fs::file_size(file,ec);
    Require(!ec && size && size<=limit && size<=uint64_t(std::numeric_limits<uint32_t>::max()),"Material file exceeds its bounded read budget");
    std::ifstream stream(file,std::ios::binary);
    Require(bool(stream),"Cannot open material file");
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    stream.read(reinterpret_cast<char*>(bytes.data()),std::streamsize(bytes.size()));
    Require(stream.gcount()==std::streamsize(bytes.size()) && stream.peek()==std::char_traits<char>::eof(),"Material file changed during read");
    return bytes;
}

struct Apartment {
    HRESULT status=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
    Apartment() { Require(SUCCEEDED(status)||status==RPC_E_CHANGED_MODE,"COM unavailable for material decoding"); }
    ~Apartment() { if(SUCCEEDED(status)) CoUninitialize(); }
};
void Budget(uint64_t bytes,uint64_t& used,const LoadPolicy& policy) {
    Require(bytes<=policy.max_decoded_bytes && used<=policy.max_decoded_bytes-bytes,"Decoded material/mip budget exceeded");
    used+=bytes;
}

void Mips(Map& map,uint64_t& used,const LoadPolicy& policy) {
    const uint32_t stride=map.storage==Storage::R16Unorm?2:4;
    while(map.levels.back().width>1 || map.levels.back().height>1) {
        const auto& source=map.levels.back();
        Mip next;next.width=std::max(1u,source.width/2);next.height=std::max(1u,source.height/2);
        const uint64_t bytes=uint64_t(next.width)*next.height*stride;
        Budget(bytes,used,policy);next.pixels.resize(size_t(bytes));
        for(uint32_t y=0;y<next.height;++y) for(uint32_t x=0;x<next.width;++x) {
            const uint32_t x0=x*source.width/next.width,x1=(x+1)*source.width/next.width;
            const uint32_t y0=y*source.height/next.height,y1=(y+1)*source.height/next.height;
            const float count=float((x1-x0)*(y1-y0));
            float sums[4]={};Vec3 normal{};
            for(uint32_t sy=y0;sy<y1;++sy) for(uint32_t sx=x0;sx<x1;++sx) {
                const auto* pixel=&source.pixels[(size_t(sy)*source.width+sx)*stride];
                if(stride==2) sums[0]+=float(uint32_t(pixel[0])|(uint32_t(pixel[1])<<8));
                else if(map.role==MapRole::Normal) normal=Add(normal,Normalize({pixel[0]/127.5f-1,pixel[1]/127.5f-1,pixel[2]/127.5f-1}));
                else for(unsigned c=0;c<4;++c) {
                    const float value=pixel[c]/255.0f;
                    sums[c]+=map.color_space==ColorSpace::Srgb && c<3?SrgbToLinear(value):
                             map.role==MapRole::Orm && c==1?value*value:value;
                }
            }
            auto* destination=&next.pixels[(size_t(y)*next.width+x)*stride];
            if(stride==2) {
                const auto value=uint32_t(std::lround(sums[0]/count));destination[0]=uint8_t(value);destination[1]=uint8_t(value>>8);
            } else if(map.role==MapRole::Normal) {
                normal=Normalize(normal);destination[0]=Byte(normal.x*0.5f+0.5f);destination[1]=Byte(normal.y*0.5f+0.5f);
                destination[2]=Byte(normal.z*0.5f+0.5f);destination[3]=255;
            } else for(unsigned c=0;c<4;++c) {
                float value=sums[c]/count;
                if(map.color_space==ColorSpace::Srgb && c<3) value=LinearToSrgb(value);
                else if(map.role==MapRole::Orm && c==1) value=std::sqrt(value);
                destination[c]=Byte(value);
            }
        }
        map.levels.push_back(std::move(next));
    }
}

Map LoadMap(const toml::table& record,std::string_view key,MapRole role,const fs::path& root,
            const LoadPolicy& policy,IWICImagingFactory* wic,uint64_t& used) {
    const auto* entry=record[key].as_table();Require(entry,"Missing/table-typed material map");
    Keys(*entry,{"file","sha256","color_space"});
    Map map;map.role=role;map.file=Text(*entry,"file");map.sha256=Lower(Text(*entry,"sha256"));
    Require(Hex(map.sha256,64),"Invalid map SHA256");
    const bool color=role==MapRole::Albedo||role==MapRole::Emission;
    Require(Text(*entry,"color_space")==std::string(color?"srgb":"linear"),"Incorrect material channel color space");
    map.color_space=color?ColorSpace::Srgb:ColorSpace::Linear;
    map.storage=role==MapRole::Height?Storage::R16Unorm:Storage::Rgba8;
    Require(fs::path(map.file).extension()==".png","Only explicitly reviewed PNG maps are supported in schema1");
    auto encoded=Read(SafeFile(root,map.file),policy.max_encoded_file);
    Require(Sha256(encoded)==map.sha256,"Material map digest mismatch");
    ComPtr<IWICStream> stream;ComPtr<IWICBitmapDecoder> decoder;ComPtr<IWICBitmapFrameDecode> frame;
    Require(SUCCEEDED(wic->CreateStream(&stream)) &&
            SUCCEEDED(stream->InitializeFromMemory(encoded.data(),DWORD(encoded.size()))) &&
            SUCCEEDED(wic->CreateDecoderFromStream(stream.Get(),nullptr,WICDecodeMetadataCacheOnDemand,&decoder)),"Cannot decode material PNG");
    GUID container{};UINT frames=0;Mip first;
    Require(SUCCEEDED(decoder->GetContainerFormat(&container)) && container==GUID_ContainerFormatPng &&
            SUCCEEDED(decoder->GetFrameCount(&frames)) && frames==1 && SUCCEEDED(decoder->GetFrame(0,&frame)) &&
            SUCCEEDED(frame->GetSize(&first.width,&first.height)),"Invalid/multiframe material PNG");
    Require(first.width && first.height && first.width<=policy.max_dimension && first.height<=policy.max_dimension,"Invalid material map dimensions");
    const uint32_t stride=map.storage==Storage::R16Unorm?2:4;
    const uint64_t bytes=uint64_t(first.width)*first.height*stride;Budget(bytes,used,policy);
    Require(bytes<=std::numeric_limits<UINT>::max(),"Material map exceeds WIC size limit");
    first.pixels.resize(size_t(bytes));
    if(map.storage==Storage::R16Unorm) {
        GUID format{};Require(SUCCEEDED(frame->GetPixelFormat(&format)) && format==GUID_WICPixelFormat16bppGray,"Height must be an actual sixteen-bit grayscale PNG");
        Require(SUCCEEDED(frame->CopyPixels(nullptr,first.width*2,UINT(bytes),first.pixels.data())),"Height PNG pixel decode failed");
    } else {
        ComPtr<IWICFormatConverter> converter;
        Require(SUCCEEDED(wic->CreateFormatConverter(&converter)) &&
                SUCCEEDED(converter->Initialize(frame.Get(),GUID_WICPixelFormat32bppRGBA,WICBitmapDitherTypeNone,nullptr,0,WICBitmapPaletteTypeCustom)) &&
                SUCCEEDED(converter->CopyPixels(nullptr,first.width*4,UINT(bytes),first.pixels.data())),"Material PNG pixel decode failed");
        if(role==MapRole::Normal) for(size_t p=0;p<first.pixels.size();p+=4) {
            const float x=first.pixels[p]/127.5f-1,y=first.pixels[p+1]/127.5f-1,z=first.pixels[p+2]/127.5f-1;
            Require(z>0 && x*x+y*y+z*z>0.01f,"Normal map contains a zero/back-facing tangent normal");
        }
    }
    map.levels.push_back(std::move(first));Mips(map,used,policy);return map;
}
bool Same(const Assignment& a,const Assignment& b) {
    return a.original_texture==b.original_texture && a.vertex_shader==b.vertex_shader && a.pixel_shader==b.pixel_shader &&
           a.stage==b.stage && a.uv_set==b.uv_set && a.surface==b.surface && a.cutout==b.cutout;
}
} // namespace

std::string Sha256(const std::vector<uint8_t>& bytes) {
    BCRYPT_ALG_HANDLE algorithm=nullptr;BCRYPT_HASH_HANDLE hash=nullptr;
    Require(BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)>=0,"SHA256 provider unavailable");
    std::array<uint8_t,32> digest{};
    NTSTATUS status=BCryptCreateHash(algorithm,&hash,nullptr,0,nullptr,0,0);
    if(status>=0) {
        for(size_t at=0;at<bytes.size() && status>=0;) {
            const auto count=ULONG(std::min<size_t>(bytes.size()-at,1u<<20));
            status=BCryptHashData(hash,const_cast<PUCHAR>(bytes.data()+at),count,0);at+=count;
        }
        if(status>=0)status=BCryptFinishHash(hash,digest.data(),ULONG(digest.size()),0);
    }
    if(hash)BCryptDestroyHash(hash);BCryptCloseAlgorithmProvider(algorithm,0);
    Require(status>=0,"SHA256 computation failed");
    constexpr char hex[]="0123456789abcdef";std::string result;
    for(uint8_t byte:digest){result+=hex[byte>>4];result+=hex[byte&15];}return result;
}

std::string AlphaDigest(const Mip& image) {
    const uint64_t pixels = uint64_t(image.width) * image.height;
    Require(image.width && image.height && pixels <= std::numeric_limits<size_t>::max() / 4 &&
            pixels * 4 == image.pixels.size(), "Invalid RGBA alpha reference");
    std::vector<uint8_t> bytes;bytes.reserve(8+image.pixels.size()/4);
    for(uint32_t value:{image.width,image.height}) for(unsigned shift=0;shift<32;shift+=8)bytes.push_back(uint8_t(value>>shift));
    for(size_t p=3;p<image.pixels.size();p+=4)bytes.push_back(image.pixels[p]);
    return Sha256(bytes);
}

Library LoadReviewedManifest(const fs::path& path,const LoadPolicy& policy) {
    Library library;
    if(!policy.enabled)return library; // No path resolution or filesystem access while disabled.
    library.status=LoadStatus::Rejected;
    try {
        Require(Hex(policy.approved_manifest_sha256,64),"An independently approved material manifest digest is required");
        Require(policy.max_materials && policy.max_dimension && policy.max_dimension<=16384 &&
                policy.max_encoded_file && policy.max_decoded_bytes,"Invalid material loading budget");
        std::error_code ec;const fs::path root=fs::canonical(path,ec);
        Require(!ec && fs::is_directory(root),"Missing material root");
        const auto bytes=Read(SafeFile(root,"materials-v1.toml"),4ull<<20);
        library.manifest_sha256=Sha256(bytes);
        Require(library.manifest_sha256==Lower(policy.approved_manifest_sha256),"Unapproved material manifest digest");
        const auto manifest=toml::parse(std::string_view(reinterpret_cast<const char*>(bytes.data()),bytes.size()));
        Keys(manifest,{"schema","materials"});Require(Number(manifest,"schema",1)==1,"Unsupported material manifest schema");
        const auto* entries=manifest["materials"].as_array();
        Require(entries && !entries->empty() && entries->size()<=policy.max_materials,"Missing/excessive material assignments");
        Apartment apartment;ComPtr<IWICImagingFactory> wic;
        Require(SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&wic))),"WIC material decoder unavailable");
        uint64_t used=0;
        for(const auto& node:*entries) {
            const auto* entry=node.as_table();Require(entry,"Material assignment must be a table");
            Keys(*entry,{"texture","vertex_shader","pixel_shader","stage","uv_set","surface","cutout",
                         "normal_convention","baseline_alpha_sha256","review_sha256","albedo","normal","orm","height","emission"});
            Material material;auto& a=material.assignment;
            a.original_texture=HashKey(Text(*entry,"texture"));a.vertex_shader=HashKey(Text(*entry,"vertex_shader"),true);
            a.pixel_shader=HashKey(Text(*entry,"pixel_shader"),true);a.stage=Number(*entry,"stage",3);
            a.uv_set=(*entry)["uv_set"].value<std::string>()==std::optional<std::string>("any")?kAnyUv:Number(*entry,"uv_set",3);
            const auto surface=Text(*entry,"surface");Require(surface=="world"||surface=="vehicle","UI/movie/effect/unknown assignments are forbidden");
            a.surface=surface=="world"?SurfaceClass::World:SurfaceClass::Vehicle;a.cutout=Flag(*entry,"cutout");
            const auto convention=Text(*entry,"normal_convention");Require(convention=="opengl"||convention=="directx","Explicit normal convention required");
            material.normal_convention=convention=="opengl"?NormalConvention::OpenGl:NormalConvention::DirectX;
            material.review_sha256=Lower(Text(*entry,"review_sha256"));material.baseline_alpha_sha256=Lower(Text(*entry,"baseline_alpha_sha256"));
            Require(Hex(material.review_sha256,64)&&Hex(material.baseline_alpha_sha256,64),"Missing review/accepted alpha proof identity");
            if(const auto it=library.by_texture.find(a.original_texture);it!=library.by_texture.end())
                for(uint32_t row:it->second)Require(!Same(library.materials[row].assignment,a),"Duplicate/conflicting PBR draw assignment");
            if((*entry)["albedo"].value<std::string>()==std::optional<std::string>("game"))material.game_albedo=true;
            else {
                material.albedo=LoadMap(*entry,"albedo",MapRole::Albedo,root,policy,wic.Get(),used);
                Require(AlphaDigest(material.albedo.levels.front())==material.baseline_alpha_sha256,"Albedo changes the accepted alpha bytes");
            }
            material.normal=LoadMap(*entry,"normal",MapRole::Normal,root,policy,wic.Get(),used);
            material.orm=LoadMap(*entry,"orm",MapRole::Orm,root,policy,wic.Get(),used);
            if(entry->contains("height")){material.height=LoadMap(*entry,"height",MapRole::Height,root,policy,wic.Get(),used);material.has_height=true;}
            if(entry->contains("emission")){material.emission=LoadMap(*entry,"emission",MapRole::Emission,root,policy,wic.Get(),used);material.has_emission=true;}
            library.by_texture[material.assignment.original_texture].push_back(uint32_t(library.materials.size()));
            library.materials.push_back(std::move(material));
        }
        library.status=LoadStatus::Ready;
    } catch(const std::exception& error) {
        library.materials.clear();library.by_texture.clear();library.errors.push_back(error.what());
    }
    return library;
}

Selection Select(const Library& library,const SurfaceEvidence& draw) {
    if(library.status!=LoadStatus::Ready)return {nullptr,SelectionReason::Disabled};
    const Material* material=nullptr;
    ForTexture(library,draw.assignment.original_texture,[&](uint32_t row){
        if(!material&&Same(library.materials[row].assignment,draw.assignment))material=&library.materials[row];});
    if(!material)return {nullptr,SelectionReason::NoAssignment};
    if(!draw.texture_2d || draw.pretransformed || draw.screen_space || draw.dynamic_texture || draw.render_target ||
       draw.depth_texture || draw.additive || draw.blended)return {nullptr,SelectionReason::UnsupportedDraw};
    if(!draw.geometric_position || !draw.geometric_normal || !draw.tangent_basis)return {nullptr,SelectionReason::MissingGeometry};
    if(!draw.linear_lighting)return {nullptr,SelectionReason::MissingLinearLighting};
    if(material->game_albedo)return {material,SelectionReason::Ready};  // the game's own texture: nothing to match
    const auto& color=material->albedo.levels.front();
    if(draw.color_width!=color.width || draw.color_height!=color.height ||
       Lower(draw.current_alpha_sha256)!=material->baseline_alpha_sha256)return {nullptr,SelectionReason::AlphaMismatch};
    return {material,SelectionReason::Ready};
}

float SrgbToLinear(float x) { x=Unit(x);return x<=0.04045f?x/12.92f:std::pow((x+0.055f)/1.055f,2.4f); }
float LinearToSrgb(float x) { x=Unit(x);return x<=0.0031308f?12.92f*x:1.055f*std::pow(x,1/2.4f)-0.055f; }
Vec3 FresnelSchlick(Vec3 f0,float cosine) {
    f0=Unit(f0);const float a=1-Unit(cosine),a2=a*a,term=a2*a2*a;
    return {f0.x+(1-f0.x)*term,f0.y+(1-f0.y)*term,f0.z+(1-f0.z)*term};
}
float DistributionGgx(float nh,float roughness) {
    const float r=std::clamp(Unit(roughness),0.045f,1.0f),alpha=r*r,a2=alpha*alpha;
    nh=Unit(nh);const float nh2=nh*nh,d=nh2*a2+(1-nh2);return a2/(kPi*d*d);
}
float VisibilitySmith(float nv,float nl,float roughness) {
    if(!std::isfinite(nv)||!std::isfinite(nl)||nv<=0||nl<=0)return 0;
    nv=Unit(nv);nl=Unit(nl);const float r=std::clamp(Unit(roughness),0.045f,1.0f),alpha=r*r,a2=alpha*alpha;
    return 1/((nv+std::sqrt(a2+(1-a2)*nv*nv))*(nl+std::sqrt(a2+(1-a2)*nl*nl)));
}
Vec3 EvaluateDirect(Vec3 color,float metallic,float roughness,Vec3 n,Vec3 v,Vec3 l,Vec3 radiance) {
    n=Normalize(n);v=Normalize(v);l=Normalize(l);const float nv=Dot(n,v),nl=Dot(n,l);
    if(nv<=0||nl<=0)return {};
    const auto h=Normalize(Add(v,l));if(Dot(h,h)<1e-12f)return {};
    color=Unit(color);metallic=Unit(metallic);
    const auto f=FresnelSchlick(Add(Mul({0.04f,0.04f,0.04f},1-metallic),Mul(color,metallic)),Dot(v,h));
    const auto diffuse=Mul(Mul({1-f.x,1-f.y,1-f.z},color),(1-metallic)/kPi);
    const auto specular=Mul(f,DistributionGgx(Dot(n,h),roughness)*VisibilitySmith(nv,nl,roughness));
    radiance={std::isfinite(radiance.x)?std::max(0.0f,radiance.x):0,
              std::isfinite(radiance.y)?std::max(0.0f,radiance.y):0,
              std::isfinite(radiance.z)?std::max(0.0f,radiance.z):0};
    return Mul(Mul(Add(diffuse,specular),radiance),nl);
}
Vec3 EvaluateIndirectDiffuse(Vec3 color,float metallic,float ao,Vec3 irradiance) {
    irradiance={std::isfinite(irradiance.x)?std::max(0.0f,irradiance.x):0,
                std::isfinite(irradiance.y)?std::max(0.0f,irradiance.y):0,
                std::isfinite(irradiance.z)?std::max(0.0f,irradiance.z):0};
    return Mul(Mul(Unit(color),irradiance),(1-Unit(metallic))*Unit(ao)/kPi);
}
Vec3 TangentNormal(Vec3 sample,float strength,NormalConvention convention,Vec3 t,Vec3 b,Vec3 n) {
    sample=Unit(sample);strength=std::isfinite(strength)?std::clamp(strength,0.0f,2.0f):0;
    Vec3 m={(sample.x*2-1)*strength,(sample.y*2-1)*strength,sample.z*2-1};
    if(convention==NormalConvention::OpenGl)m.y=-m.y;
    auto result=Normalize(Add(Add(Mul(t,m.x),Mul(b,m.y)),Mul(n,m.z)));
    return Dot(result,result)>0?result:Normalize(n);
}
std::string_view BrdfHlsl(){return kBrdfHlsl;}
} // namespace gpu::pbr
