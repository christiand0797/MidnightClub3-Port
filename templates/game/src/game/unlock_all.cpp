// A test launch option, MC3_UNLOCK_ALL=1: every city answers "unlocked", Detroit included, so renderer and texture
// checks can reach all four cities on a test profile. Nothing is written to a save by this; the profile's own progress
// stays as it was and the cities lock again on a launch without the option.
//
// sub_00292CD0(this, type, id, flag) is the unlock query (docs/HANDOFF.md, "Detroit unlock"); cities are type 5
// (0 San Diego, 1 Atlanta, 2 Detroit, 3 Tokyo). Answering at every caller matters: the profile load fills the per-city
// tables Arcade reads (sub_0013BA60), and an Arcade-only answer left Detroit's name pointer unfilled.
#include <cstdlib>

#include "core/log.h"
#include "kernel/kernel.h"
#include "xdk/hle.h"

namespace {
constexpr uint32_t kUnlockCity = 5;

bool Enabled() {
    static const bool on = [] {
        const char* v = std::getenv("MC3_UNLOCK_ALL");
        const bool set = v && *v && *v != '0';
        if (set) LOGR("unlock: MC3_UNLOCK_ALL set, every city answers unlocked (nothing is saved)");
        return set;
    }();
    return on;
}
}  // namespace

X86_EXTERN_FUNC(__imp__sub_00292CD0);
X86_FUNC(sub_00292CD0) {
    const uint32_t type = kernel::R32(ctx.esp + 4);
    __imp__sub_00292CD0(ctx, base);
    if (type == kUnlockCity && Enabled()) ctx.eax = 1;
}
