// VertexProgramWgsl (the browser build's vertex programs) beside VertexProgramHlsl.
//   wgsl_vertex_test [out dir] [programs dir]
// Synthetic programs cover every MAC and ILU opcode, write masks, outputs, relative constants, missing inputs and each
// VertexDecode. A programs dir from tools/web/vs_from_hlsl.py adds the game's own programs rebuilt from D3D12 captures:
// their HLSL must match the capture exactly (which proves the rebuilt words), and their WGSL is written too. Every WGSL
// module lands in out dir for a WGSL compiler (tools/web/wgsl_check.html runs the browser's).
#include "gpu/shaders.h"
#include "gpu/vertex.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

namespace {

struct Ins {
    uint32_t t[4] = {};
    void set(int sub, int bit, int len, uint32_t v) {
        uint32_t m = ((1u << len) - 1) << bit;
        t[sub] = (t[sub] & ~m) | (v << bit & m);
    }
    // operand 0 A, 1 B, 2 C: mux 1 temporary, 2 input, 3 constant
    Ins& src(int which, uint32_t mux, uint32_t reg, uint32_t swizzle = 0x1B, bool neg = false) {
        if (which == 0) set(1, 8, 1, neg), set(1, 0, 8, swizzle), set(2, 28, 4, reg), set(2, 26, 2, mux);
        if (which == 1) set(2, 25, 1, neg), set(2, 17, 8, swizzle), set(2, 13, 4, reg), set(2, 11, 2, mux);
        if (which == 2) set(2, 10, 1, neg), set(2, 2, 8, swizzle), set(2, 0, 2, reg >> 2), set(3, 30, 2, reg & 3), set(3, 28, 2, mux);
        return *this;
    }
    Ins& input(uint32_t v) { set(1, 9, 4, v); return *this; }
    Ins& constant(uint32_t c, bool relative = false) { set(1, 13, 8, c), set(3, 1, 1, relative); return *this; }
    Ins& mac(uint32_t op, uint32_t r, uint32_t mask) { set(1, 21, 4, op), set(3, 20, 4, r), set(3, 24, 4, mask); return *this; }
    Ins& ilu(uint32_t op, uint32_t mask) { set(1, 25, 3, op), set(3, 16, 4, mask); return *this; }
    Ins& out(uint32_t o, uint32_t mask, bool from_ilu = false) {
        set(3, 12, 4, mask), set(3, 11, 1, 1), set(3, 3, 8, o), set(3, 2, 1, from_ilu);
        return *this;
    }
};

std::vector<uint32_t> Words(std::vector<Ins> ins) {
    std::vector<uint32_t> w;
    ins.back().set(3, 0, 1, 1);
    for (auto& i : ins) w.insert(w.end(), i.t, i.t + 4);
    return w;
}

int g_written = 0;
void Write(const std::string& dir, const std::string& name, const std::string& text) {
    if (dir.empty()) return;
    std::ofstream(dir + "/" + name + ".wgsl", std::ios::binary) << text;
    g_written++;
}

// what WGSL does not allow, in the generated text
void CheckSpelling(const std::string& w) {
    assert(w.find("float4") == std::string::npos);
    assert(w.find("precise") == std::string::npos);
    assert(!std::regex_search(w, std::regex(R"(\)\)\.[xyzw]{4})")));  // a call's result swizzled: a scalar
    // one lane per write: no "X.xy = "
    assert(!std::regex_search(w, std::regex(R"(\.[xyzw]{2,4} = )")));
    assert(w.find("@vertex") != std::string::npos);
}

std::string Region(const std::string& hlsl) {
    const std::string end = "oT3 = float4(0, 0, 0, 1);\n";
    size_t start = hlsl.find(end, hlsl.find("VOut main(")) + end.size();
    return hlsl.substr(start, hlsl.find("\n    VOut o;", start) - start);
}

}  // namespace

int main(int argc, char** argv) {
    std::string out = argc > 1 ? argv[1] : "";
    using namespace gpu;

    // every MAC opcode, with masks and outputs, then every ILU one, then both in one instruction
    std::vector<Ins> all;
    for (uint32_t op = 1; op <= 12; op++)
        all.push_back(Ins().mac(op, op % 12, op | 1).src(0, 2, 0).input(0).src(1, 3, 0, 0xE4).constant(4 + op)
                          .src(2, 1, op % 12, 0x00, op & 1));
    all.push_back(Ins().mac(13, 0, 8).src(0, 1, 3, 0x00));                                  // arl
    all.push_back(Ins().mac(1, 2, 15).src(0, 3, 0).constant(10, true));                     // c[a0 + 10]
    for (uint32_t op = 1; op <= 7; op++) all.push_back(Ins().ilu(op, 15).src(2, 1, 2).mac(0, op + 2, 0));
    all.push_back(Ins().mac(4, 4, 15).ilu(4, 3).src(0, 1, 4).src(1, 1, 5).src(2, 1, 6));    // MAC to R4, ILU to R1
    all.push_back(Ins().mac(2, 1, 15).ilu(2, 8).src(0, 1, 4).src(1, 1, 5).src(2, 1, 6));    // r == 1: the MAC's dropped
    all.push_back(Ins().mac(7, 12, 15).src(0, 2, 0).input(0).src(1, 3, 0).constant(96));    // to oPos through R12
    all.push_back(Ins().mac(1, 0, 0).src(0, 1, 12).out(9, 12));                             // R12 read, oT0.xy
    all.push_back(Ins().mac(1, 0, 0).src(0, 2, 0).input(5).out(3, 15));                     // missing input: vattr
    all.push_back(Ins().ilu(2, 0).src(2, 1, 1).out(5, 8, true));                            // oFog from the ILU
    all.push_back(Ins().mac(1, 0, 0).src(0, 1, 2).out(7, 15).out(10, 6));                   // oT1.yz
    all.push_back(Ins().mac(1, 0, 0).src(0, 1, 2).out(0, 15));                              // oPos output
    std::vector<uint32_t> prog = Words(all);

    std::string w = VertexProgramWgsl(prog, 0x1, 0);
    CheckSpelling(w);
    assert(w.find("let mr = select(vec4f(0.0), vec4f(1.0), ") != std::string::npos);  // slt, sge
    assert(w.find("let ir = ilu_lit(") != std::string::npos);
    assert(w.find("c[clamp(a0 + 10, 0, 191)]") != std::string::npos);
    assert(w.find("dc.vattr[5]") != std::string::npos);
    assert(w.find("a0 = mac_arl(mr.x);") != std::string::npos);
    assert(w.find("oFog.x = ir.x;") != std::string::npos);
    assert(w.find("oT0.x = mr.x;\n        oT0.y = mr.y;") != std::string::npos);
    Write(out, "synthetic_ops", w);

    // each input decode, and no inputs at all
    for (uint32_t d = kDecodeNone; d <= kDecodeFloat2H; d++) {
        std::vector<uint32_t> p = Words({Ins().mac(1, 0, 0).src(0, 2, 0).input(2).out(0, 15)});
        std::string s = VertexProgramWgsl(p, 0x5, uint64_t(d) << 8);
        CheckSpelling(s);
        Write(out, "synthetic_decode" + std::to_string(d), s);
    }
    std::string none = VertexProgramWgsl(Words({Ins().mac(1, 0, 0).src(0, 3, 0).constant(1).out(0, 15)}), 0, 0);
    CheckSpelling(none);
    assert(none.find("vertex_index") != std::string::npos);
    Write(out, "synthetic_noinputs", none);

    // the game's programs
    int checked = 0;
    if (argc > 2) {
        std::string dir = argv[2];
        std::ifstream list(dir + "/programs.txt");
        assert(list && "programs.txt");
        std::string line;
        while (std::getline(list, line)) {
            std::istringstream in(line);
            std::string name, word;
            uint32_t regs;
            uint64_t decode;
            in >> name >> std::hex >> regs >> decode;
            std::vector<uint32_t> words;
            while (in >> word) words.push_back(uint32_t(std::stoul(word, nullptr, 16)));
            std::ifstream bf(dir + "/" + name + ".body", std::ios::binary);
            std::string body((std::istreambuf_iterator<char>(bf)), std::istreambuf_iterator<char>());
            std::string hlsl = Region(VertexProgramHlsl(words, uint16_t(regs), decode));
            if (hlsl != body) {
                std::fprintf(stderr, "%s: HLSL differs from the capture\n--- capture\n%s\n--- now\n%s\n", name.c_str(),
                             body.c_str(), hlsl.c_str());
                return 1;
            }
            std::string s = VertexProgramWgsl(words, uint16_t(regs), decode);
            CheckSpelling(s);
            Write(out, name, s);
            checked++;
        }
    }
    std::printf("wgsl_vertex_test: ok, %d game programs match their captures, %d WGSL modules written\n", checked,
                g_written);
    return 0;
}
