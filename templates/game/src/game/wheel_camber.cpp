// 0x869D0 (matrix rotate about local Z, this = matrix, one float angle, ret 4) is shared by many callers; only the
// two camber calls in the wheel palette builder take the rigid tilt. See wheel_camber.h.
#include "wheel_camber.h"
#include "cpu/x86_ops.h"
#include <cstring>

X86_EXTERN_FUNC(__imp__sub_000869D0);
X86_FUNC(sub_000869D0) {
    const uint32_t caller = X86_MEM32(ctx.esp);
    if (caller == 0xF7719u || caller == 0xF7D3Eu) {
        float angle;
        std::array<float, 6> rows;
        std::memcpy(&angle, base + uint32_t(ctx.esp + 4), 4);
        std::memcpy(rows.data(), base + ctx.ecx, sizeof(rows));
        if (wheel_camber::Rotate(rows, angle)) {
            std::memcpy(base + ctx.ecx, rows.data(), sizeof(rows));
            ctx.esp += 8;  // return address and the angle
            return;
        }
    }
    __imp__sub_000869D0(ctx, base);
}
