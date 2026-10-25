// Execute the actual assembler and threaded state/header snapshots with a CPU recording backend.
// No window, GPU device, game process or original assets are needed by this fixture.
#include "gpu/backend.h"
#include "gpu/vertex.h"
#include "kernel/kernel.h"

#include <array>
#include <cassert>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>

namespace fixture {
std::vector<uint8_t> memory(0x1000000);
uint32_t next_system = 0x400000;
constexpr uint32_t header = 0x10000, data = 0x100000, other_data = 0x180000;
constexpr uint32_t packed_normal = 614u | (819u << 11);
struct Vertex { float xyz[3]; uint32_t normal, color; float uv[2]; };
static_assert(sizeof(Vertex) == 28);

void Write(unsigned id, float radius = .415f) {
    Vertex v{{.125f, radius, 0}, packed_normal, 0xA53B7D21, {.125f, .875f}};
    std::memcpy(memory.data() + data + id * sizeof(Vertex), &v, sizeof(v));
}
Vertex Read(unsigned id) { Vertex v; std::memcpy(&v, memory.data() + data + id * 28, 28); return v; }
gpu::State State() {
    gpu::State s;
    s.vs_handle = 1;
    s.vs = std::make_shared<gpu::VertexShader>();
    s.vs->handle = 1;
    s.vs->inputs[0] = {0, 0, 0x32};
    s.vs->inputs[2] = {0, 12, 0x16};
    s.vs->inputs[3] = {0, 16, 0x40};
    s.vs->inputs[9] = {0, 20, 0x22};
    s.vs->stream_stride[0] = 28;
    s.streams[0] = {header, 28};
    s.tire_fit = {{data, 0, 0, 0}, .83f, .975f};
    return s;
}
float Radius(const gpu::Assembled& a, unsigned i) {
    const auto& b = a.buffers[a.elements[0].buffer];
    float xyz[3]; std::memcpy(xyz, b.src + size_t(i) * b.stride + a.elements[0].offset, 12);
    assert(xyz[0] == .125f);
    return std::hypot(xyz[1], xyz[2]);
}
void Near(float actual, float expected) { assert(std::fabs(actual - expected) < 1e-6f); }
void CheckUnchangedAttributes(const gpu::Assembled& a, unsigned i) {
    const auto& b = a.buffers[0];
    if (a.vertices.empty()) {
        Vertex v; std::memcpy(&v, b.src + size_t(i) * 28, 28);
        assert(v.color == 0xA53B7D21 && v.uv[0] == .125f && v.uv[1] == .875f);
    } else {
        const float* color = a.vertices.data() + size_t(i) * (b.stride / 4) + a.elements[3].offset / 4;
        assert(color[0] == float(0x3B) / 255 && color[1] == float(0x7D) / 255 &&
               color[2] == float(0x21) / 255 && color[3] == float(0xA5) / 255);
        const float* uv = a.vertices.data() + size_t(i) * (b.stride / 4) + a.elements[9].offset / 4;
        assert(uv[0] == .125f && uv[1] == .875f && uv[2] == 0 && uv[3] == 1);
    }
}
struct Record { bool fitted; uint32_t header; gpu::State::TireFit fit; std::vector<float> radius; std::vector<uint8_t> bytes; };
struct RecordingBackend final : gpu::Backend {
    gpu::Assembled assembled;
    std::vector<Record> records;
    bool Init(uint32_t, uint32_t) override { return true; }
    void Draw(const gpu::State& s, uint32_t p, uint32_t start, uint32_t count, const uint8_t* indices) override {
        assert(gpu::Assemble(s, p, start, count, indices, assembled));
        Record r{assembled.tire_fit_applied, s.streams[0].vb, s.tire_fit, {}, {}};
        for (unsigned i = 0; i < assembled.vertex_count; ++i) r.radius.push_back(Radius(assembled, i));
        const auto& b = assembled.buffers[0]; r.bytes.assign(b.src, b.src + b.bytes);
        records.push_back(std::move(r));
    }
    void Clear(const gpu::State&, uint32_t, const uint8_t*, uint32_t, uint32_t, float, uint32_t) override {}
    void Copy(const gpu::Surface&, const gpu::Surface&, uint32_t, uint32_t, uint32_t) override {}
    void Present(const gpu::Surface&) override {}
    void Sync(uint32_t, uint32_t) override {}
    void SyncLate(uint32_t) override {}
    void Flush() override {}
    void SetGamma(const uint8_t*) override {}
};
}
namespace kernel { uint8_t* g_base = fixture::memory.data(); }
namespace logx { void Write(Level, const char*, ...) {} }
namespace mem {
uint32_t AllocSystem(uint32_t n, uint32_t align) {
    auto& at = fixture::next_system; at = (at + align - 1) & ~(align - 1); const uint32_t result = at;
    at += n; assert(at < fixture::memory.size()); return result;
}
uint32_t PhysicalFromVirtual(uint32_t va) { return va; }
uint32_t VirtualFromPhysical(uint32_t pa) { return pa; }
}

int main() {
    using namespace fixture;
    kernel::W32(header, 1); kernel::W32(header + 4, data); kernel::W32(header + 8, 0);
    for (unsigned i = 0; i < 5000; ++i) Write(i);
    const std::vector<uint8_t> original(memory.begin() + data, memory.begin() + data + 5000 * 28);
    auto s = State(); gpu::Assembled a;
    // Dense unindexed draws retain a nonzero start, payload identity and independent owned output.
    assert(gpu::Assemble(s, 5, 2, 3, nullptr, a) && a.tire_fit_applied && a.vertex_count == 3);
    for (unsigned i = 0; i < 3; ++i) { Near(Radius(a, i), .4875f); CheckUnchangedAttributes(a, i); }
    auto first = std::vector<uint8_t>(a.buffers[0].src, a.buffers[0].src + a.buffers[0].bytes);
    s.tire_fit.target_ratio = .3f;
    assert(gpu::Assemble(s, 5, 2, 3, nullptr, a) && a.tire_fit_applied);
    for (unsigned i = 0; i < 3; ++i) Near(Radius(a, i), .15f);
    assert(first != std::vector<uint8_t>(a.buffers[0].src, a.buffers[0].src + a.buffers[0].bytes));
    // Both dense indexed and sparse gathered branches use the same source IDs/base vertex semantics.
    const uint16_t dense[] = {2, 4, 3}; s.base_vertex = 7;
    assert(gpu::Assemble(s, 5, 0, 3, reinterpret_cast<const uint8_t*>(dense), a) && a.tire_fit_applied);
    for (unsigned i = 0; i < a.vertex_count; ++i) { Near(Radius(a, i), .15f); CheckUnchangedAttributes(a, i); }
    const uint16_t sparse[] = {0, 4095, 5}; s.base_vertex = 0;
    assert(gpu::Assemble(s, 5, 0, 3, reinterpret_cast<const uint8_t*>(sparse), a) && a.tire_fit_applied);
    assert(a.vertex_count == 3);
    for (unsigned i = 0; i < 3; ++i) { Near(Radius(a, i), .15f); CheckUnchangedAttributes(a, i); }
    // A different VB Data address, a stock identity, absent metadata or wrong stream layout cannot opt in.
    s.tire_fit.vertex_data[0] = other_data;
    assert(gpu::Assemble(s, 5, 0, 3, nullptr, a) && !a.tire_fit_applied); Near(Radius(a, 0), .415f);
    s.tire_fit = {{data, 0, 0, 0}, .83f, .83f};
    assert(gpu::Assemble(s, 5, 0, 3, nullptr, a) && !a.tire_fit_applied); Near(Radius(a, 0), .415f);
    s.tire_fit = {};
    assert(gpu::Assemble(s, 5, 0, 3, nullptr, a) && !a.tire_fit_applied); Near(Radius(a, 0), .415f);
    s = State(); s.vs->inputs[2].type = 0x32;
    assert(gpu::Assemble(s, 5, 0, 3, nullptr, a) && !a.tire_fit_applied);
    // A bad final vertex rejects the entire fit; earlier vertices stay retail and source bytes stay unchanged.
    s = State(); Vertex invalid = Read(2); invalid.xyz[1] = 0;
    std::memcpy(memory.data() + data + 2 * 28, &invalid, 28);
    assert(gpu::Assemble(s, 5, 0, 3, nullptr, a) && !a.tire_fit_applied);
    Near(Radius(a, 0), .415f); Near(Radius(a, 1), .415f); Near(Radius(a, 2), 0);
    Write(2);
    s.tire_fit.source_ratio = std::numeric_limits<float>::quiet_NaN();
    assert(gpu::Assemble(s, 5, 0, 3, nullptr, a) && !a.tire_fit_applied);
    assert(std::memcmp(original.data(), memory.data() + data, original.size()) == 0);

    // Actual Threaded::SendState/Execute clones VB headers and snapshots three distinct fit states before
    // processing. Restoring title metadata immediately does not erase a queued repair or affect the next draw.
    auto recording = std::make_unique<RecordingBackend>(); auto* observed = recording.get();
    auto threaded = gpu::CreateThreadedBackend(std::move(recording)); s = State();
    threaded->Draw(s, 5, 0, 3, nullptr);
    s.tire_fit.target_ratio = .3f; threaded->Draw(s, 5, 0, 3, nullptr);
    s.tire_fit = {}; threaded->Draw(s, 5, 0, 3, nullptr);
    threaded->Flush();
    assert(observed->records.size() == 3);
    for (unsigned i = 0; i < 3; ++i) {
        const auto& r = observed->records[i]; assert(r.header != header);
        assert(r.fitted == (i < 2));
        Near(r.radius[0], i == 0 ? .4875f : i == 1 ? .15f : .415f);
    }
    assert((observed->records[0].fit.target_ratio == .975f && observed->records[1].fit.target_ratio == .3f &&
            observed->records[2].fit.vertex_data == std::array<uint32_t, 4>{}));
    assert(std::memcmp(original.data(), memory.data() + data, original.size()) == 0);
    threaded.reset();
    std::cout << "Tire draw-owned bead fit / threaded state tests passed (raw=" << XBE_RAW_VERTICES << ")\n";
}
