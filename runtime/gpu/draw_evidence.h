#pragma once
// CPU-side input evidence only: no GPU reads/waits, shader activation or material selection.
#include "gpu/shaders.h"
#include "gpu/vertex.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace gpu::draw_evidence {

constexpr uint64_t kMaxRecordBytes = 16ull << 20;
constexpr uint64_t kMaxShaderBytes = 256ull << 10;
constexpr uint64_t kMaxIndexBytes = 2ull << 20;
constexpr uint32_t kMaxVertices = 1u << 20;
constexpr uint32_t kMaxChunks = 32;

enum class Error { None, Disabled, InvalidSelector, UnsupportedDraw, InvalidLayout, OverBudget,
                   WrongUpload, MissingUpload, InvalidIndex, InvalidEncoding, IoFailure, HashFailure };
const char* ErrorText(Error error);

struct Selector {
    bool enabled = false, invalid = false, claimed = false;
    uint64_t vertex_key = 0, pixel_key = 0;
    uint32_t draw_ordinal = 0;
    // One explicit selected attempt per selector/backend lifetime. Selection requires an armed F4 capture.
    bool Claim(bool frame_capture, uint64_t vs, uint64_t ps, uint32_t ordinal);
};
Selector ParseSelector(std::string_view enabled, std::string_view vertex_key,
                       std::string_view pixel_key, std::string_view ordinal);

enum Flag : uint32_t {
    FixedTransform=1, ProgrammableVertex=2, Pretransformed=4, Quad=8, Strip=16,
    TireFit=32, Blended=64, MultiplePasses=128, DrawRecorded=256, ProgrammablePixel=512
};
struct Identity {
    uint64_t frame=0, presents=0, vertex_key=0, pixel_key=0, vertex_source_hash=0, pixel_source_hash=0;
    uint32_t draw_ordinal=0, guest_primitive=0, guest_start=0, guest_count=0;
    uint32_t host_width=0, host_height=0, guest_width=0, guest_height=0;
    uint32_t flags=0;
};
struct BufferLayout { uint32_t bytes=0, stride=0; };
struct Header {
    Identity identity;
    uint64_t program_hash=0, decode=0;
    uint32_t vs_handle=0, ps_handle=0, program_type=0, registers=0, topology=0;
    uint32_t vertex_count=0, draw_count=0, index_count=0, index_bytes=0, buffer_count=0;
    int32_t base_vertex=0;
    std::array<VertexElement,16> elements{};
    std::array<BufferLayout,16> buffers{};
};
enum class Kind : uint32_t {
    Declaration=1, Program, VertexBytecode, PixelBytecode, ShaderInputDeclaration, PixelDefinition,
    OriginalVertexConstants, UploadedVertexConstants, UploadedDrawConstants, OriginalTransforms,
    OriginalState, PipelineWords, DrawConstantLayout, VertexStream, Indices
};
struct Chunk { Kind kind{}; uint32_t slot=0; std::vector<uint8_t> bytes; };
struct Record { Header header; std::vector<Chunk> chunks; };

// All byte spans are copied synchronously. No upload-ring or source pointer is retained.
class Recorder {
public:
    Error Begin(const State& state, const Assembled& assembled, const Identity& identity,
                std::span<const uint8_t> vertex_bytecode, std::span<const uint8_t> pixel_bytecode,
                std::span<const uint32_t> actual_pipeline_words) noexcept;
    void VertexConstants(std::span<const uint8_t> actual_uploaded_bytes) noexcept;
    void DrawConstantsUpload(std::span<const uint8_t> actual_uploaded_bytes) noexcept;
    void VertexStream(uint32_t slot, std::span<const uint8_t> actual_uploaded_bytes) noexcept;
    void IndexUpload(std::span<const uint8_t> actual_uploaded_bytes) noexcept;
    // Called only AFTER the selected DrawIndexedInstanced/DrawInstanced call returns.
    Error DrawWasRecorded() noexcept;
    Error error() const { return error_; }
    const Record& record() const { return record_; }
private:
    Record record_;
    Error error_=Error::Disabled;
    uint32_t draw_constant_updates_=0;
    void Uploaded(Kind kind,uint32_t slot,std::span<const uint8_t> bytes,bool replace=false) noexcept;
};

// Strict little-endian envelope and current input/layout bounds. This is NOT a native replay API.
Error Validate(const Record& record, bool require_uploads=true);
Error Serialize(const Record& record,std::vector<uint8_t>& bytes);
Error Deserialize(std::span<const uint8_t> bytes,Record& record);
std::vector<uint8_t> CurrentDrawConstantLayout();
std::span<const uint8_t> Bytes(const void* data,size_t size);

// Exclusive files in an already owned capture directory. A complete marker is written only after
// the binary closes and rereads with the exact hash. Failures preserve partial/foreign files.
// runtime_file_sha256 authenticates a bounded on-disk file; external QA must join it to the loaded image.
Error WriteNew(const std::filesystem::path& owned_directory,const Record& record,
               const std::filesystem::path& runtime_file) noexcept;

}  // namespace gpu::draw_evidence
