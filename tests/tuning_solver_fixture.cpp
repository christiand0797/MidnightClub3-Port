// Execute the original generated wheel solver, not a duplicate mathematical model. Give unused guest bodies
// test-local linkage so the compiler discards the rest of this translation unit and its unrelated dependencies.
#include "cpu/x86_ops.h"
#include <cassert>

#undef X86_FUNC
#undef X86_WEAK_FUNC
#define X86_FUNC(name) static void name(X86Context& __restrict ctx, uint8_t* base)
#define X86_WEAK_FUNC(name, impl)
#define __imp__sub_000A7250 FixtureWheelSolver
#define __imp__sub_000A65D0 FixtureWheelDisplacement
#include "x86_recomp.17.cpp"
#undef __imp__sub_000A7250
#undef __imp__sub_000A65D0

extern "C" void __imp__sub_000A7250(X86Context& ctx, uint8_t* base) { FixtureWheelSolver(ctx, base); }
extern "C" void __imp__sub_000A65D0(X86Context& ctx, uint8_t* base) { FixtureWheelDisplacement(ctx, base); }

#define __imp__sub_001284F0 FixtureAirbagMove
#include "x86_recomp.33.cpp"
#undef __imp__sub_001284F0
extern "C" void __imp__sub_001284F0(X86Context& ctx, uint8_t* base) { FixtureAirbagMove(ctx, base); }

// These are bottom-out impulse paths, disabled in the fixture exactly as selected by the guest flags. Entering
// either would need a full rigid body fixture; fail loudly instead of quietly pretending a physical response.
extern "C" void sub_00341B20(X86Context&, uint8_t*) { assert(false && "unexpected bottom-out impulse query"); }
void x86_icall(X86Context&, uint8_t*, uint32_t) { assert(false && "unexpected virtual rigid body impulse"); }
