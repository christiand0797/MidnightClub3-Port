// Execute the original XDK setters and state-block constant reader from this supported disc.
#include "cpu/x86_ops.h"
#include <cassert>
#undef X86_FUNC
#undef X86_WEAK_FUNC
#define X86_FUNC(name) static void name(X86Context& __restrict ctx, uint8_t* base)
#define X86_WEAK_FUNC(name, impl)
#define __imp__sub_00414EB0 OriginalSetOne
#define __imp__sub_00414F10 OriginalSetOneFast
#define __imp__sub_00414F60 OriginalSetFour
#define __imp__sub_00415010 OriginalSetBulkFast
#define __imp__sub_004150F0 OriginalSetBulk
#define __imp__sub_00416F80 OriginalCaptureState
#include "x86_recomp.122.cpp"
#undef __imp__sub_00414EB0
#undef __imp__sub_00414F10
#undef __imp__sub_00414F60
#undef __imp__sub_00415010
#undef __imp__sub_004150F0
#undef __imp__sub_00416F80

extern "C" void FixtureOriginalSetOne(X86Context& c, uint8_t* b) { OriginalSetOne(c,b); }
extern "C" void FixtureOriginalSetOneFast(X86Context& c, uint8_t* b) { OriginalSetOneFast(c,b); }
extern "C" void FixtureOriginalSetFour(X86Context& c, uint8_t* b) { OriginalSetFour(c,b); }
extern "C" void FixtureOriginalSetBulkFast(X86Context& c, uint8_t* b) { OriginalSetBulkFast(c,b); }
extern "C" void FixtureOriginalSetBulk(X86Context& c, uint8_t* b) { OriginalSetBulk(c,b); }
extern "C" void FixtureOriginalCaptureState(X86Context& c, uint8_t* b) { OriginalCaptureState(c,b); }
// The original non-Fast bulk wrapper delegates to the exact original Fast body.
extern "C" void sub_00415010(X86Context& c, uint8_t* b) { OriginalSetBulkFast(c,b); }
#define UNEXPECTED(name) extern "C" void name(X86Context&, uint8_t*) { assert(false && #name); }
UNEXPECTED(sub_00416850)  // no push-buffer exhaustion in this bounded fixture
UNEXPECTED(sub_00414610)
UNEXPECTED(sub_004145D0)
UNEXPECTED(sub_00415AE0)
UNEXPECTED(sub_00415420)
UNEXPECTED(sub_004114D0)
