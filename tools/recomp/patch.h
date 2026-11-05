#pragma once
// Instruction operand patches from a config's [[patch]] entries: one immediate or one memory displacement of one
// guest instruction is replaced before analysis and code generation see it. Used to change constants inside large
// recompiled functions (array sizes, loop bounds, struct offsets) without rewriting the function as a host hook.
//
//   [[patch]]
//   at = 0x0001DD17
//   expect = "push 0x08"   # the instruction as the disassembly comments print it; the build fails if it differs
//   imm = 16               # or disp = 0x3E0, the new displacement of the memory operand
//   operand = 0            # optional: which operand, when the instruction has more than one candidate
//   why = "network session slots"
#include <Zydis/Zydis.h>

#include <cstdint>
#include <optional>
#include <string>

struct Patch {
    uint32_t at = 0;
    std::string expect;
    std::optional<int64_t> imm, disp;
    int operand = -1;  // -1: the only operand of the patched kind
    std::string why;
};

// Intel syntax, as the generated code's comments print it
std::string FormatAsm(const ZydisDecodedInstruction& in, const ZydisDecodedOperand* ops, uint32_t va);

// lower case with runs of white space collapsed, for comparing disassembly text
std::string NormalizeAsm(const std::string& s);

// Checks the patch against the instruction as decoded (before patching) and applies it to ops.
// text: the instruction's disassembly, compared with expect. Returns false and sets err when the patch does not fit.
bool ApplyPatch(const Patch& p, const ZydisDecodedInstruction& in, ZydisDecodedOperand* ops, const std::string& text,
                std::string& err);
