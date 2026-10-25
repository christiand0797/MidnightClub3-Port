// Let the production-wrapper regression receive the real FB4E0 result, rather than a copied formula.
#include "cpu/x86_ops.h"

#undef X86_FUNC
#undef X86_WEAK_FUNC
#define X86_FUNC(name) static void name(X86Context& __restrict ctx, uint8_t* base)
#define X86_WEAK_FUNC(name, impl)
#include "x86_recomp.27.cpp"

extern "C" void TestOriginalWidthOffset(X86Context& ctx, uint8_t* base) {
    __imp__sub_000FB4E0(ctx, base);
}
