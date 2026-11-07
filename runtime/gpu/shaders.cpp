#include "gpu/shaders.h"

#include "gpu/fast_hash.h"
#include "gpu/vertex.h"

#include <algorithm>
#include <bit>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace gpu {

namespace {

std::string F(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return buf;
}

const char* kCommonHead = R"(
struct VOut {
    float4 pos : SV_Position;
    float4 d0 : COLOR0;
    float4 d1 : COLOR1;
    float4 b0 : COLOR2;
    float4 b1 : COLOR3;
    float4 tc0 : TEXCOORD0;
    float4 tc1 : TEXCOORD1;
    float4 tc2 : TEXCOORD2;
    float4 tc3 : TEXCOORD3;
    float fogamt : TEXCOORD4;
};
cbuffer VC : register(b0) { float4 c[192]; };
cbuffer DC : register(b1) {
    float4 surf;
    float4 fogp;
    float4 fogcolor;
    float4 misc;
    float4 pc[18];
    float4 tfactor;
    float4 texsize[4];
    float4 bumpmat[4];
    float4 bumplum[4];
    float4x4 wvp;
)";
const char* kCommonTail = R"(    float4 spread[4];
    float4x4 wv;
    float4x4 wvn;
    float4x4 tm[4];
    float4 mat[5];
    float4 amb;
    float4 lt[56];
    float4 rtshift;
    float4 vattr[16];
    float4 sblend;
    float4x4 bwvp[4];
    float4x4 bwv[4];
    float4x4 bwvn[4];
    float4 carpaint;
    float4 pbr;
    float4 pbr2;
    float4 zparams;
    float4 taa;
};
)";

const ShaderHooks& Hooks() {
    static const ShaderHooks plain;
    return g_shader_hooks ? *g_shader_hooks : plain;
}

std::string Common() { return kCommonHead + Hooks().Constants() + kCommonTail; }

// vertex programs

const char* kVsHelpers = R"(
float4 mac_mul(float4 a, float4 b) { return (a == 0 || b == 0) ? (float4)0 : a * b; }
int mac_arl(float v) { return (int)floor(v + 1e-3); }
// magnitude clamped between 1 / 2^64 and 2^64, sign kept
float magclamp(float v) { float m = clamp(abs(v), 5.42101e-20, 1.84467e+19); return v < 0 ? -m : m; }
float4 ilu_rcp(float v) { return (1.0 / v).xxxx; }
float4 ilu_rcc(float v) { return magclamp(1.0 / v).xxxx; }
float4 ilu_rsq(float v) { return v == 0 ? (1.#INF).xxxx : (rsqrt(abs(v))).xxxx; }
// 2^floor(v), fraction, 2^v
float4 ilu_exp(float v) {
    float whole = floor(v);
    return float4(exp2(whole), v - whole, exp2(v), 1);
}
// exponent, mantissa, log2 of |v|; zero logs to negative infinity
float4 ilu_log(float v) {
    float mag = abs(v), l2 = log2(mag), ex = floor(l2);
    return mag == 0 ? float4(-1.#INF, 1, -1.#INF, 1) : float4(ex, mag / exp2(ex), l2, 1);
}
// power clamped to 128 less 1/256
float4 ilu_lit(float4 s) {
    const float cap = 127.99609375;
    float diffuse = max(s.x, 0);
    float spec = exp2(clamp(s.w, -cap, cap) * log2(max(s.y, 0)));
    return float4(1, diffuse, diffuse > 0 ? spec : 0, 1);
}
float fogfactor(float d) {
    if (fogp.w == 3) return (fogp.y - d) / (fogp.y - fogp.x);
    if (fogp.w == 1) return exp(-d * fogp.z);
    if (fogp.w == 2) return exp(-(d * fogp.z) * (d * fogp.z));
    return 1;
}
)";

struct Tok {
    const uint32_t* t;
    uint32_t f(int sub, int bit, int len) const { return (t[sub] >> bit) & ((1u << len) - 1); }
};

const char* kMask[16] = {"", "w", "z", "zw", "y", "yw", "yz", "yzw", "x", "xw", "xz", "xzw", "xy", "xyw", "xyz", "xyzw"};
const char* kLanes[5] = {"", "x", "xy", "xyz", "xyzw"};  // oFog takes one lane per mask bit, from x
const char* kOut[16] = {"oPos", "", "", "oD0", "oD1", "oFog", "oPts", "oB0", "oB1", "oT0", "oT1", "oT2", "oT3", "", "", ""};

// operand A 0, B 1, C 2
std::string Src(const Tok& k, int which, uint16_t regs, bool wgsl = false) {
    uint32_t neg, sw[4], reg, mux;
    if (which == 0) {
        neg = k.f(1, 8, 1);
        sw[0] = k.f(1, 6, 2), sw[1] = k.f(1, 4, 2), sw[2] = k.f(1, 2, 2), sw[3] = k.f(1, 0, 2);
        reg = k.f(2, 28, 4), mux = k.f(2, 26, 2);
    } else if (which == 1) {
        neg = k.f(2, 25, 1);
        sw[0] = k.f(2, 23, 2), sw[1] = k.f(2, 21, 2), sw[2] = k.f(2, 19, 2), sw[3] = k.f(2, 17, 2);
        reg = k.f(2, 13, 4), mux = k.f(2, 11, 2);
    } else {
        neg = k.f(2, 10, 1);
        sw[0] = k.f(2, 8, 2), sw[1] = k.f(2, 6, 2), sw[2] = k.f(2, 4, 2), sw[3] = k.f(2, 2, 2);
        reg = k.f(2, 0, 2) << 2 | k.f(3, 30, 2), mux = k.f(3, 28, 2);
        uint32_t ilu = k.f(1, 25, 3);
        if (ilu >= 2 && ilu <= 6) sw[1] = sw[2] = sw[3] = sw[0];  // scalar ops read x
    }
    // mux: 1 temporary (R12 reads oPos), 2 input, 3 constant
    std::string base;
    if (mux == 1) {
        base = reg == 12 ? "oPos" : F("R%u", reg);
    } else if (mux == 2) {
        uint32_t v = k.f(1, 9, 4);
        base = (regs >> v) & 1 ? F("i.v%u", v) : F(wgsl ? "dc.vattr[%u]" : "vattr[%u]", v);  // the inline value, as the console reads it
    } else {
        uint32_t cr = k.f(1, 13, 8);
        base = k.f(3, 1, 1) ? F("c[clamp(a0 + %u, 0, 191)]", cr) : F("c[%u]", cr);
    }
    const char* xyzw = "xyzw";
    std::string s = F("(%s(%s).%c%c%c%c)", neg ? "-" : "", base.c_str(), xyzw[sw[0]], xyzw[sw[1]], xyzw[sw[2]], xyzw[sw[3]]);
    return s;
}

// MAC opcodes from 1: mov mul add mad dp3 dph dp4 dst min max slt sge arl
std::string MacExpr(uint32_t op, const std::string& a, const std::string& b, const std::string& c) {
    switch (op) {
        case 1: return a;
        case 2: return "mac_mul(" + a + ", " + b + ")";
        case 3: return "(" + a + " + " + c + ")";
        case 4: return "(mac_mul(" + a + ", " + b + ") + " + c + ")";
        case 5: return "dot(" + a + ".xyz, " + b + ".xyz).xxxx";
        case 6: return "dot(float4(" + a + ".xyz, 1), " + b + ").xxxx";
        case 7: return "dot(" + a + ", " + b + ").xxxx";
        case 8: return "float4(1, " + a + ".y * " + b + ".y, " + a + ".z, " + b + ".w)";
        case 9: return "min(" + a + ", " + b + ")";
        case 10: return "max(" + a + ", " + b + ")";
        case 11: return "(float4)(" + a + " < " + b + ")";
        case 12: return "(float4)(" + a + " >= " + b + ")";
        case 13: return a;
        default: return "(float4)0";
    }
}

// ILU opcodes from 1: mov rcp rcc rsq exp log lit
std::string IluExpr(uint32_t op, const std::string& c) {
    switch (op) {
        case 1: return c;
        case 2: return "ilu_rcp(" + c + ".x)";
        case 3: return "ilu_rcc(" + c + ".x)";
        case 4: return "ilu_rsq(" + c + ".x)";
        case 5: return "ilu_exp(" + c + ".x)";
        case 6: return "ilu_log(" + c + ".x)";
        case 7: return "ilu_lit(" + c + ")";
        default: return "(float4)0";
    }
}

std::string RegDest(uint32_t r) { return r == 12 ? "oPos" : F("R%u", r); }

std::string VsInputStruct(uint16_t regs) {
    std::string s = "struct VIn {\n";
    for (int r = 0; r < 16; r++)
        if ((regs >> r) & 1) s += F("    float4 v%d : ATTR%d;\n", r, r);
    if (!regs) s += "    uint id : SV_VertexID;\n";
    return s + "};\n";
}

// the input struct and the head of main: elements read raw are finished into float4 i.vN first
std::string VsInputs(uint16_t regs, uint64_t decode) {
    if (!decode) return VsInputStruct(regs) + "VOut main(VIn i) {\n";
    std::string raw = "struct VRaw {\n", in = "struct VIn {\n", body;
    bool packed = false;
    for (int r = 0; r < 16; r++) {
        if (!((regs >> r) & 1)) continue;
        uint32_t d = uint32_t(decode >> (r * 4)) & 15;
        const char* type = d == kDecodeInt || d == kDecodeIntW1 ? "int4" : d == kDecodePacked ? "uint" : "float4";
        raw += F("    %s v%d : ATTR%d;\n", type, r, r);
        in += F("    float4 v%d;\n", r);
        packed |= d == kDecodePacked;
        switch (d) {
            case kDecodeW1: body += F("    i.v%d = float4(ri.v%d.xyz, 1);\n", r, r); break;
            case kDecodeInt: body += F("    i.v%d = float4(ri.v%d);\n", r, r); break;
            case kDecodeIntW1: body += F("    i.v%d = float4(float3(ri.v%d.xyz), 1);\n", r, r); break;
            case kDecodePacked: body += F("    i.v%d = UnpackNp3(ri.v%d);\n", r, r); break;
            case kDecodeFloat2H: body += F("    i.v%d = float4(ri.v%d.xy, 0, ri.v%d.z);\n", r, r, r); break;
            default: body += F("    i.v%d = ri.v%d;\n", r, r); break;
        }
    }
    std::string s = raw + "};\n" + in + "};\n";
    if (packed)
        s += "float4 UnpackNp3(uint u) {\n"
             "    int3 v = int3(int(u << 21) >> 21, int(u << 10) >> 21, int(u) >> 22);\n"
             "    return float4(max(float3(v) / float3(1023, 1023, 511), -1), 1);\n"
             "}\n";
    return s + "VOut main(VRaw ri) {\n    VIn i;\n" + body;
}

const char* kVsEpilogue = R"(
    VOut o;
    precise float4 p = oPos;
    p.w = magclamp(p.w);
    precise float4 q;
    // full screen passes drawn half a pixel up-left for the console's integer pixel centers stop half a pixel short
    // of the far edges on D3D12's, which upscaled leaves the last host columns and rows undrawn
    // surf.w set: a point, which stays put since pulled out it would leave the target
    if (surf.w == 0 && abs(p.x - (surf.x - 0.5)) < 1e-3) p.x = surf.x;
    if (surf.w == 0 && abs(p.y - (surf.y - 0.5)) < 1e-3) p.y = surf.y;
    q.x = p.x * 2 / surf.x - 1;
    q.y = 1 - p.y * 2 / surf.y;
    q.z = p.z * surf.z;
    q.xyz *= p.w;
    q.w = p.w;
    o.pos = q;
    o.d0 = saturate(oD0);
    o.d1 = saturate(oD1);
    o.b0 = saturate(oB0);
    o.b1 = saturate(oB1);
    o.tc0 = oT0;
    o.tc1 = oT1;
    o.tc2 = oT2;
    o.tc3 = oT3;
    o.fogamt = fogfactor(oFog.x);
    return o;
}
)";

const char* kVsPrologue = R"(
    int a0 = 0;
    // precise: EQUAL depth passes need bit exact positions
    precise float4 R0 = 0, R1 = 0, R2 = 0, R3 = 0, R4 = 0, R5 = 0, R6 = 0, R7 = 0, R8 = 0, R9 = 0, R10 = 0, R11 = 0;
    precise float4 oPos = 0;
    float4 oD0 = float4(0, 0, 0, 1), oD1 = 0, oB0 = float4(0, 0, 0, 1), oB1 = 0, oFog = 0, oPts = 1;
    float4 oT0 = float4(0, 0, 0, 1), oT1 = float4(0, 0, 0, 1), oT2 = float4(0, 0, 0, 1), oT3 = float4(0, 0, 0, 1);
)";

// combiners

struct In {
    uint32_t reg, chan, mod;
};
In ParseIn(uint32_t v) { return {v & 0xF, v & 0x10, v & 0xE0}; }

// the pixel stage's generators write HLSL for D3D12 or, with w set, WGSL for WebGPU (the browser build): one decision
// tree, two spellings side by side. WGSL has no multi-lane swizzle writes, no implicit vector size changes or scalar
// to vector clamps, select for conditions, mix for lerp, dpdx/dpdy for ddx/ddy, and reads DC through dc.
const char* Sp(bool w, const char* hlsl, const char* wgsl) { return w ? wgsl : hlsl; }

struct Ps {
    const PixelKey& k;
    bool w = false;
    int stage = 0;
    std::string e, f;  // final combiner E F for EF_PROD
    bool clamp_sum = false, inv_v1 = false, inv_r0 = false;
    std::string code;

    std::string Var(uint32_t reg) {
        uint32_t flags = k.count >> 8;  // PS_COMBINERCOUNT flags: 0x10 unique C0 per stage, 0x100 unique C1
        const char* pc = Sp(w, "pc[%d]", "dc.pc[%d]");
        switch (reg) {
            case 0: return Sp(w, "float4(0,0,0,0)", "vec4f(0.0)");
            case 1: return F(pc, stage == 8 ? 16 : ((flags & 0x10) ? stage * 2 : 0));
            case 2: return F(pc, stage == 8 ? 17 : ((flags & 0x100) ? stage * 2 + 1 : 1));
            case 3: return "fogin";
            case 4: return "v0";
            case 5: return "v1";
            case 8: return "t0";
            case 9: return "t1";
            case 10: return "t2";
            case 11: return "t3";
            case 12: return "r0";
            case 13: return "r1";
            case 14:
                return F(Sp(w, "%s(float4(%s.rgb + %s.rgb, 0))", "%s(vec4f(%s.rgb + %s.rgb, 0.0))"), clamp_sum ? "saturate" : "",
                         inv_v1 ? "(1 - v1)" : "v1", inv_r0 ? "(1 - r0)" : "r0");
            case 15: return Sp(w, "float4(", "vec4f(") + e + " * " + f + Sp(w, ", 0)", ", 0.0)");
            default: return Sp(w, "float4(0,0,0,0)", "vec4f(0.0)");
        }
    }

    std::string Input(In in, bool alpha) {
        std::string r = Var(in.reg);
        if (!alpha) r += in.chan ? ".aaa" : ".rgb";
        else r += in.chan ? ".a" : ".b";
        std::string z = w ? (alpha ? "0.0" : "vec3f(0.0)") : "0";
        switch (in.mod) {
            // PS_INPUTMAPPING
            case 0x00: return "max(" + r + ", " + z + ")";
            case 0x20: return "(1 - saturate(" + r + "))";
            case 0x40: return "(2 * max(" + r + ", " + z + ") - 1)";
            case 0x60: return "(-2 * max(" + r + ", " + z + ") + 1)";
            case 0x80: return "(max(" + r + ", " + z + ") - 0.5)";
            case 0xA0: return "(-max(" + r + ", " + z + ") + 0.5)";
            case 0xC0: return r;
            default: return "-" + r;
        }
    }

    static std::string Map(const std::string& v, uint32_t m) {
        switch (m) {
            // PS_COMBINEROUTPUT bias and shift
            case 0x08: return "(" + v + " - 0.5)";
            case 0x10: return "(" + v + " * 2)";
            case 0x18: return "((" + v + " - 0.5) * 2)";
            case 0x20: return "(" + v + " * 4)";
            case 0x30: return "(" + v + " / 2)";
            default: return v;
        }
    }

    static const char* Dest(uint32_t reg) {
        switch (reg) {
            case 4: return "v0";
            case 5: return "v1";
            case 8: return "t0";
            case 9: return "t1";
            case 10: return "t2";
            case 11: return "t3";
            case 12: return "r0";
            case 13: return "r1";
            default: return nullptr;
        }
    }

    // T x = clamp((T)v, -1, 1)
    std::string Clamped(const char* T, bool alpha, const std::string& name, const std::string& v) {
        if (!w) return F("%s %s = clamp((%s)%s, -1, 1);\n", T, name.c_str(), T, v.c_str());
        return F("let %s = clamp(%s(%s), %s);\n", name.c_str(), T, v.c_str(), alpha ? "-1.0, 1.0" : "vec3f(-1.0), vec3f(1.0)");
    }

    // rgb and alpha halves read the registers before either writes, so the writes come back to the caller
    std::string Stage(uint32_t inputs, uint32_t output, bool alpha) {
        In d = ParseIn(inputs & 0xFF), c = ParseIn((inputs >> 8) & 0xFF), b = ParseIn((inputs >> 16) & 0xFF),
           a = ParseIn(inputs >> 24);
        // fl: 1 cd dot, 2 ab dot, 4 mux, 0x38 mapping, 0x40 and 0x80 blue to alpha
        uint32_t cd_dst = output & 0xF, ab_dst = (output >> 4) & 0xF, sum_dst = (output >> 8) & 0xF, fl = output >> 12;
        const char* m = alpha ? "a" : "rgb";
        const char* T = alpha ? Sp(w, "float", "f32") : Sp(w, "float3", "vec3f");
        const char* n = alpha ? "a" : "c";
        std::string A = Input(a, alpha), B = Input(b, alpha), C = Input(c, alpha), D = Input(d, alpha);
        bool dots = !(w && alpha);  // HLSL's dot of two scalars is their product; WGSL has none
        std::string ab = (fl & 2) && dots ? "dot(" + A + ", " + B + ")" : "(" + A + " * " + B + ")";
        std::string cd = (fl & 1) && dots ? "dot(" + C + ", " + D + ")" : "(" + C + " * " + D + ")";
        uint32_t map = fl & 0x38;
        code += Clamped(T, alpha, std::string("ab") + n, Map(ab, map));
        code += Clamped(T, alpha, std::string("cd") + n, Map(cd, map));
        std::string sum;
        if (!(fl & 4)) sum = "(" + ab + " + " + cd + ")";
        else if (!w)
            sum = F("((%s) ? (%s)%s : (%s)%s)", ((k.count >> 8) & 1) ? "r0.a >= 0.5" : "(uint(r0.a * 255) & 1) == 1", T, cd.c_str(), T,
                    ab.c_str());
        else
            sum = F("select(%s(%s), %s(%s), %s)", T, ab.c_str(), T, cd.c_str(),
                    ((k.count >> 8) & 1) ? "r0.a >= 0.5" : "(u32(r0.a * 255.0) & 1u) == 1u");
        code += Clamped(T, alpha, std::string("ms") + n, Map(sum, map));
        auto put = [&](const char* r, const char* v) {
            if (alpha || !w) return F("%s.%s = %s%s;\n", r, m, v, n);
            return F("%s = vec4f(%s%s, %s.a);\n", r, v, n, r);
        };
        std::string post;
        if (const char* r = Dest(ab_dst)) {
            post += put(r, "ab");
            if (!alpha && (fl & 0x80)) post += F("%s.a = abc.b;\n", r);
        }
        if (const char* r = Dest(cd_dst)) {
            post += put(r, "cd");
            if (!alpha && (fl & 0x40)) post += F("%s.a = cdc.b;\n", r);
        }
        if (const char* r = Dest(sum_dst)) post += put(r, "ms");
        return post;
    }
};

// a dot stage's operand: the source stage's rgb, raw or read back as signed bytes
std::string DotInput(uint32_t map, uint32_t src) {
    switch (map) {
        case 1: return F("sbyte(t%u.rgb, 128, 128, 127)", src);        // offset by 128
        case 2: return F("sbyte(t%u.rgb, -0.5, 255.5, 127.5)", src);  // two's complement, (2b + 1) / 255
        case 3: return F("sbyte(t%u.rgb, 0, 256, 127)", src);         // two's complement
        default: return F("t%u.rgb", src);
    }
}

// stage i's coordinates as the title wrote them, over 0..1. The console draws half a pixel right and down of the
// host unless a program writes positions raw: a render target is read where the console's offset puts it
std::string TexUv(const TexInfo& t, int i, const std::string& uv, bool w = false) {
    std::string n = "(" + uv + ")";
    if (t.rect) n += F(Sp(w, " / texsize[%d].xy", " / dc.texsize[%d].xy"), i);
    n += F(Sp(w, " + texsize[%d].zw", " + dc.texsize[%d].zw"), i);
    if (!(t.flags & kTexTarget)) return n;
    std::string step = Sp(w, "ddx(", "dpdx(") + n + F(Sp(w, ") + spread[%d].w * ddy(", ") + dc.spread[%d].w * dpdy("), i) + n + ")";
    return n + F(Sp(w, " + rtshift[%d] * (", " + dc.rtshift[%d] * ("), i) + step + ")";
}

// texture shader stage i, per its mode
std::string TexFetch(const PixelKey& k, int i, bool w = false) {
    uint32_t mode = (k.tex_modes >> (i * 5)) & 0x1F;
    const TexInfo& t = k.tex[i];
    uint32_t in = i == 1 ? 0 : i == 2 ? (k.input_texture >> 16) & 0xF : (k.input_texture >> 20) & 0xF;
    uint32_t dm = i == 0 ? 0 : (k.dot_mapping >> ((i - 1) * 4)) & 0xF;
    const std::string tc = F("tc%d", i);
    const char* q = tc.c_str();
    const char* zero = Sp(w, "0", "vec4f(0.0)");
    const char* black = Sp(w, "float4(0,0,0,1)", "vec4f(0.0, 0.0, 0.0, 1.0)");
    auto put = [&](const std::string& v) { return F(Sp(w, "float4 t%d = %s;\n", "var t%d: vec4f = %s;\n"), i, v.c_str()); };
    // dpN, or dpNn for stage N + 1's product taken early
    auto dp = [&](int n, int coord, uint32_t map, uint32_t src) {
        return F(Sp(w, "float dp%d%s = dot(tc%d.xyz, %s);\n", "let dp%d%s = dot(tc%d.xyz, %s);\n"), n, coord == n ? "" : "n", coord,
                 DotInput(map, src).c_str());
    };
    auto sample = [&](const std::string& uv) {
        return F(Sp(w, "tex%d.Sample(smp%d, %s)", "textureSample(tex%d, smp%d, %s)"), i, i, uv.c_str());
    };
    auto tex2d = [&](const std::string& uv) {
        if (t.flags & kTexWide)
            return F(Sp(w, "spreadtap(tex%d, smp%d, %s, spread[%d], %s)", "spreadtap(tex%d, smp%d, %s, dc.spread[%d], %s)"), i, i,
                     uv.c_str(), i, w ? "1.0" : Hooks().ReadScale(t).c_str());
        return sample(uv);
    };
    auto cube = [&](const std::string& dir) {
        std::string c = w ? std::string() : Hooks().Cube(t, i, dir);
        return c.empty() ? sample(dir) : c;
    };
    auto sample2 = [&](const std::string& uv) {
        if (t.kind == kTex2D) return tex2d(TexUv(t, i, uv, w));
        if (t.kind == kTexCube) return cube(Sp(w, "float3(", "vec3f(") + uv + Sp(w, ", 1)", ", 1.0)"));
        if (t.kind == kTex3D) return sample(Sp(w, "float3(", "vec3f(") + uv + Sp(w, ", 0)", ", 0.0)"));
        return std::string(black);
    };
    auto sample3 = [&](const std::string& uvw) {
        if (t.kind == kTex2D) return tex2d(TexUv(t, i, "(" + uvw + ").xy", w));
        if (t.kind == kTexCube) return cube(uvw);
        if (t.kind == kTex3D) return sample(uvw);
        return std::string(black);
    };
    auto depth = [&](const std::string& ref) {
        std::string uv = TexUv(t, i, F("%s.xy / %s.w", q, q), w);
        std::string rest = F(Sp(w, ", %s, %d, i.pos.xy, texsize[%d].xy)", ", %s, %d, i.pos.xy, dc.texsize[%d].xy)"), ref.c_str(), t.shadow, i);
        return put(F("shadow(tex%d, smp%d, ", i, i) + uv + rest);
    };
    const char* f2 = Sp(w, "float2", "vec2f");
    const char* f3 = Sp(w, "float3", "vec3f");
    std::string s;
    switch (mode) {
        // PS_TEXTUREMODES: 1 project2d, 2 project3d, 3 cubemap, 4 passthru, 5 clipplane, 6 and 7 bumpenvmap, 9 dot_st, 0xA dot_zw, 0xD dot_str_3d, 0xE dot_str_cube, 0xF dpndnt_ar, 0x10 dpndnt_gb, 0x11 dotproduct
        case 0: return put(black);
        case 1: return t.shadow ? depth("0") : put(sample2(F("%s.xy / %s.w", q, q)));
        case 2: return t.shadow ? depth(F("%s.z / %s.w", q, q)) : put(sample3(F("%s.xyz / %s.w", q, q)));
        case 3: return put(sample3(tc + ".xyz"));
        case 4: return put(tc);
        case 5:  // clip plane: a set compare_mode bit kills at >= 0, a clear one below 0
            s = put(zero);
            for (int j = 0; j < 4; j++)
                s += F(Sp(w, "if (%s.%c %s 0) discard;\n", "if (%s.%c %s 0.0) { discard; }\n"), q, "xyzw"[j],
                       ((k.compare_mode >> (i * 4 + j)) & 1) ? ">=" : "<");
            return s;
        case 6:
        case 7:  // bump env map: signed du dv through the 2x2 matrix, 7 scales by luminance
            s = F(Sp(w, "float2 duv%d = sbyte(t%u.bgr, 0, 256, 127).xy;\n", "let duv%d = sbyte(t%u.bgr, 0, 256, 127).xy;\n"), i, in);
            s += put(sample2(F(Sp(w, "%s.xy + float2(dot(bumpmat[%d].xz, duv%d), dot(bumpmat[%d].yw, duv%d))",
                                     "%s.xy + vec2f(dot(dc.bumpmat[%d].xz, duv%d), dot(dc.bumpmat[%d].yw, duv%d))"),
                               q, i, i, i, i)));
            if (mode == 7)
                s += F(Sp(w, "t%d *= bumplum[%d].x * t%u.r + bumplum[%d].y;\n", "t%d *= dc.bumplum[%d].x * t%u.r + dc.bumplum[%d].y;\n"), i,
                       i, in, i);
            return s;
        case 9: return dp(i, i, dm, in) + put(sample2(F("%s(dp%d, dp%d)", f2, i - 1, i)));
        case 0xA:
        case 0x11: return dp(i, i, dm, in) + put(zero);
        case 0xB:  // reflect diffuse at stage 2: stage 3's product early for the normal
            s = dp(i, i, dm, in) + dp(i, i + 1, (k.dot_mapping >> 8) & 0xF, (k.input_texture >> 20) & 0xF);
            return s + put(sample3(F("%s(dp%d, dp%d, dp%dn)", f3, i - 1, i, i)));
        case 0xC:  // reflect specular at stage 3: eye from the three w
            s = dp(i, i, dm, in);
            if (!w)
                s += F("float3 nrm%d = float3(dp%d, dp%d, dp%d), eye%d = float3(tc%d.w, tc%d.w, tc%d.w);\n", i, i - 2, i - 1, i, i, i - 2,
                       i - 1, i);
            else
                s += F("let nrm%d = vec3f(dp%d, dp%d, dp%d);\nlet eye%d = vec3f(tc%d.w, tc%d.w, tc%d.w);\n", i, i - 2, i - 1, i, i, i - 2,
                       i - 1, i);
            s += F(Sp(w, "float3 refl%d = 2 * nrm%d * dot(nrm%d, eye%d) / dot(nrm%d, nrm%d) - eye%d;\n",
                      "let refl%d = 2 * nrm%d * dot(nrm%d, eye%d) / dot(nrm%d, nrm%d) - eye%d;\n"),
                   i, i, i, i, i, i, i);
            return s + put(sample3(F("refl%d", i)));
        case 0xD:
        case 0xE: return dp(i, i, dm, in) + put(sample3(F("%s(dp%d, dp%d, dp%d)", f3, i - 2, i - 1, i)));
        case 0xF: return put(sample2(F("t%u.ar", in)));
        case 0x10: return put(sample2(F("t%u.gb", in)));
        default: return put(zero);
    }
}

const char* kPsSbyte = R"(
// unorm to byte b, less lo under 128 or hi from 128, over d
float3 sbyte(float3 u, float lo, float hi, float d) { float3 b = u * 255; return (b - (b >= 128 ? hi : lo)) / d; }
)";
const char* kPsTail = R"(float4 spreadtap(Texture2D t, SamplerState s, float2 uv, float4 w, float m) {
    float4 c = 0;
    int n = (int)w.z;
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++) {
            float4 v = t.SampleLevel(s, uv + w.xy * (float2(x, y) - (n - 1) * 0.5), 0);
            c += float4(saturate(v.rgb * m), v.a);
        }
    return c / (n * n);
}
void alphatest(float a) {
    int f = (int)misc.y;
    int v = (int)round(a * 255), r = (int)misc.x;
    bool ok = f == 0 ? false : f == 1 ? v < r : f == 2 ? v == r : f == 3 ? v <= r : f == 4 ? v > r : f == 5 ? v != r : f == 6 ? v >= r : true;
    if (!ok) discard;
}
)";

std::string PsHelpers() { return kPsSbyte + Hooks().Shadow() + kPsTail; }

// the same helpers in WGSL; shadow is ShaderHooks::Shadow's default with reversed depth (1 - z)
const char* kWgslPsHelpers = R"(
// unorm to byte b, less lo under 128 or hi from 128, over d
fn sbyte(u: vec3f, lo: f32, hi: f32, d: f32) -> vec3f {
    let b = u * 255.0;
    return (b - select(vec3f(lo), vec3f(hi), b >= vec3f(128.0))) / d;
}
// the sampler compares, a linear filter blends the four results like the console does
fn shadow(t: texture_depth_2d, s: sampler_comparison, uv: vec2f, z: f32, fmt: i32, px: vec2f, size: vec2f) -> vec4f {
    let mx = select(16777215.0, 65535.0, fmt == 2);
    return vec4f(textureSampleCompareLevel(t, s, uv, 1.0 - clamp(z, 0.0, mx) / mx));
}
fn spreadtap(t: texture_2d<f32>, s: sampler, uv: vec2f, w: vec4f, m: f32) -> vec4f {
    var c = vec4f(0.0);
    let n = i32(w.z);
    for (var y = 0; y < n; y++) {
        for (var x = 0; x < n; x++) {
            let v = textureSampleLevel(t, s, uv + w.xy * (vec2f(f32(x), f32(y)) - f32(n - 1) * 0.5), 0.0);
            c += vec4f(saturate(v.rgb * m), v.a);
        }
    }
    return c / f32(n * n);
}
fn alphatest(a: f32) {
    let f = i32(dc.misc.y);
    let v = i32(round(a * 255.0));
    let r = i32(dc.misc.x);
    var ok = true;
    if (f == 0) { ok = false; } else if (f == 1) { ok = v < r; } else if (f == 2) { ok = v == r; } else if (f == 3) { ok = v <= r; }
    else if (f == 4) { ok = v > r; } else if (f == 5) { ok = v != r; } else if (f == 6) { ok = v >= r; }
    if (!ok) { discard; }
}
)";

// fixed function texture stage arguments, D3DTA
std::string FfArg(uint32_t a, int stage, bool alpha, bool w = false) {
    std::string v;
    switch (a & 0xF) {
        case 0: v = "v0"; break;
        case 1: v = "cur"; break;
        case 2: v = F("t%d", stage); break;
        case 3: v = Sp(w, "tfactor", "dc.tfactor"); break;
        case 4: v = "v1"; break;
        case 5: v = "tmp"; break;
        default: v = "cur"; break;
    }
    if (a & 0x20) v = "(" + v + ").aaaa";
    if (a & 0x10) v = "(1 - " + v + ")";
    return "(" + v + ")" + (alpha ? ".a" : ".rgb");
}

// D3DTOP
std::string FfOp(uint32_t op, const std::string& a1, const std::string& a2, const std::string& a0, int stage, bool alpha,
                 bool w = false) {
    const std::string lerp = Sp(w, "lerp(", "mix(");
    switch (op) {
        case 2: return a1;
        case 3: return a2;
        case 4: return a1 + " * " + a2;
        case 5: return a1 + " * " + a2 + " * 2";
        case 6: return a1 + " * " + a2 + " * 4";
        case 7: return a1 + " + " + a2;
        case 8: return a1 + " + " + a2 + " - 0.5";
        case 9: return "(" + a1 + " + " + a2 + " - 0.5) * 2";
        case 10: return a1 + " - " + a2;
        case 11: return a1 + " + " + a2 + " * (1 - " + a1 + ")";
        case 12: return lerp + a2 + ", " + a1 + ", v0.a)";
        case 13: return lerp + a2 + ", " + a1 + ", cur.a)";
        case 14: return lerp + F("%s, %s, t%d.a)", a2.c_str(), a1.c_str(), stage);
        case 15: return lerp + a2 + ", " + a1 + Sp(w, ", tfactor.a)", ", dc.tfactor.a)");
        case 16: return F("%s + %s * (1 - t%d.a)", a1.c_str(), a2.c_str(), stage);
        case 18: return alpha ? a1 : a1 + " + " + a2 + " * " + a1.substr(0, a1.size() - 4) + ".a";
        case 19: return alpha ? a1 : a1 + " * " + a2 + " + " + a1.substr(0, a1.size() - 4) + ".a";
        case 20: return alpha ? a1 : a1 + " + " + a2 + " * (1 - " + a1.substr(0, a1.size() - 4) + ".a)";
        case 21: return alpha ? a1 : "(1 - " + a1 + ") * " + a2 + " + " + a1.substr(0, a1.size() - 4) + ".a";
        case 22: {
            std::string f3 = Sp(w, "float3(", "vec3f(");
            return F("saturate(dot((%s - 0.5) * 2, (%s - 0.5) * 2))", alpha ? (f3 + a1 + ",0,0)").c_str() : a1.c_str(),
                     alpha ? (f3 + a2 + ",0,0)").c_str() : a2.c_str());
        }
        case 23: return a0 + " + " + a1 + " * " + a2;
        case 24: return lerp + a2 + ", " + a1 + ", " + a0 + ")";
        default: return alpha ? "cur.a" : "cur.rgb";
    }
}

}  // namespace

bool PixelKey::operator==(const PixelKey& o) const { return std::memcmp(this, &o, sizeof(*this)) == 0; }

std::string ShaderHooks::Constants() const { return "    float4 ext;\n"; }

std::string ShaderHooks::Shadow() const {
    std::string r = g_reversed_depth ? "1 - clamp(z, 0, mx) / mx" : "clamp(z, 0, mx) / mx";
    return R"(// the sampler compares, a linear filter blends the four results like the console does
float4 shadow(Texture2D<float> t, SamplerComparisonState s, float2 uv, float z, int fmt, float2 px, float2 size) {
    float mx = fmt == 2 ? 65535.0 : 16777215.0;
    return t.SampleCmpLevelZero(s, uv, )" + r + R"().xxxx;
}
)";
}

std::string ShaderHooks::Output() const { return "outc.rgb = saturate(outc.rgb);\n"; }

uint64_t Hash(const void* p, size_t n, uint64_t h) { return FastHash(p, n, h); }

namespace {

// D3DTOP and D3DTA values PixelKey::ff holds
constexpr uint32_t kTopSelectArg1 = 2;
constexpr uint32_t kTopSelectArg2 = 3;
constexpr uint32_t kTopBlendTextureAlpha = 14;
constexpr uint32_t kTopBlendTextureAlphaPm = 16;
constexpr uint32_t kTopModulateAlphaAddColor = 18;
constexpr uint32_t kTopModulateInvColorAddAlpha = 21;
constexpr uint32_t kTopMultiplyAdd = 23;
constexpr uint32_t kTopLerp = 24;
constexpr uint32_t kTaCurrent = 1;
constexpr uint32_t kTaTexture = 2;

// args in PixelKey::ff order: arg1, arg2, arg0
bool ReadsTexture(uint32_t op, const uint32_t* args, bool alpha) {
    if (op == kTopBlendTextureAlpha || op == kTopBlendTextureAlphaPm) return true;
    bool alpha_arg1_only = alpha && op >= kTopModulateAlphaAddColor && op <= kTopModulateInvColorAddAlpha;
    bool uses_arg1 = op != kTopSelectArg2;
    bool uses_arg2 = op != kTopSelectArg1 && !alpha_arg1_only;
    bool uses_arg0 = op == kTopMultiplyAdd || op == kTopLerp;
    auto texture = [](uint32_t a) { return (a & 0xF) == kTaTexture; };
    return (uses_arg1 && texture(args[0])) || (uses_arg2 && texture(args[1])) || (uses_arg0 && texture(args[2]));
}

// the XDK's combiner builder turns a stage op that reads a missing texture into a pass of the current colour
void PassMissingTexture(uint32_t* f) {
    if (f[0] > 1 && ReadsTexture(f[0], f + 1, false)) {
        f[0] = kTopSelectArg1;
        f[1] = kTaCurrent;
    }
    if (f[4] > 1 && ReadsTexture(f[4], f + 5, true)) {
        f[4] = kTopSelectArg1;
        f[5] = kTaCurrent;
    }
}

}  // namespace

PixelKey MakePixelKey(const State& s, const TexInfo tex[4]) {
    PixelKey k;
    std::memset(&k, 0, sizeof(k));
    for (int i = 0; i < 4; i++) {
        k.tex[i] = tex[i];
        if (s.tss[size_t(i)][kTssAlphaKill] == 4) k.alphakill |= 1u << i;  // D3DTALPHAKILL_ENABLE
    }
    k.flat = s.simple[kMShadeMode >> 2] == 0x1D00;  // D3DSHADE_FLAT
    k.two_sided = s.rs[kRsTwoSidedLighting] != 0;
    k.alphatest = s.simple[kMAlphaTestEnable >> 2] != 0;
    if (s.ps) {
        const uint32_t* d = s.ps->def;
        k.programmable = 1;
        // D3DPIXELSHADERDEF dwords: alpha inputs 0, final 8 and 9, alpha outputs 26, rgb inputs 34, compare mode 42, rgb outputs 45, count 53, dot mapping 55, input texture 56
        k.count = d[53];
        for (int i = 0; i < 8; i++) {
            k.a_in[i] = d[i];
            k.a_out[i] = d[26 + i];
            k.rgb_in[i] = d[34 + i];
            k.rgb_out[i] = d[45 + i];
        }
        k.final_abcd = d[8];
        k.final_efg = d[9];
        // the stage modes follow the bound textures, as D3D's lazy shader stage setup does
        uint32_t modes = s.rs[kRsPsTextureModes];
        for (int i = 0; i < 4; i++) {
            uint32_t m = (modes >> (i * 5)) & 0x1F;
            const TexInfo& t = tex[i];
            if (!t.kind && m != 4 && m != 5 && m != 0xA && m != 0x11) m = 0;
            else if (m >= 1 && m <= 3) m = t.kind == kTexCube ? 3 : (t.kind == kTex3D || t.shadow) ? 2 : 1;
            else if (m == 0xD || m == 0xE) m = t.kind == kTexCube ? 0xE : 0xD;
            k.tex_modes |= m << (i * 5);
        }
        k.dot_mapping = d[55];
        k.input_texture = d[56];
        k.compare_mode = d[42];
        return k;
    }
    // the console's stages divide by q; a vertex program's q is its own
    bool program = (s.vs_handle & 1) && s.vs && !s.vs->program.empty();
    for (int i = 0; i < 4; i++) {
        const auto& t = s.tss[size_t(i)];
        // A fixed-function stage selecting a missing texture terminates the cascade.
        // Otherwise a later unbound SELECTARG1 stage replaces the movie/UI with the white fallback texture.
        if (!tex[i].kind && (t[kTssColorArg1] & 0xF) == 2) break;  // D3DTA_TEXTURE
        uint32_t v[9] = {t[kTssColorOp], t[kTssColorArg1], t[kTssColorArg2], t[kTssColorArg0], t[kTssAlphaOp],
                         t[kTssAlphaArg1],  t[kTssAlphaArg2], t[kTssAlphaArg0], t[kTssResultArg]};
        std::memcpy(k.ff[i], v, sizeof(v));
        if (!tex[i].kind) PassMissingTexture(k.ff[i]);
        if (program || (t[kTssTextureTransformFlags] & 0x100)) k.proj |= 1u << i;  // D3DTTFF_PROJECTED
        if (t[kTssColorOp] <= 1) break;
    }
    return k;
}

constexpr uint32_t kMuxConstant = 3;
constexpr uint32_t kViewportOffsetConstant = 59;  // c-37: constants start at c-96

bool ReadsViewportOffset(const std::vector<uint32_t>& prog) {
    for (size_t n = 0; n + 4 <= prog.size(); n += 4) {
        Tok k{&prog[n]};
        // an instruction reads at most one constant, through any of its operands
        bool constant = k.f(2, 26, 2) == kMuxConstant || k.f(2, 11, 2) == kMuxConstant || k.f(3, 28, 2) == kMuxConstant;
        bool relative = k.f(3, 1, 1);
        if (constant && !relative && k.f(1, 13, 8) == kViewportOffsetConstant) return true;
    }
    return false;
}

std::string VertexProgramHlsl(const std::vector<uint32_t>& prog, uint16_t regs, uint64_t decode, bool velocity) {
    std::string s = Common();
    s += kVsHelpers;
    std::string inputs = VsInputs(regs, decode);
    const size_t body_start = s.size() + inputs.size() + std::strlen(kVsPrologue);
    s += inputs;
    s += kVsPrologue;
    for (size_t n = 0; n + 4 <= prog.size(); n += 4) {
        Tok k{&prog[n]};
        uint32_t mac = k.f(1, 21, 4), ilu = k.f(1, 25, 3);
        if (mac || ilu) {
            std::string a = Src(k, 0, regs), b = Src(k, 1, regs), c = Src(k, 2, regs);
            s += "    {\n";
            if (mac) s += "        precise float4 mr = " + MacExpr(mac, a, b, c) + ";\n";
            if (ilu) s += "        precise float4 ir = " + IluExpr(ilu, c) + ";\n";
            uint32_t r = k.f(3, 20, 4), omask = k.f(3, 12, 4);
            if (mac == 13) {
                s += "        a0 = mac_arl(mr.x);\n";
            } else if (mac) {
                uint32_t m = k.f(3, 24, 4);
                if (ilu && r == 1) m = 0;
                if (m && r <= 12) s += F("        %s.%s = mr.%s;\n", RegDest(r).c_str(), kMask[m], kMask[m]);
            }
            if (ilu) {
                uint32_t m = k.f(3, 16, 4), ir = mac ? 1 : r;
                if (m && ir <= 12) s += F("        %s.%s = ir.%s;\n", RegDest(ir).c_str(), kMask[m], kMask[m]);
            }
            bool use_ilu = k.f(3, 2, 1) == 1;
            if (omask && k.f(3, 11, 1) && ((use_ilu && ilu) || (!use_ilu && mac && mac != 13))) {
                uint32_t o = k.f(3, 3, 8) & 0xF;
                const char* src = use_ilu ? "ir" : "mr";
                if (kOut[o][0]) {
                    const char* msk = o == 5 ? kLanes[std::popcount(omask)] : kMask[omask];
                    s += F("        %s.%s = %s.%s;\n", kOut[o], msk, src, msk);
                }
            }
            s += "    }\n";
        }
        if (k.f(3, 0, 1)) break;  // last instruction
    }
    if (velocity) {
        // the same program on the previous frame's constants, for DLSS's motion vectors: only oPos is used, the
        // compiler drops the rest
        std::string body = s.substr(body_start), prev;
        for (size_t i = 0; i < body.size(); i++) {
            bool word = i && (std::isalnum(static_cast<unsigned char>(body[i - 1])) || body[i - 1] == '_');
            if (!word && body.compare(i, 2, "c[") == 0) {
                prev += "cprev[";
                i++;
            } else {
                prev += body[i];
            }
        }
        std::string fn = "cbuffer PV : register(b2) { float4 cprev[192]; };\nfloat4 PrevPos(VIn i) {" +
                         std::string(kVsPrologue) + prev + "    return oPos;\n}\n";
        s.insert(s.find(inputs) + inputs.find("VOut main("), fn);
    }
    s += kVsEpilogue;
    return s;
}

// WGSL for WebGPU (the browser build): the same decoding of the microcode as VertexProgramHlsl, spelled for WGSL. No
// multi-lane swizzle writes (one lane at a time), no scalar swizzles (vec4f(x)), select for vector conditions, infinity
// by bitcast; @invariant on the position stands for precise. VC is c (binding 0), DC is dc (binding 1), same layout.
// DC's matrices are only declared here: HLSL's column_major float4x4 and WGSL's mat4x4f index the other way round.
namespace {

const char* kWgslCommon = R"(
struct VOut {
    @invariant @builtin(position) pos: vec4f,
    @location(0) d0: vec4f,
    @location(1) d1: vec4f,
    @location(2) b0: vec4f,
    @location(3) b1: vec4f,
    @location(4) tc0: vec4f,
    @location(5) tc1: vec4f,
    @location(6) tc2: vec4f,
    @location(7) tc3: vec4f,
    @location(8) fogamt: f32,
};
struct DC {
    surf: vec4f,
    fogp: vec4f,
    fogcolor: vec4f,
    misc: vec4f,
    pc: array<vec4f, 18>,
    tfactor: vec4f,
    texsize: array<vec4f, 4>,
    bumpmat: array<vec4f, 4>,
    bumplum: array<vec4f, 4>,
    wvp: mat4x4f,
    ext: vec4f,
    spread: array<vec4f, 4>,
    wv: mat4x4f,
    wvn: mat4x4f,
    tm: array<mat4x4f, 4>,
    mat: array<vec4f, 5>,
    amb: vec4f,
    lt: array<vec4f, 56>,
    rtshift: vec4f,
    vattr: array<vec4f, 16>,
    sblend: vec4f,
    bwvp: array<mat4x4f, 4>,
    bwv: array<mat4x4f, 4>,
    bwvn: array<mat4x4f, 4>,
    carpaint: vec4f,
    pbr: vec4f,
    pbr2: vec4f,
    zparams: vec4f,
    taa: vec4f,
};
@group(0) @binding(0) var<uniform> c: array<vec4f, 192>;
@group(0) @binding(1) var<uniform> dc: DC;
)";

const char* kWgslVsHelpers = R"(
fn inf() -> f32 { var bits = 0x7f800000u; return bitcast<f32>(bits); }
fn mac_mul(a: vec4f, b: vec4f) -> vec4f { return select(a * b, vec4f(0.0), (a == vec4f(0.0)) | (b == vec4f(0.0))); }
fn mac_arl(v: f32) -> i32 { return i32(floor(v + 1e-3)); }
// magnitude clamped between 1 / 2^64 and 2^64, sign kept
fn magclamp(v: f32) -> f32 { let m = clamp(abs(v), 5.42101e-20, 1.84467e+19); return select(m, -m, v < 0.0); }
fn ilu_rcp(v: f32) -> vec4f { return vec4f(1.0 / v); }
fn ilu_rcc(v: f32) -> vec4f { return vec4f(magclamp(1.0 / v)); }
fn ilu_rsq(v: f32) -> vec4f {
    if (v == 0.0) { return vec4f(inf()); }
    return vec4f(inverseSqrt(abs(v)));
}
// 2^floor(v), fraction, 2^v
fn ilu_exp(v: f32) -> vec4f {
    let whole = floor(v);
    return vec4f(exp2(whole), v - whole, exp2(v), 1.0);
}
// exponent, mantissa, log2 of |v|; zero logs to negative infinity
fn ilu_log(v: f32) -> vec4f {
    let mag = abs(v);
    if (mag == 0.0) { return vec4f(-inf(), 1.0, -inf(), 1.0); }
    let l2 = log2(mag);
    let ex = floor(l2);
    return vec4f(ex, mag / exp2(ex), l2, 1.0);
}
// power clamped to 128 less 1/256
fn ilu_lit(s: vec4f) -> vec4f {
    let cap = 127.99609375;
    let diffuse = max(s.x, 0.0);
    let spec = exp2(clamp(s.w, -cap, cap) * log2(max(s.y, 0.0)));
    return vec4f(1.0, diffuse, select(0.0, spec, diffuse > 0.0), 1.0);
}
fn fogfactor(d: f32) -> f32 {
    if (dc.fogp.w == 3.0) { return (dc.fogp.y - d) / (dc.fogp.y - dc.fogp.x); }
    if (dc.fogp.w == 1.0) { return exp(-d * dc.fogp.z); }
    if (dc.fogp.w == 2.0) { return exp(-(d * dc.fogp.z) * (d * dc.fogp.z)); }
    return 1.0;
}
)";

const char* kWgslVsPrologue = R"(
    var a0: i32 = 0;
    var R0 = vec4f(0.0); var R1 = vec4f(0.0); var R2 = vec4f(0.0); var R3 = vec4f(0.0);
    var R4 = vec4f(0.0); var R5 = vec4f(0.0); var R6 = vec4f(0.0); var R7 = vec4f(0.0);
    var R8 = vec4f(0.0); var R9 = vec4f(0.0); var R10 = vec4f(0.0); var R11 = vec4f(0.0);
    var oPos = vec4f(0.0);
    var oD0 = vec4f(0.0, 0.0, 0.0, 1.0); var oD1 = vec4f(0.0); var oB0 = vec4f(0.0, 0.0, 0.0, 1.0); var oB1 = vec4f(0.0);
    var oFog = vec4f(0.0); var oPts = vec4f(1.0);
    var oT0 = vec4f(0.0, 0.0, 0.0, 1.0); var oT1 = vec4f(0.0, 0.0, 0.0, 1.0);
    var oT2 = vec4f(0.0, 0.0, 0.0, 1.0); var oT3 = vec4f(0.0, 0.0, 0.0, 1.0);
)";

const char* kWgslVsEpilogue = R"(
    var o: VOut;
    var p = oPos;
    p.w = magclamp(p.w);
    // full screen passes drawn half a pixel up-left stop half a pixel short of the far edges (kVsEpilogue)
    if (dc.surf.w == 0.0 && abs(p.x - (dc.surf.x - 0.5)) < 1e-3) { p.x = dc.surf.x; }
    if (dc.surf.w == 0.0 && abs(p.y - (dc.surf.y - 0.5)) < 1e-3) { p.y = dc.surf.y; }
    var q: vec4f;
    q.x = (p.x * 2.0 / dc.surf.x - 1.0) * p.w;
    q.y = (1.0 - p.y * 2.0 / dc.surf.y) * p.w;
    q.z = p.z * dc.surf.z * p.w;
    q.w = p.w;
    o.pos = q;
    o.d0 = saturate(oD0);
    o.d1 = saturate(oD1);
    o.b0 = saturate(oB0);
    o.b1 = saturate(oB1);
    o.tc0 = oT0;
    o.tc1 = oT1;
    o.tc2 = oT2;
    o.tc3 = oT3;
    o.fogamt = fogfactor(oFog.x);
    return o;
}
)";

std::string MacWgsl(uint32_t op, const std::string& a, const std::string& b, const std::string& c) {
    switch (op) {
        case 1: return a;
        case 2: return "mac_mul(" + a + ", " + b + ")";
        case 3: return "(" + a + " + " + c + ")";
        case 4: return "(mac_mul(" + a + ", " + b + ") + " + c + ")";
        case 5: return "vec4f(dot(" + a + ".xyz, " + b + ".xyz))";
        case 6: return "vec4f(dot(vec4f(" + a + ".xyz, 1.0), " + b + "))";
        case 7: return "vec4f(dot(" + a + ", " + b + "))";
        case 8: return "vec4f(1.0, " + a + ".y * " + b + ".y, " + a + ".z, " + b + ".w)";
        case 9: return "min(" + a + ", " + b + ")";
        case 10: return "max(" + a + ", " + b + ")";
        case 11: return "select(vec4f(0.0), vec4f(1.0), " + a + " < " + b + ")";
        case 12: return "select(vec4f(0.0), vec4f(1.0), " + a + " >= " + b + ")";
        case 13: return a;
        default: return "vec4f(0.0)";
    }
}

std::string IluWgsl(uint32_t op, const std::string& c) {
    switch (op) {
        case 1: return c;
        case 2: return "ilu_rcp(" + c + ".x)";
        case 3: return "ilu_rcc(" + c + ".x)";
        case 4: return "ilu_rsq(" + c + ".x)";
        case 5: return "ilu_exp(" + c + ".x)";
        case 6: return "ilu_log(" + c + ".x)";
        case 7: return "ilu_lit(" + c + ")";
        default: return "vec4f(0.0)";
    }
}

// dest.lanes = src.lanes, one lane at a time
std::string LaneWrites(const std::string& dest, const char* lanes, const char* src) {
    std::string s;
    for (const char* l = lanes; *l; l++) s += F("        %s.%c = %s.%c;\n", dest.c_str(), *l, src, *l);
    return s;
}

std::string WgslVsInputs(uint16_t regs, uint64_t decode) {
    if (!regs) return "struct VIn {\n    @builtin(vertex_index) id: u32,\n};\n@vertex\nfn main(i: VIn) -> VOut {\n";
    if (!decode) {
        std::string s = "struct VIn {\n";
        for (int r = 0; r < 16; r++)
            if ((regs >> r) & 1) s += F("    @location(%d) v%d: vec4f,\n", r, r);
        return s + "};\n@vertex\nfn main(i: VIn) -> VOut {\n";
    }
    std::string raw = "struct VRaw {\n", in = "struct VIn {\n", body;
    bool packed = false;
    for (int r = 0; r < 16; r++) {
        if (!((regs >> r) & 1)) continue;
        uint32_t d = uint32_t(decode >> (r * 4)) & 15;
        const char* type = d == kDecodeInt || d == kDecodeIntW1 ? "vec4i" : d == kDecodePacked ? "u32" : "vec4f";
        raw += F("    @location(%d) v%d: %s,\n", r, r, type);
        in += F("    v%d: vec4f,\n", r);
        packed |= d == kDecodePacked;
        switch (d) {
            case kDecodeW1: body += F("    i.v%d = vec4f(ri.v%d.xyz, 1.0);\n", r, r); break;
            case kDecodeInt: body += F("    i.v%d = vec4f(ri.v%d);\n", r, r); break;
            case kDecodeIntW1: body += F("    i.v%d = vec4f(vec3f(ri.v%d.xyz), 1.0);\n", r, r); break;
            case kDecodePacked: body += F("    i.v%d = UnpackNp3(ri.v%d);\n", r, r); break;
            case kDecodeFloat2H: body += F("    i.v%d = vec4f(ri.v%d.xy, 0.0, ri.v%d.z);\n", r, r, r); break;
            default: body += F("    i.v%d = ri.v%d;\n", r, r); break;
        }
    }
    std::string s = raw + "};\n" + in + "};\n";
    if (packed)
        s += "fn UnpackNp3(u: u32) -> vec4f {\n"
             "    let v = vec3i(i32(u << 21u) >> 21u, i32(u << 10u) >> 21u, i32(u) >> 22u);\n"
             "    return vec4f(max(vec3f(v) / vec3f(1023.0, 1023.0, 511.0), vec3f(-1.0)), 1.0);\n"
             "}\n";
    return s + "@vertex\nfn main(ri: VRaw) -> VOut {\n    var i: VIn;\n" + body;
}

}  // namespace

std::string VertexProgramWgsl(const std::vector<uint32_t>& prog, uint16_t regs, uint64_t decode) {
    std::string s = kWgslCommon;
    s += kWgslVsHelpers;
    s += WgslVsInputs(regs, decode);
    s += kWgslVsPrologue;
    for (size_t n = 0; n + 4 <= prog.size(); n += 4) {
        Tok k{&prog[n]};
        uint32_t mac = k.f(1, 21, 4), ilu = k.f(1, 25, 3);
        if (mac || ilu) {
            std::string a = Src(k, 0, regs, true), b = Src(k, 1, regs, true), c = Src(k, 2, regs, true);
            s += "    {\n";
            if (mac) s += "        let mr = " + MacWgsl(mac, a, b, c) + ";\n";
            if (ilu) s += "        let ir = " + IluWgsl(ilu, c) + ";\n";
            uint32_t r = k.f(3, 20, 4), omask = k.f(3, 12, 4);
            if (mac == 13) {
                s += "        a0 = mac_arl(mr.x);\n";
            } else if (mac) {
                uint32_t m = k.f(3, 24, 4);
                if (ilu && r == 1) m = 0;
                if (m && r <= 12) s += LaneWrites(RegDest(r), kMask[m], "mr");
            }
            if (ilu) {
                uint32_t m = k.f(3, 16, 4), ir = mac ? 1 : r;
                if (m && ir <= 12) s += LaneWrites(RegDest(ir), kMask[m], "ir");
            }
            bool use_ilu = k.f(3, 2, 1) == 1;
            if (omask && k.f(3, 11, 1) && ((use_ilu && ilu) || (!use_ilu && mac && mac != 13))) {
                uint32_t o = k.f(3, 3, 8) & 0xF;
                if (kOut[o][0])
                    s += LaneWrites(kOut[o], o == 5 ? kLanes[std::popcount(omask)] : kMask[omask], use_ilu ? "ir" : "mr");
            }
            s += "    }\n";
        }
        if (k.f(3, 0, 1)) break;  // last instruction
    }
    s += kWgslVsEpilogue;
    return s;
}

FixedKey MakeFixedKey(const State& s) {
    FixedKey f;
    f.blend = s.rs[kRsVertexBlend] & 7;
    if (s.rs[kRsLighting]) {
        f.light = 1 | (s.rs[kRsSpecularEnable] ? 2 : 0) | (s.rs[kRsLocalViewer] ? 4 : 0) | (s.rs[kRsNormalizeNormals] ? 8 : 0);
        if (s.rs[kRsColorVertex])
            f.light |= (s.rs[kRsDiffuseMaterialSource] & 3) << 8 | (s.rs[kRsSpecularMaterialSource] & 3) << 10 |
                       (s.rs[kRsAmbientMaterialSource] & 3) << 12 | (s.rs[kRsEmissiveMaterialSource] & 3) << 14;
        for (int i = 0; i < 8; i++)
            if ((s.light_enable >> i) & 1) f.types |= (s.lights[size_t(i)][0] & 3) << (i * 2);
    }
    for (int t = 0; t < 4; t++) {
        uint32_t tci = s.tss[size_t(t)][kTssTexCoordIndex], ttf = s.tss[size_t(t)][kTssTextureTransformFlags];
        uint32_t set = tci & 3, size = 2;
        if (s.vs && (s.vs_handle & 1)) {
            uint32_t ty = s.vs->inputs[9 + set].type;
            size = ty == 0x16 ? 3 : std::min<uint32_t>(ty >> 4, 4);  // 0x16 NORMPACKED3
            if (ty >> 4 > 4) size = 3;
            if (!size) size = 2;
        } else {
            uint32_t code = (s.vs_handle >> (16 + set * 2)) & 3;
            size = code == 0 ? 2 : code == 1 ? 3 : code == 2 ? 4 : 1;
        }
        f.tex[t] = set | ((tci >> 16) & 7) << 4 | (ttf & 7) << 8 | (ttf & 0x100 ? 0x800 : 0) | size << 12;
    }
    return f;
}

namespace {

const char* kFixedTailHlsl = R"(
    VOut o;
    // like the pretransformed ones, target sized passes end half a console pixel short of the far edges
    float2 sp = (float2(cp.x, -cp.y) / cp.w + 1) * 0.5 * surf.xy;
    if (surf.w == 0 && abs(sp.x - (surf.x - 0.5)) < 1e-3) cp.x = cp.w;
    if (surf.w == 0 && abs(sp.y - (surf.y - 0.5)) < 1e-3) cp.y = -cp.w;
    o.pos = cp;
    o.d0 = saturate(oD0);
    o.d1 = saturate(oD1);
    o.b0 = o.d0;
    o.b1 = o.d1;
    o.tc0 = oT0;
    o.tc1 = oT1;
    o.tc2 = oT2;
    o.tc3 = oT3;
    o.fogamt = fogfactor(abs(cp.z));
    return o;
}
)";

const char* kFixedTailWgsl = R"(
    var o: VOut;
    // like the pretransformed ones, target sized passes end half a console pixel short of the far edges
    let sp = (vec2f(cp.x, -cp.y) / cp.w + 1.0) * 0.5 * dc.surf.xy;
    if (dc.surf.w == 0.0 && abs(sp.x - (dc.surf.x - 0.5)) < 1e-3) { cp.x = cp.w; }
    if (dc.surf.w == 0.0 && abs(sp.y - (dc.surf.y - 0.5)) < 1e-3) { cp.y = -cp.w; }
    o.pos = cp;
    o.d0 = saturate(oD0);
    o.d1 = saturate(oD1);
    o.b0 = o.d0;
    o.b1 = o.d1;
    o.tc0 = oT0;
    o.tc1 = oT1;
    o.tc2 = oT2;
    o.tc3 = oT3;
    o.fogamt = fogfactor(abs(cp.z));
    return o;
}
)";

// D3D's fixed function vertex pipeline: transform, lighting in view space, texture coordinate generation and
// transforms. w: WGSL, where mul(v, M) of HLSL's column_major matrices is v * M
std::string FixedVertex(uint16_t regs, bool pretransformed, uint64_t decode, const FixedKey& f, bool w) {
    std::string s = w ? std::string(kWgslCommon) + kWgslVsHelpers + WgslVsInputs(regs, decode) + kWgslVsPrologue
                      : Common() + kVsHelpers + VsInputs(regs, decode) + kVsPrologue;
    const char* f4 = Sp(w, "float4", "vec4f");
    const char* dcp = Sp(w, "", "dc.");  // DC members
    // a register the vertices lack reads its inline value, as on the console; position has none worth reading
    auto v = [&](int r, const char* def) {
        return (regs >> r) & 1 ? F("i.v%d", r) : r == 0 ? std::string(def) : F(Sp(w, "vattr[%d]", "dc.vattr[%d]"), r);
    };
    const char* black = Sp(w, "float4(0,0,0,1)", "vec4f(0.0, 0.0, 0.0, 1.0)");
    const char* clear = Sp(w, "float4(0,0,0,0)", "vec4f(0.0)");
    // only vertices that carry a normal (register 2) are lit: geometry drawn without one while D3DRS_LIGHTING is
    // still on from a lit draw keeps its vertex diffuse colour unlit, per D3D8
    bool lit = !pretransformed && (f.light & 1) && ((regs >> 2) & 1);
    if (pretransformed) {
        if (!w) {
            s += "    float4 rhw = " + v(0, black) + ";\n";
            s += "    float w = rhw.w == 0 ? 1 : 1 / rhw.w;\n";
            s += "    oPos = float4(rhw.xy, rhw.z / surf.z, w);\n";
        } else {
            s += "    let rhw = " + v(0, black) + ";\n";
            s += "    let w = select(1.0 / rhw.w, 1.0, rhw.w == 0.0);\n";
            s += "    oPos = vec4f(rhw.xy, rhw.z / dc.surf.z, w);\n";
        }
    } else {
        s += Sp(w, "    float4 ip = float4(", "    let ip = vec4f(") + v(0, black) + Sp(w, ".xyz, 1);\n", ".xyz, 1.0);\n");
        // only vertices that carry blend weights (register 1) are blended: geometry drawn without them while the
        // state is still on from a skinned draw keeps its plain world transform
        if (f.blend >= 1 && f.blend <= 6 && ((regs >> 1) & 1)) {
            // the console's vertex blending: the weights in register 1 mix world 0 to 3; odd modes leave the last
            // weight to make the sum one
            uint32_t n = (f.blend + 3) / 2;
            bool implied = f.blend & 1;
            if (!w) {
                s += "    float4 bw = " + v(1, clear) + ";\n";
                s += "    float bwt[4] = {bw.x, bw.y, bw.z, bw.w};\n";
                if (implied) {
                    s += "    float bsum = 0;\n";
                    s += F("    for (int k = 0; k < %u; k++) bsum += bwt[k];\n", n - 1);
                    s += F("    bwt[%u] = 1 - bsum;\n", n - 1);
                }
                s += "    float4 cp = 0;\n    float3 vp = 0, vn = 0;\n";
                s += "    float4 inrm = float4(" + v(2, clear) + ".xyz, 0);\n";
                s += F("    for (int k = 0; k < %u; k++) {\n", n);
                s += "        cp += bwt[k] * mul(ip, bwvp[k]);\n";
                s += "        vp += bwt[k] * mul(ip, bwv[k]).xyz;\n";
                s += "        vn += bwt[k] * mul(inrm, bwvn[k]).xyz;\n";
                s += "    }\n";
            } else {
                s += "    let bw = " + v(1, clear) + ";\n";
                s += "    var bwt = array<f32, 4>(bw.x, bw.y, bw.z, bw.w);\n";
                if (implied) {
                    s += "    var bsum = 0.0;\n";
                    s += F("    for (var k = 0; k < %u; k++) { bsum += bwt[k]; }\n", n - 1);
                    s += F("    bwt[%u] = 1.0 - bsum;\n", n - 1);
                }
                s += "    var cp = vec4f(0.0);\n    var vp = vec3f(0.0);\n    var vn = vec3f(0.0);\n";
                s += "    let inrm = vec4f(" + v(2, clear) + ".xyz, 0.0);\n";
                s += F("    for (var k = 0; k < %u; k++) {\n", n);
                s += "        cp += bwt[k] * (ip * dc.bwvp[k]);\n";
                s += "        vp += bwt[k] * (ip * dc.bwv[k]).xyz;\n";
                s += "        vn += bwt[k] * (inrm * dc.bwvn[k]).xyz;\n";
                s += "    }\n";
            }
        } else if (!w) {
            s += "    float4 cp = mul(ip, wvp);\n";
            s += "    float3 vp = mul(ip, wv).xyz;\n";
            s += "    float3 vn = mul(float4(" + v(2, clear) + ".xyz, 0), wvn).xyz;\n";
        } else {
            s += "    var cp = ip * dc.wvp;\n";
            s += "    let vp = (ip * dc.wv).xyz;\n";
            s += "    var vn = (vec4f(" + v(2, clear) + ".xyz, 0.0) * dc.wvn).xyz;\n";
        }
        if (f.light & 8) s += Sp(w, "    vn = dot(vn, vn) > 0 ? normalize(vn) : vn;\n", "    vn = select(vn, normalize(vn), dot(vn, vn) > 0.0);\n");
    }
    if (lit) {
        // D3DMCS: 1 the vertex diffuse, 2 its specular, else the material
        auto src = [&](int field, int mat) {
            uint32_t m = (f.light >> (8 + field * 2)) & 3;
            if (m == 1) return v(3, "");
            if (m == 2) return v(4, "");
            return F(Sp(w, "mat[%d]", "dc.mat[%d]"), mat);
        };
        if (!w) {
            s += "    float4 md = " + src(0, 0) + ", ms = " + src(1, 2) + ", ma = " + src(2, 1) + ", me = " + src(3, 3) + ";\n";
            s += "    float3 la = 0, ld = 0, ls = 0;\n";
            s += (f.light & 4) ? "    float3 eye = -normalize(vp);\n" : "    float3 eye = float3(0, 0, 1);\n";
        } else {
            s += "    let md = " + src(0, 0) + ";\n    let ms = " + src(1, 2) + ";\n    let ma = " + src(2, 1) + ";\n    let me = " +
                 src(3, 3) + ";\n";
            s += "    var la = vec3f(0.0);\n    var ld = vec3f(0.0);\n    var ls = vec3f(0.0);\n";
            s += (f.light & 4) ? "    let eye = -normalize(vp);\n" : "    let eye = vec3f(0.0, 0.0, 1.0);\n";
        }
        const char* lt = Sp(w, "lt", "dc.lt");
        for (int n = 0; n < 8; n++) {
            uint32_t type = (f.types >> (n * 2)) & 3;  // D3DLIGHTTYPE: 1 point, 2 spot, 3 directional
            if (!type) continue;
            int b = n * 7;
            s += Sp(w, "    {\n        float3 l;\n        float att = 1;\n", "    {\n        var l: vec3f;\n        var att = 1.0;\n");
            if (type == 3) {
                s += F("        l = -normalize(%s[%d].xyz);\n", lt, b + 4);
            } else {
                s += F(Sp(w, "        float3 d = %s[%d].xyz - vp;\n        float dist = length(d);\n        l = d / max(dist, 1e-6);\n",
                          "        let d = %s[%d].xyz - vp;\n        let dist = length(d);\n        l = d / max(dist, 1e-6);\n"),
                       lt, b + 3);
                if (!w)
                    s += F("        att = dist > lt[%d].w ? 0 : 1 / max(lt[%d].x + lt[%d].y * dist + lt[%d].z * dist * dist, 1e-6);\n", b + 3, b + 5,
                           b + 5, b + 5);
                else
                    s += F("        att = select(1.0 / max(dc.lt[%d].x + dc.lt[%d].y * dist + dc.lt[%d].z * dist * dist, 1e-6), 0.0, dist > dc.lt[%d].w);\n",
                           b + 5, b + 5, b + 5, b + 3);
                if (type == 2) {
                    s += F(Sp(w, "        float rho = dot(-l, normalize(%s[%d].xyz));\n", "        let rho = dot(-l, normalize(%s[%d].xyz));\n"), lt,
                           b + 4);
                    if (!w)
                        s += F("        att *= rho > lt[%d].x ? 1 : rho <= lt[%d].y ? 0 : pow(saturate((rho - lt[%d].y) / (lt[%d].x - lt[%d].y)), lt[%d].w);\n",
                               b + 6, b + 6, b + 6, b + 6, b + 6, b + 4);
                    else
                        s += F("        att *= select(select(pow(saturate((rho - dc.lt[%d].y) / (dc.lt[%d].x - dc.lt[%d].y)), dc.lt[%d].w), 0.0, "
                               "rho <= dc.lt[%d].y), 1.0, rho > dc.lt[%d].x);\n",
                               b + 6, b + 6, b + 6, b + 4, b + 6, b + 6);
                }
            }
            s += F("        la += att * %s[%d].rgb;\n", lt, b + 2);
            s += Sp(w, "        float nl = dot(vn, l);\n", "        let nl = dot(vn, l);\n");
            s += F(Sp(w, "        ld += att * %s[%d].rgb * max(nl, 0);\n", "        ld += att * %s[%d].rgb * max(nl, 0.0);\n"), lt, b);
            if (f.light & 2)
                s += F(Sp(w, "        if (nl > 0) ls += att * %s[%d].rgb * pow(max(dot(vn, normalize(l + eye)), 0), %s[4].x);\n",
                          "        if (nl > 0.0) { ls += att * %s[%d].rgb * pow(max(dot(vn, normalize(l + eye)), 0.0), %s[4].x); }\n"),
                       lt, b + 1, Sp(w, "mat", "dc.mat"));
            s += "    }\n";
        }
        s += F("    oD0 = %s(me.rgb + ma.rgb * (%samb.rgb + la) + md.rgb * ld, md.a);\n", f4, dcp);
        s += (f.light & 2) ? F("    oD1 = %s(ms.rgb * ls, ms.a);\n", f4) : F("    oD1 = %s;\n", clear);
    } else {
        s += "    oD0 = " + v(3, Sp(w, "float4(1,1,1,1)", "vec4f(1.0)")) + ";\n";
        s += "    oD1 = " + v(4, clear) + ";\n";
    }
    s += "    oB0 = oD0;\n    oB1 = oD1;\n";
    for (int t = 0; t < 4; t++) {
        uint32_t u = f.tex[t], set = u & 3, gen = (u >> 4) & 7, count = (u >> 8) & 7, size = (u >> 12) & 7;
        std::string in = v(9 + int(set), black);
        if (pretransformed || (!gen && !count)) {
            s += F("    oT%d = %s;\n", t, in.c_str());
            continue;
        }
        // generated coordinates are view space; a transform takes the set with 1 after its last element
        std::string c = !w ? (gen == 1 ? "float4(vn, 1)" : gen == 2 ? "float4(vp, 1)" : gen == 3 ? "float4(reflect(normalize(vp), vn), 1)"
                              : size == 1 ? F("float4(%s.x, 1, 0, 0)", in.c_str()) : size == 2 ? F("float4(%s.xy, 1, 0)", in.c_str())
                              : size == 3 ? F("float4(%s.xyz, 1)", in.c_str()) : in)
                           : (gen == 1 ? "vec4f(vn, 1.0)" : gen == 2 ? "vec4f(vp, 1.0)" : gen == 3 ? "vec4f(reflect(normalize(vp), vn), 1.0)"
                              : size == 1 ? F("vec4f(%s.x, 1.0, 0.0, 0.0)", in.c_str()) : size == 2 ? F("vec4f(%s.xy, 1.0, 0.0)", in.c_str())
                              : size == 3 ? F("vec4f(%s.xyz, 1.0)", in.c_str()) : in);
        if (!count) {
            s += F("    oT%d = %s;\n", t, c.c_str());
            continue;
        }
        s += F(Sp(w, "    {\n        float4 tc = mul(%s, tm[%d]);\n", "    {\n        let tc = %s * dc.tm[%d];\n"), c.c_str(), t);
        if (u & 0x800) s += F("        oT%d = %s(tc.xy, tc.z, tc[%u]);\n", t, f4, std::min<uint32_t>(count, 4) - 1);
        else s += F("        oT%d = %s(tc.xy, %s, 1%s);\n", t, f4, count >= 3 ? "tc.z" : Sp(w, "0", "0.0"), Sp(w, "", ".0"));
        s += "    }\n";
    }
    s += "    oFog = " + v(5, clear) + ";\n";
    if (pretransformed) return s + (w ? kWgslVsEpilogue : kVsEpilogue);
    return s + (w ? kFixedTailWgsl : kFixedTailHlsl);
}

const char* kPsTailHlsl =
    "if (sblend.x != 0) {\n"
    "    float4 sb = round(saturate(outc) * 255);\n"
    "    float4 sv = (sb >= 128 ? sb - 256 : sb) / 255;\n"
    "    outc = sblend.x == 1 ? max(sv, 0) : max(-sv, 0);\n"
    "}\n"
    // the game's screen effects, scaled by the graphics settings (D3D12Backend::ScaleScreenEffect)
    "outc.a *= 1 + sblend.y;\n"
    "outc *= 1 + sblend.z;\n"
    // a source-alpha blended draw made brighter (D3D12Backend::ScaleTextureEffect): alpha up to 1 first, the
    // rest of the factor into the colour, so colour times alpha grows by 1 + sblend.w. The colour grows only
    // until its brightest channel reaches 1: past that the others would catch up and a coloured neon go white
    "if (sblend.w != 0) {\n"
    "    float sa = outc.a * (1 + sblend.w), na = saturate(sa);\n"
    "    float peak = max(max(outc.r, outc.g), outc.b);\n"
    "    outc.rgb *= na > 0 ? min(sa / na, peak > 0 ? 1 / peak : 1) : 0;\n"
    "    outc.a = na;\n"
    "}\n";

const char* kPsTailWgsl =
    "if (dc.sblend.x != 0.0) {\n"
    "    let sb = round(saturate(outc) * 255.0);\n"
    "    let sv = select(sb, sb - 256.0, sb >= vec4f(128.0)) / 255.0;\n"
    "    outc = select(max(-sv, vec4f(0.0)), max(sv, vec4f(0.0)), dc.sblend.x == 1.0);\n"
    "}\n"
    "outc.a *= 1.0 + dc.sblend.y;\n"
    "outc *= 1.0 + dc.sblend.z;\n"
    "if (dc.sblend.w != 0.0) {\n"
    "    let sa = outc.a * (1.0 + dc.sblend.w);\n"
    "    let na = saturate(sa);\n"
    "    let peak = max(max(outc.r, outc.g), outc.b);\n"
    "    outc = vec4f(outc.rgb * select(0.0, min(sa / na, select(1.0, 1.0 / peak, peak > 0.0)), na > 0.0), na);\n"
    "}\n"
    "outc = vec4f(saturate(outc.rgb), outc.a);\n";

const char* kPbrParallaxHlsl =
    "    if (pbr2.x > 0) {\n"
    "        float h0 = pbr_orm.Sample(smp0, tc0.xy).a - 0.5;\n"
    "        float2 duv_dx = ddx(tc0.xy), duv_dy = ddy(tc0.xy);\n"
    "        float2 v_scr = normalize((i.pos.xy / max(surf.xy, 1) - 0.5) + 1e-6);\n"
    "        tc0.xy -= (duv_dx * v_scr.x + duv_dy * v_scr.y) * (h0 * pbr2.x * 16.0);\n"
    "    }\n";

const char* kPbrParallaxWgsl =
    "    if (dc.pbr2.x > 0.0) {\n"
    "        let h0 = textureSample(pbr_orm, smp0, tc0.xy).a - 0.5;\n"
    "        let duv_dx = dpdx(tc0.xy);\n"
    "        let duv_dy = dpdy(tc0.xy);\n"
    "        let v_scr = normalize((i.pos.xy / max(dc.surf.xy, vec2f(1.0)) - 0.5) + 1e-6);\n"
    "        tc0 = vec4f(tc0.xy - (duv_dx * v_scr.x + duv_dy * v_scr.y) * (h0 * dc.pbr2.x * 16.0), tc0.zw);\n"
    "    }\n";

const char* kPbrHlsl =
    "{\n"
    "    float3 pn = pbr_nrm.Sample(smp0, tc0.xy).rgb * 2 - 1;\n"
    "    pn.xy *= pbr.x;\n"
    "    pn = normalize(float3(pn.xy, max(pn.z, 1e-3)));\n"
    "    float4 orm = pbr_orm.Sample(smp0, tc0.xy);\n"
    "    float ao = saturate(1 + (orm.r - 1) * pbr.y);\n"
    "    float rough = clamp(pbr.z < 0 ? (-pbr.z - 1) : orm.g * pbr.z, 0.04, 1);\n"
    "    float metal = saturate(pbr.w < 0 ? (-pbr.w - 1) : orm.b * pbr.w);\n"
    "    float2 dux = ddx(tc0.xy), duy = ddy(tc0.xy);\n"
    "    float2 grad = float2(pn.x * dux.x + pn.y * duy.x, pn.x * dux.y + pn.y * duy.y);\n"
    "    float glen = max(length(dux) + length(duy), 1e-5);\n"
    "    float2 n_scr = clamp(grad / glen, -1, 1);\n"
    "    float3 L_ts = normalize(float3(0.35, 0.45, 0.82));\n"
    "    float nl = saturate(dot(pn, L_ts));\n"
    "    float relief = clamp(1 + (nl - 0.82) * 0.85 * saturate(pbr.x) + (n_scr.x * 0.12 - n_scr.y * 0.15) * saturate(pbr.x), 0.55, 1.45);\n"
    "    float3 V_ts = normalize(float3(-(i.pos.xy / max(surf.xy, 1) - 0.5) * 0.5, 1));\n"
    "    float3 H_ts = normalize(L_ts + V_ts);\n"
    "    float nh = saturate(dot(pn, H_ts)), nv = saturate(dot(pn, V_ts)) + 1e-4, vh = saturate(dot(V_ts, H_ts));\n"
    "    float a2 = rough * rough * rough * rough;\n"
    "    float d = nh * nh * (a2 - 1) + 1;\n"
    "    float D = a2 / (3.14159265 * d * d + 1e-6);\n"
    "    float3 F0 = lerp(float3(0.04, 0.04, 0.04), max(outc.rgb, 0.04), metal);\n"
    "    float3 F = F0 + (1 - F0) * pow(1 - vh, 5);\n"
    "    float k_g = rough * rough * 0.5;\n"
    "    float G = nl / (4 * (nl * (1 - k_g) + k_g) * (nv * (1 - k_g) + k_g) + 1e-5);\n"
    "    float luma = dot(v0.rgb, float3(0.299, 0.587, 0.114));\n"
    "    float3 spec = D * F * G * max(v0.rgb, float3(luma, luma, luma) * 0.5 + 0.15) * saturate(pbr.x);\n"
    "    float3 lit = outc.rgb * pbr2.yzw * (relief * ao) * (1 - metal * 0.25) + spec * 0.35;\n"
    "    outc.rgb = lerp(outc.rgb, lit, fogin.a);\n"
    "}\n";

const char* kPbrWgsl =
    "{\n"
    "    var pn = textureSample(pbr_nrm, smp0, tc0.xy).rgb * 2.0 - 1.0;\n"
    "    pn = vec3f(pn.xy * dc.pbr.x, pn.z);\n"
    "    pn = normalize(vec3f(pn.xy, max(pn.z, 1e-3)));\n"
    "    let orm = textureSample(pbr_orm, smp0, tc0.xy);\n"
    "    let ao = saturate(1.0 + (orm.r - 1.0) * dc.pbr.y);\n"
    "    let rough = clamp(select(orm.g * dc.pbr.z, -dc.pbr.z - 1.0, dc.pbr.z < 0.0), 0.04, 1.0);\n"
    "    let metal = saturate(select(orm.b * dc.pbr.w, -dc.pbr.w - 1.0, dc.pbr.w < 0.0));\n"
    "    let dux = dpdx(tc0.xy);\n"
    "    let duy = dpdy(tc0.xy);\n"
    "    let grad = vec2f(pn.x * dux.x + pn.y * duy.x, pn.x * dux.y + pn.y * duy.y);\n"
    "    let glen = max(length(dux) + length(duy), 1e-5);\n"
    "    let n_scr = clamp(grad / glen, vec2f(-1.0), vec2f(1.0));\n"
    "    let L_ts = normalize(vec3f(0.35, 0.45, 0.82));\n"
    "    let nl = saturate(dot(pn, L_ts));\n"
    "    let relief = clamp(1.0 + (nl - 0.82) * 0.85 * saturate(dc.pbr.x) + (n_scr.x * 0.12 - n_scr.y * 0.15) * saturate(dc.pbr.x), 0.55, 1.45);\n"
    "    let V_ts = normalize(vec3f(-(i.pos.xy / max(dc.surf.xy, vec2f(1.0)) - 0.5) * 0.5, 1.0));\n"
    "    let H_ts = normalize(L_ts + V_ts);\n"
    "    let nh = saturate(dot(pn, H_ts));\n"
    "    let nv = saturate(dot(pn, V_ts)) + 1e-4;\n"
    "    let vh = saturate(dot(V_ts, H_ts));\n"
    "    let a2 = rough * rough * rough * rough;\n"
    "    let d = nh * nh * (a2 - 1.0) + 1.0;\n"
    "    let D = a2 / (3.14159265 * d * d + 1e-6);\n"
    "    let F0 = mix(vec3f(0.04), max(outc.rgb, vec3f(0.04)), metal);\n"
    "    let F = F0 + (1.0 - F0) * pow(1.0 - vh, 5.0);\n"
    "    let k_g = rough * rough * 0.5;\n"
    "    let G = nl / (4.0 * (nl * (1.0 - k_g) + k_g) * (nv * (1.0 - k_g) + k_g) + 1e-5);\n"
    "    let luma = dot(v0.rgb, vec3f(0.299, 0.587, 0.114));\n"
    "    let spec = D * F * G * max(v0.rgb, vec3f(luma) * 0.5 + 0.15) * saturate(dc.pbr.x);\n"
    "    let lit = outc.rgb * dc.pbr2.yzw * (relief * ao) * (1.0 - metal * 0.25) + spec * 0.35;\n"
    "    outc = vec4f(mix(outc.rgb, lit, fogin.a), outc.a);\n"
    "}\n";

std::string Pixel(const PixelKey& k, bool w) {
    std::string s = w ? std::string(kWgslCommon) + kWgslPsHelpers : Common() + PsHelpers();
    for (int i = 0; i < 4; i++) {
        const TexInfo& t = k.tex[i];
        if (!w) {
            const char* type = t.shadow ? "Texture2D<float>" : t.kind == kTex3D ? "Texture3D" : t.kind == kTexCube ? "TextureCube" : "Texture2D";
            s += F("%s tex%d : register(t%d);\n%s smp%d : register(s%d);\n", type, i, i, t.shadow ? "SamplerComparisonState" : "SamplerState", i, i);
        } else {
            const char* type = t.shadow ? "texture_depth_2d" : t.kind == kTex3D ? "texture_3d<f32>" : t.kind == kTexCube ? "texture_cube<f32>" : "texture_2d<f32>";
            s += F("@group(1) @binding(%d) var tex%d: %s;\n@group(1) @binding(%d) var smp%d: %s;\n", i * 2, i, type, i * 2 + 1, i,
                   t.shadow ? "sampler_comparison" : "sampler");
        }
    }
    if (k.pbr)
        s += Sp(w, "Texture2D pbr_nrm : register(t4);\nTexture2D pbr_orm : register(t5);\n",
                "@group(1) @binding(8) var pbr_nrm: texture_2d<f32>;\n@group(1) @binding(9) var pbr_orm: texture_2d<f32>;\n");
    if (!w) {
        s += F("float4 main(VOut i, bool front : SV_IsFrontFace) : SV_Target {\n");
        s += "    float4 tc0 = i.tc0, tc1 = i.tc1, tc2 = i.tc2, tc3 = i.tc3;\n";
        s += k.two_sided ? "    float4 v0 = front ? i.d0 : i.b0, v1 = front ? i.d1 : i.b1;\n" : "    float4 v0 = i.d0, v1 = i.d1;\n";
        s += "    if (misc.w == 0) v1 = float4(0, 0, 0, 1);\n";
        s += "    float4 fogin = float4(fogcolor.rgb, saturate(i.fogamt));\n";
    } else {
        s += "@fragment\nfn main(i: VOut, @builtin(front_facing) front: bool) -> @location(0) vec4f {\n";
        s += "    var tc0 = i.tc0;\n    var tc1 = i.tc1;\n    var tc2 = i.tc2;\n    var tc3 = i.tc3;\n";
        s += k.two_sided ? "    var v0 = select(i.b0, i.d0, front);\n    var v1 = select(i.b1, i.d1, front);\n"
                         : "    var v0 = i.d0;\n    var v1 = i.d1;\n";
        s += "    if (dc.misc.w == 0.0) { v1 = vec4f(0.0, 0.0, 0.0, 1.0); }\n";
        s += "    let fogin = vec4f(dc.fogcolor.rgb, saturate(i.fogamt));\n";
    }
    if (k.pbr) s += w ? kPbrParallaxWgsl : kPbrParallaxHlsl;
    const char* kill = Sp(w, "if (t%d.a == 0) discard;\n", "if (t%d.a == 0.0) { discard; }\n");
    std::string body;
    if (k.programmable) {
        for (int i = 0; i < 4; i++) {
            body += TexFetch(k, i, w);
            if (!w) body += Hooks().Fetched(k.tex[i], i);
        }
        for (int i = 0; i < 4; i++) {
            uint32_t m = (k.tex_modes >> (i * 5)) & 0x1F;
            bool fetch = m && m != 4 && m != 5 && m != 0xA && m != 0x11;
            if (fetch && ((k.alphakill >> i) & 1)) body += F(kill, i);
        }
        uint32_t mode0 = k.tex_modes & 0x1F;
        body += F(Sp(w, "float4 r0 = float4(0, 0, 0, %s), r1 = 0;\n", "var r0 = vec4f(0.0, 0.0, 0.0, %s);\nvar r1 = vec4f(0.0);\n"),
                  mode0 ? "t0.a" : Sp(w, "1", "1.0"));
        Ps ps{k, w};
        uint32_t n = std::min<uint32_t>(k.count & 0xFF, 8);
        for (uint32_t st = 0; st < n; st++) {
            ps.stage = int(st);
            ps.code += "{\n";
            std::string wc = ps.Stage(k.rgb_in[st], k.rgb_out[st], false);
            std::string wa = ps.Stage(k.a_in[st], k.a_out[st], true);
            ps.code += wc + wa + "}\n";
        }
        body += ps.code;
        if (k.final_abcd || k.final_efg) {
            ps.stage = 8;
            uint32_t fl = k.final_efg & 0xFF;
            ps.clamp_sum = fl & 0x80;
            ps.inv_v1 = fl & 0x40;
            ps.inv_r0 = fl & 0x20;
            ps.e = ps.Input(ParseIn(k.final_efg >> 24), false);
            ps.f = ps.Input(ParseIn((k.final_efg >> 16) & 0xFF), false);
            std::string A = ps.Input(ParseIn(k.final_abcd >> 24), false), B = ps.Input(ParseIn((k.final_abcd >> 16) & 0xFF), false),
                        C = ps.Input(ParseIn((k.final_abcd >> 8) & 0xFF), false), D = ps.Input(ParseIn(k.final_abcd & 0xFF), false),
                        G = ps.Input(ParseIn((k.final_efg >> 8) & 0xFF), true);
            if (!w) {
                body += "float4 outc;\n";
                body += "outc.rgb = " + D + " + lerp(" + C + ", " + B + ", " + A + ");\n";
                body += "outc.a = " + G + ";\n";
            } else {
                body += "var outc = vec4f(" + D + " + mix(" + C + ", " + B + ", " + A + "), " + G + ");\n";
            }
        } else {
            body += Sp(w, "float4 outc = float4(lerp(fogin.rgb, r0.rgb, fogin.a), r0.a);\n", "var outc = vec4f(mix(fogin.rgb, r0.rgb, fogin.a), r0.a);\n");
        }
    } else {
        body += Sp(w, "float4 cur = v0, tmp = 0;\n", "var cur = v0;\nvar tmp = vec4f(0.0);\n");
        for (int i = 0; i < 4; i++) {
            const uint32_t* f = k.ff[i];
            if (f[0] <= 1) break;
            const TexInfo& t = k.tex[i];
            std::string uv = TexUv(t, i, (k.proj >> i) & 1 ? F("tc%d.xy / tc%d.w", i, i) : F("tc%d.xy", i), w);
            const char* decl = Sp(w, "float4 t%d = ", "var t%d: vec4f = ");
            std::string cube;
            if (t.shadow)
                body += F(decl, i) + F("shadow(tex%d, smp%d, ", i, i) + TexUv(t, i, F("tc%d.xy / tc%d.w", i, i), w) +
                        F(Sp(w, ", tc%d.z / tc%d.w, %d, i.pos.xy, texsize[%d].xy);\n", ", tc%d.z / tc%d.w, %d, i.pos.xy, dc.texsize[%d].xy);\n"), i, i,
                          t.shadow, i);
            else if (t.kind == kTex2D && (t.flags & kTexWide))
                body += F(decl, i) + F(Sp(w, "spreadtap(tex%d, smp%d, %s, spread[%d], %s);\n", "spreadtap(tex%d, smp%d, %s, dc.spread[%d], %s);\n"), i, i,
                                       uv.c_str(), i, w ? "1.0" : Hooks().ReadScale(t).c_str());
            else if (t.kind == kTex2D)
                body += F(decl, i) + F(Sp(w, "tex%d.Sample(smp%d, %s);\n", "textureSample(tex%d, smp%d, %s);\n"), i, i, uv.c_str());
            else if (!w && t.kind == kTexCube && !(cube = Hooks().Cube(t, i, F("tc%d.xyz", i))).empty())
                body += F("float4 t%d = %s;\n", i, cube.c_str());
            else if (t.kind)
                body += F(decl, i) + F(Sp(w, "tex%d.Sample(smp%d, tc%d.xyz);\n", "textureSample(tex%d, smp%d, tc%d.xyz);\n"), i, i, i);
            else
                body += F(decl, i) + Sp(w, "float4(1,1,1,1);\n", "vec4f(1.0);\n");
            if (!w) body += Hooks().Fetched(t, i);
            if ((k.alphakill >> i) & 1) body += F(kill, i);
            const char* dst = f[8] == 5 ? "tmp" : "cur";
            std::string rgb = FfOp(f[0], FfArg(f[1], i, false, w), FfArg(f[2], i, false, w), FfArg(f[3], i, false, w), i, false, w);
            std::string a = f[4] <= 1 ? std::string("cur.a")
                                      : FfOp(f[4], FfArg(f[5], i, true, w), FfArg(f[6], i, true, w), FfArg(f[7], i, true, w), i, true, w);
            body += F(Sp(w, "{ float3 c3 = saturate(%s); float c1 = saturate(%s); %s = float4(c3, c1); }\n",
                         "{ let c3 = vec3f(saturate(%s)); let c1 = f32(saturate(%s)); %s = vec4f(c3, c1); }\n"),
                      rgb.c_str(), a.c_str(), dst);
        }
        body += Sp(w, "cur.rgb = saturate(cur.rgb + v1.rgb);\n", "cur = vec4f(saturate(cur.rgb + v1.rgb), cur.a);\n");
        body += Sp(w, "float4 outc = float4(lerp(fogin.rgb, cur.rgb, fogin.a), cur.a);\n", "var outc = vec4f(mix(fogin.rgb, cur.rgb, fogin.a), cur.a);\n");
    }
    // the console's signed blends read colours as signed bytes; the draw runs as an add of the positive part and a
    // reverse subtract of the negative one, this picks the part
    if (k.pbr && !k.car_paint) body += w ? kPbrWgsl : kPbrHlsl;
    if (k.alphatest) body += "alphatest(outc.a);\n";
    body += w ? std::string(kPsTailWgsl) + "return outc;\n}\n" : kPsTailHlsl + Hooks().Output() + "return outc;\n}\n";
    s += body;
    if (!k.quad) return s;
    return w ? QuadWgsl(std::move(s)) : QuadHlsl(std::move(s));
}

}  // namespace

std::string FixedVertexHlsl(uint16_t regs, bool pretransformed, uint64_t decode, const FixedKey& f) {
    return FixedVertex(regs, pretransformed, decode, f, false);
}
std::string FixedVertexWgsl(uint16_t regs, bool pretransformed, uint64_t decode, const FixedKey& f) {
    return FixedVertex(regs, pretransformed, decode, f, true);
}
std::string PixelHlsl(const PixelKey& k) { return Pixel(k, false); }
// the browser has no backend extension: ShaderHooks' defaults, written out in WGSL
std::string PixelWgsl(const PixelKey& k) { return Pixel(k, true); }

// the vertex shader hands on the pixel position as drawn and as pulled (e), the pixel shader takes the stretch back out
std::string QuadHlsl(std::string s) {
    auto sub = [&](const char* at, const char* before, const char* after) {
        size_t p = s.find(at);
        if (p != std::string::npos) s.replace(p, std::strlen(at), std::string(before) + at + after);
    };
    sub("    float fogamt : TEXCOORD4;\n", "", "    noperspective float4 e : TEXCOORD5;\n");
    sub("    if (surf.w == 0 && abs(p.x - (surf.x - 0.5)) < 1e-3) p.x = surf.x;\n", "    o.e.xy = p.xy;\n", "");
    sub("    if (surf.w == 0 && abs(p.y - (surf.y - 0.5)) < 1e-3) p.y = surf.y;\n", "", "    o.e.zw = p.xy;\n");
    sub("    o.pos = cp;\n", "    o.e = float4(sp, (float2(cp.x, -cp.y) / cp.w + 1) * 0.5 * surf.xy);\n", "");
    sub("float4 main(VOut i, bool front", R"(float4 unstretch(float4 t, float4 e) {
    float2 d = e.zw - e.xy, g = float2(ddx(e.x), ddy(e.y));
    return t + (d.x ? d.x / g.x * ddx(t) : 0) + (d.y ? d.y / g.y * ddy(t) : 0);
}
)", "");
    for (int i = 0; i < 4; i++) sub(F("i.tc%d", i).c_str(), "unstretch(", ", i.e)");
    return s;
}

// QuadHlsl over the WGSL generators' text
std::string QuadWgsl(std::string s) {
    auto sub = [&](const char* at, const char* before, const char* after) {
        size_t p = s.find(at);
        if (p != std::string::npos) s.replace(p, std::strlen(at), std::string(before) + at + after);
    };
    sub("    @location(8) fogamt: f32,\n", "", "    @location(9) @interpolate(linear) e: vec4f,\n");
    sub("    if (dc.surf.w == 0.0 && abs(p.x - (dc.surf.x - 0.5)) < 1e-3) { p.x = dc.surf.x; }\n", "    o.e = vec4f(p.xy, 0.0, 0.0);\n", "");
    sub("    if (dc.surf.w == 0.0 && abs(p.y - (dc.surf.y - 0.5)) < 1e-3) { p.y = dc.surf.y; }\n", "", "    o.e = vec4f(o.e.xy, p.xy);\n");
    sub("    o.pos = cp;\n", "    o.e = vec4f(sp, (vec2f(cp.x, -cp.y) / cp.w + 1.0) * 0.5 * dc.surf.xy);\n", "");
    sub("@fragment\n", R"(fn unstretch(t: vec4f, e: vec4f) -> vec4f {
    let d = e.zw - e.xy;
    let g = vec2f(dpdx(e.x), dpdy(e.y));
    return t + select(vec4f(0.0), d.x / g.x * dpdx(t), d.x != 0.0) + select(vec4f(0.0), d.y / g.y * dpdy(t), d.y != 0.0);
}
)", "");
    for (int i = 0; i < 4; i++) sub(F("i.tc%d", i).c_str(), "unstretch(", ", i.e)");
    return s;
}

namespace {

// A w buffer's reversed depth comes from the clip w alone, zparams.x * (zparams.y / w - 1), exact where z / w had
// already rounded away; a z buffer's is 1 - z. zparams.z is the polygon offset in the units of each
const char* kReversedClipHlsl = R"(float ReversedClip(float z, float w) {
    if (zparams.x != 0) return zparams.x * (zparams.y - (w + zparams.z));
    return w - z - zparams.z * w;
}
)";

const char* kReversedClipWgsl = R"(fn ReversedClip(z: f32, w: f32) -> f32 {
    if (dc.zparams.x != 0.0) { return dc.zparams.x * (dc.zparams.y - (w + dc.zparams.z)); }
    return w - z - dc.zparams.z * w;
}
)";

bool InsertAfter(std::string& s, const char* at, const char* text) {
    size_t p = s.find(at);
    if (p == std::string::npos) return false;
    s.insert(p + std::strlen(at), text);
    return true;
}

}  // namespace

std::string ReversedDepthHlsl(std::string s) {
    size_t entry = s.find("VOut main(");
    if (entry != std::string::npos) s.insert(entry, kReversedClipHlsl);
    InsertAfter(s, "    o.pos = q;\n", "    o.pos.z = ReversedClip(q.z, q.w);\n");
    InsertAfter(s, "    o.pos = cp;\n", "    o.pos.z = ReversedClip(cp.z, cp.w);\n");
    return s;
}

std::string ReversedDepthWgsl(std::string s) {
    size_t entry = s.find("@vertex\n");
    if (entry != std::string::npos) s.insert(entry, kReversedClipWgsl);
    InsertAfter(s, "    o.pos = q;\n", "    o.pos.z = ReversedClip(q.z, q.w);\n");
    InsertAfter(s, "    o.pos = cp;\n", "    o.pos.z = ReversedClip(cp.z, cp.w);\n");
    return s;
}

std::string SpriteSizeWgsl(std::string s) {
    const char* tc3 = "    o.tc3 = oT3;\n";
    size_t at = s.find(tc3);
    if (at != std::string::npos) s.replace(at, std::strlen(tc3), "    o.tc3 = oPts;\n");
    return s;
}

namespace {

const char* kVelocityOutputs = "    float4 cur : TEXCOORD8;\n    float4 prev : TEXCOORD9;\n";

// cur and prev go last in VOut, after any variant's own outputs (car paint's cn, cv): a plain pixel shader's inputs
// stay a prefix of the velocity vertex shader's outputs
void AddVelocityOutputs(std::string& s) {
    size_t v = s.find("struct VOut {");
    size_t end = v == std::string::npos ? v : s.find("};", v);
    if (end != std::string::npos) s.insert(end, kVelocityOutputs);
}

bool ReplaceOnce(std::string& s, const char* at, const std::string& with) {
    size_t p = s.find(at);
    if (p == std::string::npos) return false;
    s.replace(p, std::strlen(at), with);
    return true;
}

}  // namespace

std::string VelocityVertexHlsl(std::string s) {
    AddVelocityOutputs(s);
    // a program's screen position (oPos) as of the previous frame, to clip space the way the epilogue does
    const char* program = "    o.pos.z = ReversedClip(q.z, q.w);\n";
    const char* fixed = "    o.pos.z = ReversedClip(cp.z, cp.w);\n";
    if (s.find("float4 PrevPos(VIn i)") != std::string::npos) {
        InsertAfter(s, program, R"(    o.cur = q;
    {
        precise float4 pp = PrevPos(i);
        pp.w = magclamp(pp.w);
        o.prev = float4((pp.x * 2 / surf.x - 1) * pp.w, (1 - pp.y * 2 / surf.y) * pp.w, 0, pp.w);
    }
    o.pos.xy += taa.xy * o.pos.w;
)");
    } else {
        InsertAfter(s, program, "    o.cur = q;\n    o.prev = q;\n    o.pos.xy += taa.xy * o.pos.w;\n");
    }
    InsertAfter(s, fixed, "    o.cur = cp;\n    o.prev = cp;\n    o.pos.xy += taa.xy * o.pos.w;\n");
    return s;
}

std::string VelocityPixelHlsl(std::string s) {
    AddVelocityOutputs(s);
    ReplaceOnce(s, "float4 main(VOut i, bool front : SV_IsFrontFace) : SV_Target {",
                R"(struct PsVelocity {
    float4 c : SV_Target0;
    float4 v : SV_Target1;  // the velocity target: previous minus current position in uv, HUD mask, own motion known
};
float4 Velocity(VOut i, float4 outc) {
    // a HUD draw marks where DLSS keeps the current frame, by its own coverage (the target blends by that alpha)
    if (taa.z > 0) return float4(0, 0, 1, saturate(max(outc.a, max(outc.r, max(outc.g, outc.b)))));
    float2 c = i.cur.xy / i.cur.w, p = i.prev.xy / i.prev.w;
    return float4((p - c) * float2(0.5, -0.5), 0, taa.w);
}
PsVelocity main(VOut i, bool front : SV_IsFrontFace) {)");
    size_t r = s.rfind("return outc;\n}");
    if (r != std::string::npos)
        s.replace(r, std::strlen("return outc;\n}"),
                  "PsVelocity pv;\npv.c = outc;\npv.v = Velocity(i, outc);\nreturn pv;\n}");
    return s;
}

// a point covers its console pixel, several host pixels on an upscaled target: drawn as a quad of that size
std::string PointHlsl() {
    return Common() + R"(
[maxvertexcount(4)]
void main(point VOut v[1], inout TriangleStream<VOut> strip) {
    float2 half_pixel = v[0].pos.w / surf.xy;  // in clip units, the target spans 2 across surf.xy pixels
    // a target is read half a host pixel back (surf.w host pixels per console pixel): left and top reach that far
    float2 back = (0.5 - 1e-3) * 2 * half_pixel / float2(surf.w, surf.w * spread[0].w);
    for (int i = 0; i < 4; i++) {
        VOut o = v[0];
        o.pos.x += (i & 1) ? half_pixel.x : -half_pixel.x - back.x;
        o.pos.y += (i & 2) ? -half_pixel.y : half_pixel.y + back.y;
        strip.Append(o);
    }
}
)";
}

}  // namespace gpu
