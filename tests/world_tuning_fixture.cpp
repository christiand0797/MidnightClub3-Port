// Exercise the actual generated retail density writers and target arithmetic.
// Unrelated static bodies are discarded by the compiler, as in the wheel fixtures.
#include "cpu/x86_ops.h"
#include <cassert>
#undef X86_FUNC
#undef X86_WEAK_FUNC
#define X86_FUNC(name) static void name(X86Context& __restrict ctx, uint8_t* base)
#define X86_WEAK_FUNC(name, impl)
#define __imp__sub_00222770 FixturePedestrianDensity
#include "x86_recomp.62.cpp"
#undef __imp__sub_00222770
#define __imp__sub_002B9670 FixtureTrafficDensity
#define __imp__sub_002B93C0 FixtureTrafficTarget
#define __imp__sub_002B9300 FixtureTrafficRequest
#include "x86_recomp.81.cpp"
#undef __imp__sub_002B9670
#undef __imp__sub_002B93C0
#undef __imp__sub_002B9300
#define __imp__sub_0007DFB0 FixtureAvailableAmbient
#include "x86_recomp.13.cpp"
#undef __imp__sub_0007DFB0

extern "C" void __imp__sub_00222770(X86Context& ctx, uint8_t* base) { FixturePedestrianDensity(ctx, base); }
extern "C" void __imp__sub_002B9670(X86Context& ctx, uint8_t* base) { FixtureTrafficDensity(ctx, base); }
extern "C" void sub_002B93C0(X86Context& ctx, uint8_t* base) { FixtureTrafficTarget(ctx, base); }
extern "C" void FixturePopulationTrafficRequest(X86Context& ctx, uint8_t* base) { FixtureTrafficRequest(ctx, base); }
extern "C" void FixturePopulationAvailableAmbient(X86Context& ctx, uint8_t* base) { FixtureAvailableAmbient(ctx, base); }
void x86_icall(X86Context&, uint8_t*, uint32_t) { assert(false && "unexpected population indirect call"); }
