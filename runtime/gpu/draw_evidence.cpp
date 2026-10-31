#include "gpu/draw_evidence.h"
#include "gpu/capture_directory.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#include <dxgiformat.h>

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <sstream>

namespace gpu::draw_evidence {
namespace {
static_assert(sizeof(float)==4 && std::endian::native==std::endian::little);
constexpr uint32_t kAllFlags=1023, kForbidden=Pretransformed|Quad|Blended|MultiplePasses;
constexpr size_t kVertexConstantsBytes=192*4*sizeof(float);
constexpr size_t kTransformBytes=512*16*sizeof(float);
constexpr size_t kPipelineBytes=41*4;
constexpr uint8_t kMagic[8]={'M','3','D','R','A','W','1',0};

struct Writer {
    std::vector<uint8_t> out;
    void U32(uint32_t x) { for(int i=0;i<4;++i) out.push_back(static_cast<uint8_t>(x>>(i*8))); }
    void U64(uint64_t x) { U32(static_cast<uint32_t>(x)); U32(static_cast<uint32_t>(x>>32)); }
    void Raw(std::span<const uint8_t> b) { out.insert(out.end(),b.begin(),b.end()); }
    void F32(float x) { U32(std::bit_cast<uint32_t>(x)); }
    template<class C> void Words(const C& words) { for(const auto& x:words) U32(x); }
};
struct Reader {
    std::span<const uint8_t> bytes;
    size_t pos=0;
    bool ok=true;
    uint32_t U32() {
        if(pos>bytes.size() || bytes.size()-pos<4) {ok=false;return 0;}
        uint32_t n=0;for(int i=0;i<4;++i)n|=static_cast<uint32_t>(bytes[pos++])<<(i*8);return n;
    }
    uint64_t U64() {uint64_t lo=U32();return lo|(static_cast<uint64_t>(U32())<<32);}
    std::span<const uint8_t> Raw(size_t n) {
        if(pos>bytes.size()||n>bytes.size()-pos){ok=false;return {};}
        auto b=bytes.subspan(pos,n);pos+=n;return b;
    }
};
bool ParseHex(std::string_view s,uint64_t& x) {
    if(s.size()!=16)return false;
    x=0;for(char c:s){unsigned d=c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:c>='A'&&c<='F'?c-'A'+10:16;
        if(d==16)return false;x=(x<<4)|d;}return true;
}
uint32_t FormatWidth(uint8_t f) {
    switch(static_cast<DXGI_FORMAT>(f)) {
    case DXGI_FORMAT_R32_FLOAT:case DXGI_FORMAT_R32_UINT:case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_R16G16_SNORM:case DXGI_FORMAT_R16G16_SINT:case DXGI_FORMAT_R8G8B8A8_UNORM:return 4;
    case DXGI_FORMAT_R32G32_FLOAT:case DXGI_FORMAT_R16G16B16A16_SNORM:case DXGI_FORMAT_R16G16B16A16_SINT:return 8;
    case DXGI_FORMAT_R32G32B32_FLOAT:return 12;
    case DXGI_FORMAT_R32G32B32A32_FLOAT:return 16;
    case DXGI_FORMAT_R16_SNORM:case DXGI_FORMAT_R16_SINT:case DXGI_FORMAT_R8G8_UNORM:return 2;
    case DXGI_FORMAT_R8_UNORM:return 1;
    default:return 0;
    }
}
const Chunk* Find(const Record& r,Kind kind,uint32_t slot=0) {
    for(const auto& c:r.chunks)if(c.kind==kind&&c.slot==slot)return &c;return nullptr;
}
uint64_t PayloadSize(const Record& r) {
    uint64_t n=1024; // Strict upper reserve for the fixed header and chunk framing.
    for(const auto& c:r.chunks)n+=16+c.bytes.size();return n;
}
void Add(Record& r,Kind k,std::span<const uint8_t> b,uint32_t slot=0) {
    r.chunks.push_back({k,slot,std::vector<uint8_t>(b.begin(),b.end())});
}
std::vector<uint8_t> OriginalState(const State& s) {
    Writer w;w.U32(1);w.Words(s.simple);w.Words(s.rs);
    for(const auto& a:s.tss)w.Words(a);w.Words(s.textures);w.Words(s.palettes);
    for(auto a:s.streams){w.U32(a.vb);w.U32(a.stride);}
    w.U32(s.index_buffer);w.U32(s.index_data);w.U32(s.base_vertex);
    for(const auto& a:s.psc)for(float x:a)w.F32(x);
    const auto& v=s.viewport;w.U32(v.x);w.U32(v.y);w.U32(v.w);w.U32(v.h);w.F32(v.min_z);w.F32(v.max_z);
    for(auto a:{s.rt,s.zs}){w.U32(a.header);w.U32(a.data);w.U32(a.format);w.U32(a.size);}
    w.U32(s.scissor_count);w.Words(s.scissors);for(float x:s.material)w.F32(x);
    for(const auto& a:s.lights)w.Words(a);w.U32(s.light_enable);
    w.F32(s.ss_offset_x);w.F32(s.ss_offset_y);w.F32(s.zscale);
    for(const auto& a:s.vattr)for(float x:a)w.F32(x);
    w.Words(s.tire_fit.vertex_data);w.F32(s.tire_fit.source_ratio);w.F32(s.tire_fit.target_ratio);
    return std::move(w.out);
}
constexpr size_t kOriginalStateBytes=4+0x800*4+kRsMax*4+4*kTssCount*4+4*4+4*4+16*8+12+
    16*4*4+24+32+4+4*8*4+17*4+8*26*4+4+12+16*4*4+24;
void EncodeHeader(Writer& w,const Header& h) {
    const auto& i=h.identity;
    for(uint64_t v:{i.frame,i.presents,i.vertex_key,i.pixel_key,i.vertex_source_hash,i.pixel_source_hash})w.U64(v);
    for(uint32_t v:{i.draw_ordinal,i.guest_primitive,i.guest_start,i.guest_count,i.host_width,i.host_height,
                   i.guest_width,i.guest_height,i.flags})w.U32(v);
    w.U64(h.program_hash);w.U64(h.decode);
    for(uint32_t v:{h.vs_handle,h.ps_handle,h.program_type,h.registers,h.topology,h.vertex_count,h.draw_count,
                   h.index_count,h.index_bytes,h.buffer_count})w.U32(v);
    w.U32(std::bit_cast<uint32_t>(h.base_vertex));
    for(const auto& e:h.elements)w.U32(static_cast<uint32_t>(e.format)|(static_cast<uint32_t>(e.buffer)<<8)|
                                     (static_cast<uint32_t>(e.offset)<<16));
    for(const auto& b:h.buffers){w.U32(b.bytes);w.U32(b.stride);}
}
Header DecodeHeader(Reader& r) {
    Header h;auto& i=h.identity;
    i.frame=r.U64();i.presents=r.U64();i.vertex_key=r.U64();i.pixel_key=r.U64();
    i.vertex_source_hash=r.U64();i.pixel_source_hash=r.U64();
    i.draw_ordinal=r.U32();i.guest_primitive=r.U32();i.guest_start=r.U32();i.guest_count=r.U32();
    i.host_width=r.U32();i.host_height=r.U32();i.guest_width=r.U32();i.guest_height=r.U32();i.flags=r.U32();
    h.program_hash=r.U64();h.decode=r.U64();h.vs_handle=r.U32();h.ps_handle=r.U32();h.program_type=r.U32();
    h.registers=r.U32();h.topology=r.U32();h.vertex_count=r.U32();h.draw_count=r.U32();h.index_count=r.U32();
    h.index_bytes=r.U32();h.buffer_count=r.U32();h.base_vertex=std::bit_cast<int32_t>(r.U32());
    for(auto& e:h.elements){uint32_t v=r.U32();e={static_cast<uint8_t>(v),static_cast<uint8_t>(v>>8),static_cast<uint16_t>(v>>16)};}
    for(auto& b:h.buffers){b.bytes=r.U32();b.stride=r.U32();}return h;
}
Error Layout(const Header& h) {
    uint32_t flags=h.identity.flags;
    if((flags&~kAllFlags)||(flags&kForbidden)||((flags&FixedTransform)!=0)==((flags&ProgrammableVertex)!=0)||
        h.topology!=static_cast<uint32_t>(Topology::Triangles))return Error::UnsupportedDraw;
    if(h.vertex_count>kMaxVertices||h.index_bytes>kMaxIndexBytes)return Error::OverBudget;
    if(!h.vertex_count||!h.buffer_count||h.buffer_count>16||!(h.registers&1)||(h.registers&~0xffffu))return Error::InvalidLayout;
    uint64_t total=0;
    for(uint32_t b=0;b<h.buffer_count;++b){if(!h.buffers[b].stride||!h.buffers[b].bytes)return Error::InvalidLayout;
        total+=h.buffers[b].bytes;}
    if(total>kMaxRecordBytes)return Error::OverBudget;
    for(uint32_t b=h.buffer_count;b<16;++b)if(h.buffers[b].stride||h.buffers[b].bytes)return Error::InvalidLayout;
    for(uint32_t r=0;r<16;++r) {
        uint32_t decode=static_cast<uint32_t>((h.decode>>(r*4))&15);
        if(decode>kDecodeFloat2H)return Error::InvalidLayout;
        if(!(h.registers&(1u<<r))) {
            if(h.elements[r].format||h.elements[r].buffer||h.elements[r].offset)return Error::InvalidLayout;
            continue;
        }
        const auto& e=h.elements[r];if(e.buffer>=h.buffer_count)return Error::InvalidLayout;
        uint64_t width=FormatWidth(e.format);const auto& b=h.buffers[e.buffer];
        if(!width||static_cast<uint64_t>(e.offset)+width>b.stride||
            static_cast<uint64_t>(h.vertex_count-1)*b.stride+e.offset+width>b.bytes)return Error::InvalidLayout;
    }
    uint32_t n=h.index_count?h.index_count:h.draw_count;
    if(n<3||(!(flags&Strip)&&n%3))return Error::InvalidLayout;
    if(h.index_count) {
        if(h.index_bytes!=static_cast<uint64_t>(h.index_count)*2&&h.index_bytes!=static_cast<uint64_t>(h.index_count)*4)
            return Error::InvalidLayout;
    }else if(h.index_bytes||h.draw_count>h.vertex_count||h.base_vertex)return Error::InvalidLayout;
    return Error::None;
}
struct Hash {
    BCRYPT_ALG_HANDLE algorithm=nullptr;BCRYPT_HASH_HANDLE hash=nullptr;std::vector<uint8_t> object;
    ~Hash(){if(hash)BCryptDestroyHash(hash);if(algorithm)BCryptCloseAlgorithmProvider(algorithm,0);}
    bool Init(){ULONG n=0,size=0;return BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)>=0&&
        BCryptGetProperty(algorithm,BCRYPT_OBJECT_LENGTH,reinterpret_cast<PUCHAR>(&size),sizeof(size),&n,0)>=0&&
        (object.resize(size),BCryptCreateHash(algorithm,&hash,object.data(),size,nullptr,0,0)>=0);}
    bool Update(std::span<const uint8_t> b){return b.size()<=std::numeric_limits<ULONG>::max()&&
        BCryptHashData(hash,const_cast<PUCHAR>(b.data()),static_cast<ULONG>(b.size()),0)>=0;}
    std::string Finish(){std::array<uint8_t,32> d{};if(BCryptFinishHash(hash,d.data(),d.size(),0)<0)return {};
        const char* hex="0123456789abcdef";std::string s;for(auto b:d){s+=hex[b>>4];s+=hex[b&15];}return s;}
};
std::string HashBytes(std::span<const uint8_t> b){Hash h;return h.Init()&&h.Update(b)?h.Finish():std::string{};}
std::string HashFile(const std::filesystem::path& path,uint64_t limit,uint64_t* bytes=nullptr) {
    FILE* f=nullptr;if(_wfopen_s(&f,path.c_str(),L"rb")||!f)return {};
    std::unique_ptr<FILE,decltype(&std::fclose)> file(f,&std::fclose);Hash h;if(!h.Init())return {};
    std::array<uint8_t,65536> buffer{};uint64_t size=0;
    while(size_t n=std::fread(buffer.data(),1,buffer.size(),f)){
        if(n>limit-size||!h.Update({buffer.data(),n}))return {};size+=n;}
    if(std::ferror(f))return {};if(bytes)*bytes=size;return h.Finish();
}
bool NewFile(const std::filesystem::path& path,std::span<const uint8_t> bytes) {
    FILE* f=OpenCaptureFileNew(path);if(!f)return false;
    bool ok=std::fwrite(bytes.data(),1,bytes.size(),f)==bytes.size();return std::fclose(f)==0&&ok;
}
} // namespace

const char* ErrorText(Error e) {
    switch(e){
    case Error::None:return "none";case Error::Disabled:return "disabled";case Error::InvalidSelector:return "invalid selector";
    case Error::UnsupportedDraw:return "unsupported draw";case Error::InvalidLayout:return "invalid layout";
    case Error::OverBudget:return "over budget";case Error::WrongUpload:return "wrong or duplicate upload";
    case Error::MissingUpload:return "missing upload";case Error::InvalidIndex:return "invalid index";
    case Error::InvalidEncoding:return "invalid encoding";case Error::IoFailure:return "exclusive file I/O failed";
    case Error::HashFailure:return "hash/readback failed";}return "unknown";
}
std::span<const uint8_t> Bytes(const void* data,size_t size){return {static_cast<const uint8_t*>(data),size};}
Selector ParseSelector(std::string_view enabled,std::string_view vs,std::string_view ps,std::string_view ordinal) {
    Selector s;if(enabled.empty()||enabled=="0")return s;
    s.enabled=true;s.invalid=enabled!="1"||!ParseHex(vs,s.vertex_key)||!ParseHex(ps,s.pixel_key)||ordinal.empty()||ordinal.size()>5;
    if(!s.invalid)for(char c:ordinal){if(c<'0'||c>'9'){s.invalid=true;break;}s.draw_ordinal=s.draw_ordinal*10+(c-'0');}
    s.invalid=s.invalid||s.draw_ordinal>65535;return s;
}
bool Selector::Claim(bool cap,uint64_t vs,uint64_t ps,uint32_t ordinal) {
    if(!enabled||invalid||claimed||!cap||vs!=vertex_key||ps!=pixel_key||ordinal!=draw_ordinal)return false;
    claimed=true;return true;
}
std::vector<uint8_t> CurrentDrawConstantLayout() {
    Writer w;w.U32(1);w.U32(sizeof(DrawConstants));w.U32(0);
#define FIELD(name) w.U32(offsetof(DrawConstants,name));w.U32(sizeof(DrawConstants::name))
    FIELD(surf);FIELD(fog);FIELD(fog_color);FIELD(misc);FIELD(pc);FIELD(tfactor);FIELD(texsize);FIELD(bump);FIELD(bumpl);
    FIELD(wvp);FIELD(ext);FIELD(spread);FIELD(wv);FIELD(wvn);FIELD(tm);FIELD(mat);FIELD(amb);FIELD(lt);FIELD(rtshift);
    FIELD(vattr);FIELD(sblend);FIELD(bwvp);FIELD(bwv);FIELD(bwvn);FIELD(carpaint);FIELD(pbr);FIELD(pbr2);
#undef FIELD
    // Field count is encoded from the emitted table, not a duplicated declaration count.
    uint32_t count=static_cast<uint32_t>((w.out.size()-12)/8);for(int i=0;i<4;++i)w.out[8+i]=static_cast<uint8_t>(count>>(i*8));
    return std::move(w.out);
}

Error Recorder::Begin(const State& s,const Assembled& a,const Identity& id,std::span<const uint8_t> vs,
                       std::span<const uint8_t> ps,std::span<const uint32_t> pipeline) noexcept {
    try {
        record_={};draw_constant_updates_=0;error_=Error::None;auto& h=record_.header;h.identity=id;
        // These bits describe assembled/uploaded storage and cannot be asserted by the selector's caller.
        h.identity.flags&=~(DrawRecorded|Pretransformed|Strip|TireFit);
        h.decode=a.decode;h.vs_handle=s.vs_handle;h.ps_handle=s.ps?s.ps->handle:0;
        h.program_hash=s.vs?s.vs->hash:0;h.program_type=s.vs?s.vs->program_type:0;
        h.registers=a.regs;h.topology=static_cast<uint32_t>(a.topology);h.vertex_count=a.vertex_count;h.draw_count=a.draw_count;
        h.index_count=a.index_count;h.base_vertex=a.base_vertex;h.buffer_count=a.buffer_count;
        // ResetAssembled intentionally reuses inactive element slots. They are not inputs of this draw.
        for(uint32_t r=0;r<16;++r)if(a.regs&(1u<<r))h.elements[r]=a.elements[r];
        if(a.pretransformed)h.identity.flags|=Pretransformed;if(a.strip)h.identity.flags|=Strip;if(a.tire_fit_applied)h.identity.flags|=TireFit;
        uint64_t ib=static_cast<uint64_t>(a.index_count)*(a.index16?2:4);
        if(ib>kMaxIndexBytes)return error_=Error::OverBudget;h.index_bytes=static_cast<uint32_t>(ib);
        if(a.buffer_count>16)return error_=Error::InvalidLayout;
        for(uint32_t i=0;i<a.buffer_count;++i)h.buffers[i]={a.buffers[i].bytes,a.buffers[i].stride};
        if(auto e=Layout(h);e!=Error::None)return error_=e;
        uint64_t budget=1024+kTransformBytes+kOriginalStateBytes+2*kVertexConstantsBytes+sizeof(DrawConstants)+
            256+240+CurrentDrawConstantLayout().size()+kPipelineBytes+512+ib+vs.size()+ps.size();
        if(vs.empty()||ps.empty()||vs.size()>kMaxShaderBytes||ps.size()>kMaxShaderBytes||pipeline.size()!=41)
            return error_=Error::InvalidLayout;
        if(s.vs){
            if(s.vs->decl.size()>kMaxShaderBytes/4||s.vs->program.size()>kMaxShaderBytes/4)return error_=Error::OverBudget;
            budget+=(s.vs->decl.size()+s.vs->program.size())*4;
        }
        for(uint32_t i=0;i<a.buffer_count;++i)budget+=a.buffers[i].bytes;
        if(budget>kMaxRecordBytes)return error_=Error::OverBudget;
        Add(record_,Kind::Declaration,s.vs?Bytes(s.vs->decl.data(),s.vs->decl.size()*4):std::span<const uint8_t>{});
        Add(record_,Kind::Program,s.vs?Bytes(s.vs->program.data(),s.vs->program.size()*4):std::span<const uint8_t>{});
        Add(record_,Kind::VertexBytecode,vs);Add(record_,Kind::PixelBytecode,ps);
        Writer input;
        if(s.vs){for(auto i:s.vs->inputs){input.U32(std::bit_cast<uint32_t>(static_cast<int32_t>(i.stream)));input.U32(i.offset);input.U32(i.type);}
            input.Words(s.vs->stream_stride);}
        Add(record_,Kind::ShaderInputDeclaration,input.out);
        Add(record_,Kind::PixelDefinition,s.ps?Bytes(s.ps->def,sizeof(s.ps->def)):std::span<const uint8_t>{});
        Add(record_,Kind::OriginalVertexConstants,Bytes(s.vsc.data(),sizeof(s.vsc)));
        Add(record_,Kind::OriginalTransforms,Bytes(s.transforms.data(),sizeof(s.transforms)));
        auto original=OriginalState(s);Add(record_,Kind::OriginalState,original);
        Add(record_,Kind::PipelineWords,Bytes(pipeline.data(),pipeline.size_bytes()));
        auto layout=CurrentDrawConstantLayout();Add(record_,Kind::DrawConstantLayout,layout);
        return error_=Validate(record_,false);
    }catch(...){return error_=Error::OverBudget;}
}
void Recorder::Uploaded(Kind k,uint32_t slot,std::span<const uint8_t> bytes,bool replace) noexcept {
    if(error_!=Error::None)return;
    try {
        auto* existing=const_cast<Chunk*>(Find(record_,k,slot));
        if(existing&&!replace){error_=Error::WrongUpload;return;}
        size_t expected=0;
        if(k==Kind::UploadedVertexConstants)expected=kVertexConstantsBytes;
        else if(k==Kind::UploadedDrawConstants)expected=sizeof(DrawConstants);
        else if(k==Kind::VertexStream&&slot<record_.header.buffer_count)expected=record_.header.buffers[slot].bytes;
        else if(k==Kind::Indices)expected=record_.header.index_bytes;
        if(!expected||bytes.size()!=expected){error_=Error::WrongUpload;return;}
        if(PayloadSize(record_)+bytes.size()+16>kMaxRecordBytes){error_=Error::OverBudget;return;}
        if(existing)existing->bytes.assign(bytes.begin(),bytes.end());else Add(record_,k,bytes,slot);
    }catch(...){error_=Error::OverBudget;}
}
void Recorder::VertexConstants(std::span<const uint8_t> b) noexcept {Uploaded(Kind::UploadedVertexConstants,0,b);}
void Recorder::DrawConstantsUpload(std::span<const uint8_t> b) noexcept {
    if(error_!=Error::None)return;
    if(++draw_constant_updates_>3){error_=Error::WrongUpload;return;}Uploaded(Kind::UploadedDrawConstants,0,b,true);
}
void Recorder::VertexStream(uint32_t slot,std::span<const uint8_t> b) noexcept {Uploaded(Kind::VertexStream,slot,b);}
void Recorder::IndexUpload(std::span<const uint8_t> b) noexcept {Uploaded(Kind::Indices,0,b);}
Error Recorder::DrawWasRecorded() noexcept {
    if(error_!=Error::None)return error_;
    try{
        if(record_.header.identity.flags&DrawRecorded)return error_=Error::WrongUpload;
        record_.header.identity.flags|=DrawRecorded;return error_=Validate(record_);
    }catch(...){return error_=Error::OverBudget;}
}

Error Validate(const Record& r,bool uploads) {
    const auto& h=r.header;if(auto e=Layout(h);e!=Error::None)return e;
    if(r.chunks.size()>kMaxChunks||PayloadSize(r)>kMaxRecordBytes)return Error::OverBudget;
    for(size_t i=0;i<r.chunks.size();++i){const auto& c=r.chunks[i];
        if(static_cast<uint32_t>(c.kind)<1||static_cast<uint32_t>(c.kind)>15)return Error::InvalidEncoding;
        if(c.kind==Kind::VertexStream){if(c.slot>=h.buffer_count)return Error::InvalidLayout;}
        else if(c.slot)return Error::InvalidEncoding;
        for(size_t j=0;j<i;++j)if(r.chunks[j].kind==c.kind&&r.chunks[j].slot==c.slot)return Error::InvalidEncoding;
    }
    auto length=[&](Kind k,size_t n,bool optional=false){auto c=Find(r,k);return c?c->bytes.size()==n:optional;};
    for(Kind k:{Kind::Declaration,Kind::Program}){
        auto c=Find(r,k);if(!c||c->bytes.size()>kMaxShaderBytes||c->bytes.size()%4)return Error::InvalidLayout;
    }
    if((h.identity.flags&ProgrammableVertex)&&Find(r,Kind::Program)->bytes.empty())return Error::InvalidLayout;
    for(Kind k:{Kind::VertexBytecode,Kind::PixelBytecode}){auto c=Find(r,k);if(!c||c->bytes.empty()||c->bytes.size()>kMaxShaderBytes)return Error::InvalidLayout;}
    auto inputs=Find(r,Kind::ShaderInputDeclaration);if(!inputs||(inputs->bytes.size()!=0&&inputs->bytes.size()!=256))return Error::InvalidLayout;
    auto pd=Find(r,Kind::PixelDefinition);if(!pd||(pd->bytes.size()!=0&&pd->bytes.size()!=240))return Error::InvalidLayout;
    if(!length(Kind::OriginalVertexConstants,kVertexConstantsBytes)||!length(Kind::OriginalTransforms,kTransformBytes)||
        !length(Kind::OriginalState,kOriginalStateBytes)||!length(Kind::PipelineWords,kPipelineBytes))return Error::InvalidLayout;
    auto os=Find(r,Kind::OriginalState);if(os->bytes[0]!=1||os->bytes[1]||os->bytes[2]||os->bytes[3])return Error::InvalidEncoding;
    // The explicitly packed PSO metadata must agree with the exact assembler layout copied above.
    Reader pipeline{Find(r,Kind::PipelineWords)->bytes};std::array<uint32_t,41> words{};
    for(auto& word:words)word=pipeline.U32();
    if(words[0]!=h.registers||words[1]!=h.topology||words[20]!=0)return Error::InvalidLayout;
    for(size_t i=0;i<16;++i){const auto& e=h.elements[i];
        uint32_t encoded=static_cast<uint32_t>(e.format)|(static_cast<uint32_t>(e.buffer)<<8)|(static_cast<uint32_t>(e.offset)<<16);
        if(words[4+i]!=encoded)return Error::InvalidLayout;}
    auto layout=Find(r,Kind::DrawConstantLayout);if(!layout||layout->bytes!=CurrentDrawConstantLayout())return Error::InvalidLayout;
    if(!uploads)return Error::None;
    if(!(h.identity.flags&DrawRecorded))return Error::MissingUpload;
    if(!length(Kind::UploadedVertexConstants,kVertexConstantsBytes)||!length(Kind::UploadedDrawConstants,sizeof(DrawConstants)))return Error::MissingUpload;
    for(uint32_t b=0;b<h.buffer_count;++b){auto c=Find(r,Kind::VertexStream,b);if(!c||c->bytes.size()!=h.buffers[b].bytes)return Error::MissingUpload;}
    if(h.index_count){auto c=Find(r,Kind::Indices);if(!c||c->bytes.size()!=h.index_bytes)return Error::MissingUpload;
        uint32_t width=h.index_bytes/h.index_count;
        for(uint32_t i=0;i<h.index_count;++i){uint32_t n=0;for(uint32_t b=0;b<width;++b)n|=static_cast<uint32_t>(c->bytes[static_cast<size_t>(i)*width+b])<<(8*b);
            int64_t index=static_cast<int64_t>(n)+h.base_vertex;if(index<0||index>=h.vertex_count)return Error::InvalidIndex;}
    }else if(Find(r,Kind::Indices))return Error::InvalidLayout;
    return Error::None;
}
Error Serialize(const Record& r,std::vector<uint8_t>& bytes) {
    bytes.clear();if(auto e=Validate(r);e!=Error::None)return e;
    Writer w;w.Raw(kMagic);w.U32(1);w.U32(static_cast<uint32_t>(r.chunks.size()));EncodeHeader(w,r.header);
    for(const auto& c:r.chunks){w.U32(static_cast<uint32_t>(c.kind));w.U32(c.slot);w.U64(c.bytes.size());w.Raw(c.bytes);}
    if(w.out.size()>kMaxRecordBytes)return Error::OverBudget;bytes=std::move(w.out);return Error::None;
}
Error Deserialize(std::span<const uint8_t> bytes,Record& result) {
    result={};if(bytes.size()>kMaxRecordBytes)return Error::OverBudget;
    Reader r{bytes};auto magic=r.Raw(8);
    if(magic.size()!=8||!std::equal(magic.begin(),magic.end(),std::begin(kMagic))||r.U32()!=1)return Error::InvalidEncoding;
    uint32_t count=r.U32();if(count>kMaxChunks)return Error::OverBudget;
    Record decoded;decoded.header=DecodeHeader(r);
    if(!r.ok)return Error::InvalidEncoding;if(auto e=Layout(decoded.header);e!=Error::None)return e;
    for(uint32_t i=0;i<count;++i){auto k=static_cast<Kind>(r.U32());uint32_t slot=r.U32();uint64_t n=r.U64();
        if(!r.ok||n>kMaxRecordBytes||n>bytes.size()-r.pos)return Error::InvalidEncoding;
        auto data=r.Raw(static_cast<size_t>(n));Add(decoded,k,data,slot);}
    if(!r.ok||r.pos!=bytes.size())return Error::InvalidEncoding;
    if(auto e=Validate(decoded);e!=Error::None)return e;result=std::move(decoded);return Error::None;
}
Error WriteNew(const std::filesystem::path& directory,const Record& r,const std::filesystem::path& runtime) noexcept {
    try {
        std::vector<uint8_t> bytes;if(auto e=Serialize(r,bytes);e!=Error::None)return e;
        std::error_code ec;if(!std::filesystem::is_directory(directory,ec)||ec)return Error::IoFailure;
        uint64_t runtime_bytes=0;auto runtime_hash=HashFile(runtime,128ull<<20,&runtime_bytes);
        auto expected_hash=HashBytes(bytes);if(runtime_hash.empty()||!runtime_bytes||expected_hash.empty())return Error::HashFailure;
        auto binary=directory/"draw-inputs-v1.bin";auto complete=directory/"draw-inputs-v1.complete.json";
        // Both names are reserved exclusively. Pre-existing evidence is never modified or removed.
        if(std::filesystem::exists(complete,ec)||ec)return Error::IoFailure;
        if(!NewFile(binary,bytes))return Error::IoFailure;
        uint64_t reread_bytes=0;auto reread_hash=HashFile(binary,kMaxRecordBytes,&reread_bytes);
        if(reread_bytes!=bytes.size()||reread_hash!=expected_hash)return Error::HashFailure;
        std::ostringstream json;json<<"{\n  \"format\": \"mc3-draw-input-evidence-v1\",\n  \"binary_file\": \"draw-inputs-v1.bin\",\n"
            <<"  \"binary_bytes\": "<<bytes.size()<<",\n  \"binary_sha256\": \""<<reread_hash<<"\",\n"
            <<"  \"runtime_file_bytes\": "<<runtime_bytes<<",\n  \"runtime_file_sha256\": \""<<runtime_hash<<"\",\n"
            <<"  \"runtime_hash_scope\": \"on-disk file; loaded-image identity requires external QA\",\n"
            <<"  \"gpu_execution_observed\": false,\n  \"whole_draw_replay_certified\": false,\n"
            <<"  \"constants_scope\": \"post-extension b0 and last-bound b1 before the recorded draw\",\n"
            <<"  \"geometry_scope\": \"immediate copy of mapped upload bytes before command-list draw recording\",\n"
            <<"  \"chunks\": [";
        for(size_t i=0;i<r.chunks.size();++i){const auto& c=r.chunks[i];auto hash=HashBytes(c.bytes);if(hash.empty())return Error::HashFailure;
            json<<(i?",":"")<<"\n    {\"kind\":"<<static_cast<uint32_t>(c.kind)<<",\"slot\":"<<c.slot<<",\"bytes\":"<<c.bytes.size()<<",\"sha256\":\""<<hash<<"\"}";}
        json<<"\n  ]\n}\n";auto text=json.str();
        return NewFile(complete,Bytes(text.data(),text.size()))?Error::None:Error::IoFailure;
    }catch(...){return Error::IoFailure;}
}
} // namespace gpu::draw_evidence
