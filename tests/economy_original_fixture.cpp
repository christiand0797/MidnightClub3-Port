// Retain the exact original save-needed query, not a restatement of its dirty semantics.
#include "cpu/x86_ops.h"
#undef X86_FUNC
#undef X86_WEAK_FUNC
#define X86_FUNC(name) static void name(X86Context& __restrict ctx, uint8_t* base)
#define X86_WEAK_FUNC(name, impl)
#define __imp__sub_000197F0 OriginalSaveNeeded
#include "x86_recomp.1.cpp"
#undef __imp__sub_000197F0

bool EconomyOriginalSaveNeeded(uint8_t* memory) {
    X86Context ctx{};
    ctx.esp = 0x10000;
    OriginalSaveNeeded(ctx, memory);
    return ctx.eax != 0;
}
