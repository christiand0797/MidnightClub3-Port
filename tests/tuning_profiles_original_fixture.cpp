// Retain actual profile-image copying and slot-compaction instructions. CarCfg
// value copying and clearing the unused tail slot are isolated test contracts.
#include "cpu/x86_ops.h"
#undef X86_FUNC
#undef X86_WEAK_FUNC
#define X86_FUNC(name) static void name(X86Context& __restrict ctx,uint8_t* base)
#define X86_WEAK_FUNC(name,impl)
#define __imp__sub_0026BD10 OriginalProfileImageCopy
#define __imp__sub_0026BB80 OriginalOwnedCarRemove
#include "x86_recomp.71.cpp"
#undef __imp__sub_0026BD10
#undef __imp__sub_0026BB80
extern "C" void __imp__sub_0026BD10(X86Context& ctx,uint8_t* base){OriginalProfileImageCopy(ctx,base);}
extern "C" void __imp__sub_0026BB80(X86Context& ctx,uint8_t* base){OriginalOwnedCarRemove(ctx,base);}
