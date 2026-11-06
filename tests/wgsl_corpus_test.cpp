// The fixed function vertex shaders and the pixel shaders over a deterministic corpus of keys, HLSL and WGSL.
//   wgsl_corpus_test <out dir> [hlsl|wgsl|both]
// hlsl: writes every HLSL shader, for diffing the generator's HLSL before and after a change (the D3D12 build must not
// move); wgsl: writes every WGSL module for a compiler (tools/web/wgsl_check.html, which reads list.txt).
#include "gpu/shaders.h"
#include "gpu/vertex.h"

#include <cassert>
#include <cstdio>
#include <fstream>
#include <regex>
#include <string>

namespace {

uint64_t g_rng = 0x9E3779B97F4A7C15ull;
uint32_t R(uint32_t n) {  // xorshift64*, 0..n-1
    g_rng ^= g_rng >> 12, g_rng ^= g_rng << 25, g_rng ^= g_rng >> 27;
    return uint32_t((g_rng * 0x2545F4914F6CDD1Dull) >> 33) % n;
}

std::string g_out;
std::ofstream g_list;
void Put(const std::string& name, const std::string& text, const char* ext) {
    std::ofstream(g_out + "/" + name + ext, std::ios::binary) << text;
    if (std::string(ext) == ".wgsl") g_list << name << ext << "\n";
}

uint32_t CombinerInput() {  // reg 0..15 (6 7 unused), channel bit, mapping
    static const uint32_t regs[] = {0, 1, 2, 3, 4, 5, 8, 9, 10, 11, 12, 13};
    return regs[R(12)] | (R(2) << 4) | (R(8) << 5);
}
uint32_t Inputs() { return CombinerInput() << 24 | CombinerInput() << 16 | CombinerInput() << 8 | CombinerInput(); }
uint32_t Dest() { static const uint32_t d[] = {0, 4, 5, 8, 9, 10, 11, 12, 13}; return d[R(9)]; }
uint32_t Outputs() {
    static const uint32_t maps[] = {0, 0x08, 0x10, 0x18, 0x20, 0x30};
    return Dest() | Dest() << 4 | Dest() << 8 | (R(8) | maps[R(6)] | R(4) << 6) << 12;
}

gpu::TexInfo Tex() {
    gpu::TexInfo t;
    t.kind = uint8_t(R(4));
    if (t.kind == gpu::kTex2D) {
        t.rect = uint8_t(R(4) == 0);
        t.shadow = uint8_t(R(6) == 0 ? 1 + R(2) : 0);
        t.flags = uint8_t((R(4) == 0 ? gpu::kTexWide : 0) | (R(3) == 0 ? gpu::kTexTarget : 0));
    }
    return t;
}

gpu::PixelKey Programmable() {
    gpu::PixelKey k;
    k.programmable = 1;
    // simple modes anywhere (those reading an earlier stage from stage 1 on), or one of the XDK's dot product chains
    static const uint32_t simple[] = {0, 1, 2, 3, 4, 5, 6, 7, 0xF, 0x10};
    static const uint32_t chains[][3] = {{0x11, 9, 1}, {0x11, 0xA, 1}, {0x11, 0x11, 0xD}, {0x11, 0x11, 0xE}, {0x11, 0xB, 0xC}};
    for (int i = 0; i < 4; i++) {
        k.tex[i] = Tex();
        uint32_t m = simple[R(i ? 10 : 6)];
        k.tex_modes |= m << (i * 5);
    }
    if (R(2)) {
        const uint32_t* c = chains[R(5)];
        k.tex_modes = (k.tex_modes & 0x1F) | c[0] << 5 | c[1] << 10 | c[2] << 15;
    }
    // depth textures are only read by the projective modes
    for (int i = 0; i < 4; i++) {
        uint32_t m = (k.tex_modes >> (i * 5)) & 0x1F;
        if (m != 1 && m != 2) k.tex[i].shadow = 0;
    }
    k.dot_mapping = R(0x1000);
    k.input_texture = R(2) << 16 | R(3) << 20;  // stages 2 and 3 read an earlier one
    if (((k.tex_modes >> 10) & 0x1F) == 0xB) k.input_texture = (k.input_texture & ~(0xFu << 20)) | R(2) << 20;  // stage 2 takes stage 3's product early
    k.compare_mode = R(0x10000);
    k.count = (1 + R(8)) | (R(2) ? 0x1000 : 0) | (R(2) ? 0x10000 : 0) | R(2) << 8;
    for (int s = 0; s < 8; s++) k.rgb_in[s] = Inputs(), k.rgb_out[s] = Outputs(), k.a_in[s] = Inputs(), k.a_out[s] = Outputs() & 0x3FFF;
    if (R(3)) k.final_abcd = Inputs(), k.final_efg = (CombinerInput() << 24 | CombinerInput() << 16 | CombinerInput() << 8) | R(8) << 5;
    k.alphakill = R(16);
    return k;
}

gpu::PixelKey Fixed() {
    gpu::PixelKey k;
    for (int i = 0; i < 4; i++) {
        k.tex[i] = Tex();
        uint32_t* f = k.ff[i];
        f[0] = i == 0 || R(3) ? 2 + R(23) : 1;
        static const uint32_t ops[] = {2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 18, 19, 20, 21, 22, 23, 24};
        f[0] = f[0] <= 1 ? 1 : ops[R(22)];
        for (int a : {1, 2, 3, 5, 6, 7}) f[a] = R(6) | R(2) << 4 | R(2) << 5;
        f[4] = R(4) ? ops[R(22)] : 1;
        f[8] = R(4) == 0 ? 5 : 1;
        if (f[0] <= 1) break;
    }
    k.alphakill = R(16);
    k.proj = R(16);
    return k;
}

void Finish(gpu::PixelKey& k) {
    k.two_sided = R(4) == 0;
    k.alphatest = R(2);
    k.pbr = R(6) == 0 && !k.tex[0].shadow;
    k.quad = R(8) == 0;
}

int CheckWgsl(const std::string& w) {
    assert(w.find("float4") == std::string::npos && w.find("float3") == std::string::npos);
    assert(!std::regex_search(w, std::regex(R"(\.[xyzwrgba]{2,4} [-+*/]?= )")));  // one lane per write
    return 1;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) return std::fprintf(stderr, "usage: wgsl_corpus_test <out dir> [hlsl|wgsl|both]\n"), 2;
    g_out = argv[1];
    std::string mode = argc > 2 ? argv[2] : "both";
    bool hlsl = mode != "wgsl", wgsl = mode != "hlsl";
    g_list.open(g_out + "/list.txt");
    int n = 0;
    using namespace gpu;

    for (int i = 0; i < 400; i++, n++) {
        PixelKey k = i % 2 ? Programmable() : Fixed();
        Finish(k);
        if (hlsl) Put("ps" + std::to_string(i), PixelHlsl(k), ".hlsl");
        if (wgsl) CheckWgsl(PixelWgsl(k)), Put("ps" + std::to_string(i), PixelWgsl(k), ".wgsl");
    }
    for (int i = 0; i < 200; i++, n++) {
        FixedKey f;
        f.light = R(16) | R(256) << 8;
        f.types = R(1 << 16);
        for (auto& t : f.tex) t = R(4) | R(4) << 4 | R(5) << 8 | (R(3) == 0 ? 0x800 : 0) | (1 + R(4)) << 12;
        f.blend = R(8);
        uint16_t regs = uint16_t(R(1 << 16) | (R(5) ? 1 : 0));
        uint64_t decode = 0;
        if (R(2))
            for (int r = 0; r < 16; r++) decode |= uint64_t(R(6)) << (r * 4);
        bool pre = R(5) == 0;
        if (hlsl) Put("fv" + std::to_string(i), FixedVertexHlsl(regs, pre, decode, f), ".hlsl");
        if (wgsl) CheckWgsl(FixedVertexWgsl(regs, pre, decode, f)), Put("fv" + std::to_string(i), FixedVertexWgsl(regs, pre, decode, f), ".wgsl");
        if (R(8) == 0) {  // a screen quad's vertex shader
            if (hlsl) Put("qv" + std::to_string(i), QuadHlsl(FixedVertexHlsl(regs, pre, decode, f)), ".hlsl");
            if (wgsl) Put("qv" + std::to_string(i), QuadWgsl(FixedVertexWgsl(regs, pre, decode, f)), ".wgsl");
        }
    }
    std::printf("wgsl_corpus_test: %d shaders (%s)\n", n, mode.c_str());
    return 0;
}
