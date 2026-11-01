// Road-scrape evidence for playtests: counts the game's own chassis-contact effects without changing them.
//  - 0x1027D0: the body/bound contact damage handler (every contact set it is given)
//  - 0x36F60: the spark emitter it calls once the contact response passes its thresholds (docs/notes/road_scrape.md)
//  - 0x10B130: the scrape sound
// One "scrape:" report line at most every five seconds, and only when something happened, so a slammed car's log
// shows whether it really touches the road (sparks and sound) or only rests on its tucked wheels.
#include <atomic>
#include <chrono>

#include "core/log.h"
#include "xdk/hle.h"

namespace {
std::atomic<uint32_t> g_contacts{0}, g_sparks{0}, g_sounds{0};
std::atomic<int64_t> g_next_report{0};

void Report() {
    using namespace std::chrono;
    const int64_t now = duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
    int64_t due = g_next_report.load();
    if (now < due || !g_next_report.compare_exchange_strong(due, now + 5000)) return;
    const uint32_t contacts = g_contacts.exchange(0), sparks = g_sparks.exchange(0), sounds = g_sounds.exchange(0);
    if (sparks || sounds) LOGR("scrape: %u body contact sets, %u spark emissions, %u scrape sounds in the last 5 s",
                               contacts, sparks, sounds);
}
}  // namespace

X86_EXTERN_FUNC(__imp__sub_001027D0);
X86_FUNC(sub_001027D0) {
    g_contacts++;
    __imp__sub_001027D0(ctx, base);
    Report();
}

X86_EXTERN_FUNC(__imp__sub_00036F60);
X86_FUNC(sub_00036F60) {
    g_sparks++;
    __imp__sub_00036F60(ctx, base);
}

X86_EXTERN_FUNC(__imp__sub_0010B130);
X86_FUNC(sub_0010B130) {
    g_sounds++;
    __imp__sub_0010B130(ctx, base);
}
