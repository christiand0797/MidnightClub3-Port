// Run the exact string-table teardown that crashed after a garage purchase/save. Unused guest bodies have
// test-local linkage, just as in tuning_solver_fixture.cpp, and are discarded with their unrelated dependencies.
#include "cpu/x86_ops.h"

#undef X86_FUNC
#undef X86_WEAK_FUNC
#define X86_FUNC(name) static void name(X86Context& __restrict ctx, uint8_t* base)
#define X86_WEAK_FUNC(name, impl)
#define __imp__sub_000EA950 FixtureStringTableClear
#define __imp__sub_000EA9F0 FixtureStringTablesDestroy
#include "x86_recomp.25.cpp"
#undef __imp__sub_000EA950
#undef __imp__sub_000EA9F0

extern "C" void __imp__sub_000EA950(X86Context& ctx, uint8_t* base) { FixtureStringTableClear(ctx, base); }
extern "C" void FixtureGarageLeaveForSave(X86Context& ctx, uint8_t* base) { FixtureStringTablesDestroy(ctx, base); }
