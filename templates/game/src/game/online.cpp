// Online lobbies in place of Xbox Live (runtime/core/relay_link.cpp, runtime/app/lobby_menu.cpp).
//
// The network select page (built at 0x180220) has two entries: "Internet_Play" with callback 0x180160 and "LAN_Play"
// with callback 0x1801C0. Internet Play sets the network mode [0x50F14C] = 2 and opens page 0x1F, the Xbox Live
// sign-in, which needs Microsoft's retired servers. LAN Play sets mode 1, starts System Link (0x1B0AD0) and opens
// page 0x20, Host/Join. Online play here is System Link carried between players by the lobby server, so Internet
// Play now connects the built-in link, opens the lobby window and continues exactly like LAN Play. Both callbacks
// are thiscall members of the page (ecx), so the LAN body runs with the same context.
// [online] enabled = false in settings.toml restores the original entry.
#include "app/app.h"
#include "core/log.h"
#include "core/relay_link.h"
#include "cpu/x86_ops.h"
#include "kernel/kernel.h"

using namespace kernel;

X86_EXTERN_FUNC(__imp__sub_00180160);
X86_EXTERN_FUNC(__imp__sub_001801C0);
X86_FUNC(sub_00180160) {
    if (!app::Get().online.enabled) return __imp__sub_00180160(ctx, base);
    LOGR("online: Internet Play opens the online lobbies");
    relay::Start();
    app::OpenLobby();
    __imp__sub_001801C0(ctx, base);
}
