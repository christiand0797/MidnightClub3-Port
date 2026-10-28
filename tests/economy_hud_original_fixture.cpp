// Execute the original bounded formatter and Flash wrappers/value writers. The fixture supplies only
// the CRT's final character formatting and named-variable resolver, with strict argument contracts.
#include "cpu/x86_ops.h"
#include <cassert>

#undef X86_FUNC
#undef X86_WEAK_FUNC
#define X86_FUNC(name) static void name(X86Context& __restrict ctx, uint8_t* base)
#define X86_WEAK_FUNC(name, impl)
#define __imp__sub_001395E0 OriginalWalletString
#define __imp__sub_00139520 OriginalWalletInteger
#include "x86_recomp.35.cpp"
#undef __imp__sub_001395E0
#undef __imp__sub_00139520
#define __imp__sub_00064350 OriginalFlashStringValue
#include "x86_recomp.9.cpp"
#undef __imp__sub_00064350
#define __imp__sub_0020CF50 OriginalWalletFormat
#include "x86_recomp.59.cpp"
#undef __imp__sub_0020CF50

extern "C" void sub_00064350(X86Context& ctx, uint8_t* base) { OriginalFlashStringValue(ctx, base); }

void EconomyOriginalHudCall(X86Context& ctx, uint8_t* base, uint32_t address) {
    if (address == 0x20CF50) OriginalWalletFormat(ctx, base);
    else if (address == 0x1395E0) OriginalWalletString(ctx, base);
    else if (address == 0x139520) OriginalWalletInteger(ctx, base);
    else assert(false && "unexpected broad original call during wallet refresh");
}
