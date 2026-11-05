#include "patch.h"

#include <cctype>

std::string FormatAsm(const ZydisDecodedInstruction& in, const ZydisDecodedOperand* ops, uint32_t va) {
    static ZydisFormatter fmt = [] {
        ZydisFormatter f;
        ZydisFormatterInit(&f, ZYDIS_FORMATTER_STYLE_INTEL);
        return f;
    }();
    char buf[256];
    ZydisFormatterFormatInstruction(&fmt, &in, ops, in.operand_count_visible, buf, sizeof(buf), va, nullptr);
    return buf;
}

std::string NormalizeAsm(const std::string& s) {
    std::string out;
    bool space = false;
    for (char c : s) {
        if (std::isspace(uint8_t(c))) {
            space = !out.empty();
            continue;
        }
        if (space) out += ' ';
        space = false;
        out += char(std::tolower(uint8_t(c)));
    }
    return out;
}

static bool IsFlow(ZydisMnemonic m) {
    switch (m) {
        case ZYDIS_MNEMONIC_CALL: case ZYDIS_MNEMONIC_JMP: case ZYDIS_MNEMONIC_RET: case ZYDIS_MNEMONIC_IRET:
        case ZYDIS_MNEMONIC_IRETD: case ZYDIS_MNEMONIC_LOOP: case ZYDIS_MNEMONIC_LOOPE: case ZYDIS_MNEMONIC_LOOPNE:
        case ZYDIS_MNEMONIC_JCXZ: case ZYDIS_MNEMONIC_JECXZ: case ZYDIS_MNEMONIC_ENTER: case ZYDIS_MNEMONIC_INT:
        case ZYDIS_MNEMONIC_INT3: case ZYDIS_MNEMONIC_INTO:
            return true;
        default:
            break;
    }
    return m >= ZYDIS_MNEMONIC_JB && m <= ZYDIS_MNEMONIC_JZ;
}

bool ApplyPatch(const Patch& p, const ZydisDecodedInstruction& in, ZydisDecodedOperand* ops, const std::string& text,
                std::string& err) {
    if (p.imm.has_value() == p.disp.has_value()) {
        err = "needs exactly one of imm or disp";
        return false;
    }
    if (p.expect.empty()) {
        err = "needs expect, the instruction text it replaces";
        return false;
    }
    if (NormalizeAsm(text) != NormalizeAsm(p.expect)) {
        err = "instruction is \"" + text + "\", not \"" + p.expect + "\"";
        return false;
    }
    if (IsFlow(in.mnemonic)) {
        err = "control flow instructions cannot be patched";
        return false;
    }
    const ZydisOperandType kind = p.imm ? ZYDIS_OPERAND_TYPE_IMMEDIATE : ZYDIS_OPERAND_TYPE_MEMORY;
    int pick = -1;
    if (p.operand >= 0) {
        if (p.operand >= in.operand_count_visible || ops[p.operand].type != kind) {
            err = "operand " + std::to_string(p.operand) + " is not " + (p.imm ? "an immediate" : "a memory operand");
            return false;
        }
        pick = p.operand;
    } else {
        for (int i = 0; i < in.operand_count_visible; i++)
            if (ops[i].type == kind) {
                if (pick >= 0) {
                    err = "more than one candidate operand: name one with operand =";
                    return false;
                }
                pick = i;
            }
        if (pick < 0) {
            err = std::string("no ") + (p.imm ? "immediate" : "memory") + " operand";
            return false;
        }
    }
    ZydisDecodedOperand& o = ops[pick];
    if (p.imm) {
        // the value must fit the instruction's operand size, as a signed or an unsigned number
        unsigned bits = in.operand_width ? in.operand_width : o.size;
        if (bits < 64) {
            int64_t lo = -(int64_t(1) << (bits - 1)), hi = (int64_t(1) << bits) - 1;
            if (*p.imm < lo || *p.imm > hi) {
                err = "imm " + std::to_string(*p.imm) + " does not fit " + std::to_string(bits) + " bits";
                return false;
            }
        }
        o.imm.value.s = *p.imm;
    } else {
        if (o.mem.type == ZYDIS_MEMOP_TYPE_AGEN && in.mnemonic != ZYDIS_MNEMONIC_LEA) {
            err = "memory operand is not a data access";
            return false;
        }
        if (*p.disp < INT32_MIN || *p.disp > UINT32_MAX) {
            err = "disp does not fit 32 bits";
            return false;
        }
        o.mem.disp.value = *p.disp;
        o.mem.disp.has_displacement = ZYAN_TRUE;
    }
    return true;
}
