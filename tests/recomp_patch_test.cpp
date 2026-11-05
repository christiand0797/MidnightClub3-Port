// [[patch]] operand overrides in the recompiler (tools/recomp/patch.cpp), on instructions taken from default.xbe.
#include <cassert>
#include <cstdio>
#include <string>
#include <vector>

#include "patch.h"

namespace {

struct Decoded {
    ZydisDecodedInstruction in;
    ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
};

Decoded Decode(std::vector<uint8_t> bytes) {
    static ZydisDecoder dec = [] {
        ZydisDecoder d;
        ZydisDecoderInit(&d, ZYDIS_MACHINE_MODE_LEGACY_32, ZYDIS_STACK_WIDTH_32);
        return d;
    }();
    Decoded d{};
    bool ok = ZYAN_SUCCESS(ZydisDecoderDecodeFull(&dec, bytes.data(), bytes.size(), &d.in, d.ops));
    assert(ok);
    return d;
}

std::string Text(const Decoded& d, uint32_t va) {
    static ZydisFormatter fmt = [] {
        ZydisFormatter f;
        ZydisFormatterInit(&f, ZYDIS_FORMATTER_STYLE_INTEL);
        return f;
    }();
    char buf[256];
    ZydisFormatterFormatInstruction(&fmt, &d.in, d.ops, d.in.operand_count_visible, buf, sizeof(buf), va, nullptr);
    return buf;
}

Patch Imm(const char* expect, int64_t v) {
    Patch p;
    p.expect = expect;
    p.imm = v;
    return p;
}

Patch Disp(const char* expect, int64_t v) {
    Patch p;
    p.expect = expect;
    p.disp = v;
    return p;
}

}  // namespace

int main() {
    std::string err;

    assert(NormalizeAsm("  PUSH   0x08 ") == "push 0x08");

    // 0001DD17 push 0x08: the network session's slot count
    {
        Decoded d = Decode({0x6A, 0x08});
        assert(Text(d, 0x1DD17) == "push 0x08");
        assert(ApplyPatch(Imm("push 0x08", 16), d.in, d.ops, Text(d, 0x1DD17), err));
        assert(d.ops[0].imm.value.s == 16);
        assert(Text(d, 0x1DD17) == "push 0x10");
    }
    // 00268D00 push 0x3E0: the race config allocation
    {
        Decoded d = Decode({0x68, 0xE0, 0x03, 0x00, 0x00});
        assert(ApplyPatch(Imm("push 0x3E0", 0x968), d.in, d.ops, Text(d, 0x268D00), err));
        assert(d.ops[0].imm.value.u == 0x968);
    }
    // 002693DD lea eax, [eax+ecx*1+0x114]: the race config's player array
    {
        Decoded d = Decode({0x8D, 0x84, 0x08, 0x14, 0x01, 0x00, 0x00});
        std::string t = Text(d, 0x2693DD);
        assert(t == "lea eax, [eax+ecx*1+0x114]");
        assert(ApplyPatch(Disp("lea eax, [eax+ecx*1+0x114]", 0x3E0), d.in, d.ops, t, err));
        assert(d.ops[1].mem.disp.value == 0x3E0);
        assert(Text(d, 0x2693DD) == "lea eax, [eax+ecx*1+0x3E0]");
    }
    // 0013C3BA cmp [ecx+0x4C], eax: a memory operand without an immediate
    {
        Decoded d = Decode({0x39, 0x41, 0x4C});
        Patch p = Imm("cmp [ecx+0x4C], eax", 1);
        assert(!ApplyPatch(p, d.in, d.ops, Text(d, 0x13C3BA), err));
        assert(err.find("no immediate") != std::string::npos);
    }
    // 0016CCAA mov dword ptr [eax+0x4C], 0x04: memory and immediate, each patchable on its own
    {
        Decoded d = Decode({0xC7, 0x40, 0x4C, 0x04, 0x00, 0x00, 0x00});
        std::string t = Text(d, 0x16CCAA);
        Decoded e = d;
        assert(ApplyPatch(Imm(t.c_str(), 8), d.in, d.ops, t, err));
        assert(d.ops[1].imm.value.s == 8);
        assert(ApplyPatch(Disp(t.c_str(), 0x50), e.in, e.ops, t, err));
        assert(e.ops[0].mem.disp.value == 0x50);
    }
    // wrong expect text: the image or config changed
    {
        Decoded d = Decode({0x6A, 0x08});
        assert(!ApplyPatch(Imm("push 0x09", 16), d.in, d.ops, Text(d, 0), err));
        assert(err.find("push 0x08") != std::string::npos);
        assert(d.ops[0].imm.value.s == 8);
    }
    // an 8-bit operand cannot take 0x100
    {
        Decoded d = Decode({0xB3, 0x08});  // mov bl, 0x08
        assert(!ApplyPatch(Imm("mov bl, 0x08", 0x100), d.in, d.ops, Text(d, 0), err));
        assert(ApplyPatch(Imm("mov bl, 0x08", 0x10), d.in, d.ops, Text(d, 0), err));
    }
    // control flow stays as analysed
    {
        Decoded d = Decode({0xC2, 0x04, 0x00});  // ret 0x04
        assert(!ApplyPatch(Imm("ret 0x04", 8), d.in, d.ops, Text(d, 0), err));
        assert(err.find("control flow") != std::string::npos);
    }
    // exactly one of imm and disp
    {
        Decoded d = Decode({0x6A, 0x08});
        Patch p = Imm("push 0x08", 1);
        p.disp = 4;
        assert(!ApplyPatch(p, d.in, d.ops, Text(d, 0), err));
        Patch q;
        q.expect = "push 0x08";
        assert(!ApplyPatch(q, d.in, d.ops, Text(d, 0), err));
    }
    // an explicit operand of the wrong kind
    {
        Decoded d = Decode({0xC7, 0x40, 0x4C, 0x04, 0x00, 0x00, 0x00});
        Patch p = Imm("mov dword ptr [eax+0x4C], 0x04", 8);
        p.operand = 0;
        assert(!ApplyPatch(p, d.in, d.ops, Text(d, 0), err));
    }
    std::puts("recomp_patch: ok");
    return 0;
}
