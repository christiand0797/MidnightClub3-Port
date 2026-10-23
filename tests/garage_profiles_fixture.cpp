// Use the actual retail diameter/ratio/fit readers and CarCfg byte setter. Other generated bodies are local and
// discarded. The production hooks handle extended indices; stock and automatic cases still execute the original.
#include "cpu/x86_ops.h"
#include <cassert>

#undef X86_FUNC
#undef X86_WEAK_FUNC
#define X86_FUNC(name) static void name(X86Context& __restrict ctx, uint8_t* base)
#define X86_WEAK_FUNC(name, impl)
#define __imp__sub_00270B40 FixtureTireDiameter
#define __imp__sub_00270B60 FixtureTireFit
#define __imp__sub_00270BC0 FixtureTireRatio
#define __imp__sub_00272FE0 FixtureCfgTireDiameter
#define __imp__sub_00270AF0 FixtureAutoProfile
#define __imp__sub_00272020 FixtureCfgSet
#include "x86_recomp.72.cpp"
#undef __imp__sub_00270B40
#undef __imp__sub_00270B60
#undef __imp__sub_00270BC0
#undef __imp__sub_00272FE0
#undef __imp__sub_00270AF0
#undef __imp__sub_00272020

extern "C" void __imp__sub_00270B40(X86Context& ctx, uint8_t* base) { FixtureTireDiameter(ctx, base); }
extern "C" void __imp__sub_00270B60(X86Context& ctx, uint8_t* base) { FixtureTireFit(ctx, base); }
extern "C" void __imp__sub_00270BC0(X86Context& ctx, uint8_t* base) { FixtureTireRatio(ctx, base); }
extern "C" void __imp__sub_00272FE0(X86Context& ctx, uint8_t* base) { FixtureCfgTireDiameter(ctx, base); }
extern "C" void sub_00270AF0(X86Context& ctx, uint8_t* base) { FixtureAutoProfile(ctx, base); }
extern "C" void FixtureGarageSetProfile(X86Context& ctx, uint8_t* base) { FixtureCfgSet(ctx, base); }
void x86_icall(X86Context&, uint8_t*, uint32_t) { assert(false && "unexpected profile setter indirect call"); }
