#include "gpu/draw_evidence.h"
#include "gpu/capture_directory.h"
#include <dxgiformat.h>
#include <cassert>
#include <bit>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>

using namespace gpu;
using namespace gpu::draw_evidence;
namespace {
const Chunk& ChunkOf(const Record& r,Kind kind,uint32_t slot=0) {
    for(auto& c:r.chunks)if(c.kind==kind&&c.slot==slot)return c;std::abort();
}
Chunk& Mutable(Record& r,Kind kind,uint32_t slot=0) {return const_cast<Chunk&>(ChunkOf(r,kind,slot));}
std::vector<uint8_t> Read(const std::filesystem::path& p) {
    std::ifstream f(p,std::ios::binary);return {std::istreambuf_iterator<char>(f),{}};
}
struct Fixture {
    State state;
    Assembled assembled;
    Identity id;
    std::array<float,9> vertices{0,0,0,1,0,0,0,1,0};
    std::array<uint16_t,3> indices{0,1,2};
    std::array<uint32_t,41> pipeline{};
    std::array<uint8_t,8> vs{'D','X','B','C',1,2,3,4},ps{'D','X','B','C',5,6,7,8};
    std::array<std::array<float,4>,192> uploaded{};
    DrawConstants draw{};
    Fixture() {
        assembled.regs=1;assembled.vertex_count=3;assembled.draw_count=3;assembled.buffer_count=1;
        assembled.buffers[0]={reinterpret_cast<const uint8_t*>(vertices.data()),sizeof(vertices),12};
        assembled.elements[0]={DXGI_FORMAT_R32G32B32_FLOAT,0,0};
        id.vertex_key=0x1234;id.pixel_key=0x5678;id.flags=FixedTransform;
        id.host_width=2560;id.host_height=1440;id.guest_width=640;id.guest_height=480;
        state.vsc[0]={1,2,3,4};uploaded=state.vsc;uploaded[0][0]=13;
        draw.ext[0]=9;draw.wvp[0]=2;pipeline[0]=1;pipeline[1]=static_cast<uint32_t>(Topology::Triangles);
        pipeline[4]=DXGI_FORMAT_R32G32B32_FLOAT;
    }
    Error Begin(Recorder& r) {return r.Begin(state,assembled,id,vs,ps,pipeline);}
    void Upload(Recorder& r,bool indexed=false) {
        r.VertexConstants(Bytes(uploaded.data(),sizeof(uploaded)));
        r.DrawConstantsUpload(Bytes(&draw,sizeof(draw)));
        r.VertexStream(0,Bytes(vertices.data(),sizeof(vertices)));
        if(indexed)r.IndexUpload(Bytes(indices.data(),sizeof(indices)));
    }
    Record Complete(bool indexed=false) {
        if(indexed){assembled.index_count=3;assembled.index16=true;assembled.index_src=indices.data();}
        Recorder r;assert(Begin(r)==Error::None);Upload(r,indexed);assert(r.DrawWasRecorded()==Error::None);return r.record();
    }
};
void SelectorCases() {
    auto off=ParseSelector("","bad","bad","bad");assert(!off.enabled&&!off.invalid);
    assert(!off.Claim(true,0,0,0));
    for(auto e:{"true","01","yes"})assert(ParseSelector(e,"0000000000001234","0000000000005678","2").invalid);
    assert(ParseSelector("1","1234","0000000000005678","2").invalid);
    assert(ParseSelector("1","0000000000001234","0000000000005678","65536").invalid);
    assert(ParseSelector("1","0000000000001234","0000000000005678","-2").invalid);
    assert(ParseSelector("1","0000000000001234","0000000000005678","2x").invalid);
    auto s=ParseSelector("1","0000000000001234","0000000000005678","2");
    assert(!s.invalid&&!s.Claim(false,0x1234,0x5678,2)&&!s.Claim(true,0x1234,0x5679,2));
    assert(!s.Claim(true,0x1234,0x5678,1));assert(s.Claim(true,0x1234,0x5678,2));
    assert(!s.Claim(true,0x1234,0x5678,2));
}
void UploadLifetimeAndFinalConstants() {
    Fixture f;Recorder r;assert(f.Begin(r)==Error::None);
    f.Upload(r);f.uploaded[0][0]=400;f.vertices.fill(300); // Source/upload storage can change after every callback.
    DrawConstants final=f.draw;final.ext[0]=19;final.sblend[2]=3;
    r.DrawConstantsUpload(Bytes(&final,sizeof(final)));final.ext[0]=100;
    assert(r.DrawWasRecorded()==Error::None);
    float value=0;const auto& b0=ChunkOf(r.record(),Kind::UploadedVertexConstants).bytes;
    std::memcpy(&value,b0.data(),4);assert(value==13);
    const auto& original=ChunkOf(r.record(),Kind::OriginalVertexConstants).bytes;
    std::memcpy(&value,original.data(),4);assert(value==1);
    const auto& b1=ChunkOf(r.record(),Kind::UploadedDrawConstants).bytes;
    std::memcpy(&value,b1.data()+offsetof(DrawConstants,ext),4);assert(value==19);
    const auto& vb=ChunkOf(r.record(),Kind::VertexStream).bytes;std::memcpy(&value,vb.data()+12,4);assert(value==1);
    assert(r.DrawWasRecorded()==Error::WrongUpload); // A second draw cannot be certified by the same recorder.
    f.assembled.elements[15]={DXGI_FORMAT_R32G32B32_FLOAT,9,55};
    Recorder reused_layout;assert(f.Begin(reused_layout)==Error::None);
    assert(reused_layout.record().header.elements[15].format==0); // Inactive retained assembler slots are not current IA inputs.
    Recorder missing;assert(f.Begin(missing)==Error::None);assert(missing.DrawWasRecorded()==Error::MissingUpload);
    Recorder duplicate;assert(f.Begin(duplicate)==Error::None);duplicate.VertexConstants(Bytes(f.uploaded.data(),sizeof(f.uploaded)));
    duplicate.VertexConstants(Bytes(f.uploaded.data(),sizeof(f.uploaded)));assert(duplicate.error()==Error::WrongUpload);
    Recorder short_data;assert(f.Begin(short_data)==Error::None);short_data.VertexStream(0,{vb.data(),vb.size()-1});assert(short_data.error()==Error::WrongUpload);
    Recorder unbound;assert(f.Begin(unbound)==Error::None);unbound.VertexStream(1,vb);assert(unbound.error()==Error::WrongUpload);
    Recorder too_many;assert(f.Begin(too_many)==Error::None);
    for(int i=0;i<4;++i)too_many.DrawConstantsUpload(Bytes(&f.draw,sizeof(f.draw)));assert(too_many.error()==Error::WrongUpload);
}
void LayoutAndBudgetCases() {
    // Appended experiment constants must remain described even while that shader is quarantined.
    // A raw b1 hash without complete field offsets cannot explain its current uploaded layout.
    const auto fields=CurrentDrawConstantLayout();
    auto word=[&](size_t at){uint32_t v=0;for(size_t i=0;i<4;++i)v|=uint32_t(fields.at(at+i))<<(i*8);return v;};
    assert(word(4)==sizeof(DrawConstants)&&fields.size()==12+word(8)*8);
    assert(word(fields.size()-16)==offsetof(DrawConstants,pbr));
    assert(word(fields.size()-12)==sizeof(DrawConstants::pbr));
    assert(word(fields.size()-8)==offsetof(DrawConstants,pbr2));
    assert(word(fields.size()-4)==sizeof(DrawConstants::pbr2));
    Fixture f;Recorder r;
    f.assembled.elements[0].offset=4;assert(f.Begin(r)==Error::InvalidLayout);f.assembled.elements[0].offset=0;
    f.assembled.buffers[0].bytes-=1;assert(f.Begin(r)==Error::InvalidLayout);f.assembled.buffers[0].bytes+=1;
    f.assembled.buffers[0].stride=0;assert(f.Begin(r)==Error::InvalidLayout);f.assembled.buffers[0].stride=12;
    f.assembled.elements[0].format=DXGI_FORMAT_UNKNOWN;assert(f.Begin(r)==Error::InvalidLayout);f.assembled.elements[0].format=DXGI_FORMAT_R32G32B32_FLOAT;
    f.assembled.decode=15;assert(f.Begin(r)==Error::InvalidLayout);f.assembled.decode=0;
    f.assembled.buffer_count=17;assert(f.Begin(r)==Error::InvalidLayout);f.assembled.buffer_count=1;
    f.assembled.vertex_count=kMaxVertices+1;assert(f.Begin(r)==Error::OverBudget);f.assembled.vertex_count=3;
    f.assembled.buffers[0].bytes=static_cast<uint32_t>(kMaxRecordBytes);assert(f.Begin(r)==Error::OverBudget);f.assembled.buffers[0].bytes=sizeof(f.vertices);
    f.assembled.index_count=0xffffffffu;assert(f.Begin(r)==Error::OverBudget);f.assembled.index_count=0;
    for(uint32_t flag:{Quad,Blended,MultiplePasses}){f.id.flags=FixedTransform|flag;assert(f.Begin(r)==Error::UnsupportedDraw);}
    f.id.flags=FixedTransform;f.assembled.pretransformed=true;assert(f.Begin(r)==Error::UnsupportedDraw);f.assembled.pretransformed=false;
    f.id.flags=FixedTransform;f.assembled.topology=Topology::Lines;assert(f.Begin(r)==Error::UnsupportedDraw);
    f.assembled.topology=Topology::Triangles;f.assembled.draw_count=2;assert(f.Begin(r)==Error::InvalidLayout);
    f.assembled.draw_count=3;f.assembled.strip=true;f.assembled.tire_fit_applied=true;assert(f.Begin(r)==Error::None);
    assert((r.record().header.identity.flags&(Strip|TireFit))==(Strip|TireFit));
    f.assembled.strip=false;f.assembled.regs|=2;f.assembled.buffer_count=2;
    f.assembled.elements[1]={DXGI_FORMAT_R32G32B32_FLOAT,1,0};f.assembled.buffers[1]=f.assembled.buffers[0];
    f.pipeline[0]=f.assembled.regs;f.pipeline[5]=DXGI_FORMAT_R32G32B32_FLOAT|(1u<<8);
    assert(f.Begin(r)==Error::None);f.Upload(r);r.VertexStream(1,Bytes(f.vertices.data(),sizeof(f.vertices)));
    assert(r.DrawWasRecorded()==Error::None);
    f.assembled.buffer_count=1;assert(f.Begin(r)==Error::InvalidLayout);
}
void IndexAndByteEncodingCases() {
    Fixture f;auto record=f.Complete(true);assert(Validate(record)==Error::None);
    Mutable(record,Kind::Indices).bytes[4]=3;assert(Validate(record)==Error::InvalidIndex);
    record=f.Complete(true);record.header.base_vertex=-1;assert(Validate(record)==Error::InvalidIndex);
    f.indices={1,2,3};f.assembled.base_vertex=-1;record=f.Complete(true);assert(Validate(record)==Error::None);
    // Wider index uploads and a signed base are encoded explicitly and checked before acceptance.
    std::array<uint32_t,3> wide{1,2,3};f.assembled.index16=false;
    Recorder r;assert(f.Begin(r)==Error::None);f.Upload(r);r.IndexUpload(Bytes(wide.data(),sizeof(wide)));
    assert(r.DrawWasRecorded()==Error::None);record=r.record();
    assert(record.header.index_bytes==12);assert(Mutable(record,Kind::Indices).bytes[0]==1);
    std::vector<uint8_t> binary;assert(Serialize(record,binary)==Error::None);Record parsed;
    assert(Deserialize(binary,parsed)==Error::None);assert(parsed.header.base_vertex==-1);
    assert(parsed.header.identity.vertex_key==0x1234&&parsed.header.elements[0].format==DXGI_FORMAT_R32G32B32_FLOAT);
    std::vector<uint8_t> again;assert(Serialize(parsed,again)==Error::None&&binary==again);
    // Preserve raw float bits, including NaN, without pretending they establish valid geometry.
    auto& b=Mutable(record,Kind::UploadedVertexConstants).bytes;b[0]=0x01;b[1]=0;b[2]=0xc0;b[3]=0x7f;
    assert(Serialize(record,binary)==Error::None&&Deserialize(binary,parsed)==Error::None);
    assert(ChunkOf(parsed,Kind::UploadedVertexConstants).bytes==b);
    for(size_t n:{size_t(0),size_t(8),size_t(30),binary.size()-1})assert(Deserialize({binary.data(),n},parsed)!=Error::None&&parsed.chunks.empty());
    auto bad=binary;bad.push_back(0);assert(Deserialize(bad,parsed)==Error::InvalidEncoding);
    bad=binary;bad[8]=2;assert(Deserialize(bad,parsed)==Error::InvalidEncoding);
    bad=binary;bad[12]=255;assert(Deserialize(bad,parsed)==Error::OverBudget);
    // The fixed envelope is 16 bytes plus 336 explicit header bytes; payload lengths are never trusted for allocation.
    bad=binary;for(size_t i=360;i<368;++i)bad[i]=255;assert(Deserialize(bad,parsed)==Error::InvalidEncoding);
    bad=binary;for(size_t i=352;i<356;++i)bad[i]=0;assert(Deserialize(bad,parsed)==Error::InvalidEncoding);
    record.chunks.push_back(record.chunks.back());assert(Validate(record)==Error::InvalidEncoding);record.chunks.pop_back();
    Mutable(record,Kind::DrawConstantLayout).bytes[0]=2;assert(Validate(record)==Error::InvalidLayout);
    record=f.Complete(true);Mutable(record,Kind::PipelineWords).bytes[4]=0;assert(Validate(record)==Error::InvalidLayout);
    record=f.Complete(true);record.header.elements[15].format=DXGI_FORMAT_R32_FLOAT;assert(Validate(record)==Error::InvalidLayout);
    std::vector<uint8_t> huge(kMaxRecordBytes+1);assert(Deserialize(huge,parsed)==Error::OverBudget);
}
void ProgramAndFileCases() {
    Fixture f;f.state.vs=std::make_shared<VertexShader>();f.state.vs->program={1,2,3,4};f.state.vs->decl={5,6};
    f.state.vs->inputs[0]={0,0,0};f.state.vs->hash=0x12345678;
    f.state.ps=std::make_shared<PixelShader>();f.state.ps->def[0]=0x55;
    f.id.flags=ProgrammableVertex|ProgrammablePixel;auto record=f.Complete();
    assert(ChunkOf(record,Kind::ShaderInputDeclaration).bytes.size()==256);
    assert(ChunkOf(record,Kind::PixelDefinition).bytes[0]==0x55&&record.header.program_hash==0x12345678);
    Mutable(record,Kind::Program).bytes.clear();assert(Validate(record)==Error::InvalidLayout);
    record=f.Complete();CaptureStamp stamp{1,2,3,4,5};
    auto base=std::filesystem::current_path()/"synthetic-evidence-test-output";
    // Fixed stamps: earlier runs' folders would use up the 64 unique-name attempts.
    {std::error_code ec;std::filesystem::remove_all(base,ec);}
    auto dir=CreateUniqueCaptureDirectory(base,stamp);assert(!dir.path.empty());
    auto runtime=dir.path/"synthetic-runtime.bin";std::ofstream(runtime,std::ios::binary)<<"synthetic runtime fixture\n";
    assert(WriteNew(dir.path,record,runtime)==Error::None);
    auto evidence=Read(dir.path/"draw-inputs-v1.bin"),complete=Read(dir.path/"draw-inputs-v1.complete.json");
    assert(!evidence.empty()&&!complete.empty());Record parsed;assert(Deserialize(evidence,parsed)==Error::None);
    std::string metadata(complete.begin(),complete.end());assert(metadata.find("\"gpu_execution_observed\": false")!=std::string::npos);
    assert(metadata.find("\"runtime_file_sha256\"")!=std::string::npos);
    assert(WriteNew(dir.path,record,runtime)==Error::IoFailure);
    assert(Read(dir.path/"draw-inputs-v1.bin")==evidence&&Read(dir.path/"draw-inputs-v1.complete.json")==complete);
    auto foreign=CreateUniqueCaptureDirectory(base,stamp);assert(!foreign.path.empty());
    std::ofstream(foreign.path/"draw-inputs-v1.bin",std::ios::binary)<<"foreign partial evidence";
    auto before=Read(foreign.path/"draw-inputs-v1.bin");assert(WriteNew(foreign.path,record,runtime)==Error::IoFailure);
    assert(Read(foreign.path/"draw-inputs-v1.bin")==before&&!std::filesystem::exists(foreign.path/"draw-inputs-v1.complete.json"));
    auto no_runtime=CreateUniqueCaptureDirectory(base,stamp);assert(WriteNew(no_runtime.path,record,no_runtime.path/"missing")==Error::HashFailure);
    assert(!std::filesystem::exists(no_runtime.path/"draw-inputs-v1.bin"));
    // Keep these small independent fixtures as CPU proof. No canonical runtime, source asset or user file is opened.
}
}
int main() {
    SelectorCases();UploadLifetimeAndFinalConstants();LayoutAndBudgetCases();IndexAndByteEncodingCases();ProgramAndFileCases();
    std::cout<<"draw evidence: selector, post-extension/final-upload ownership, layout/budget, signed indices, strict envelope, exclusive/hash file checks passed\n";
}
