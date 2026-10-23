// HD movies (gpu/video_hd.h): the title's movie player, as found in default.xbe:
//  - BinkOpen(path, flags) at 0x3AB590 returns the Bink; its Width +0, Height +4, Frames +8, FrameNum +0xC (1 based)
//  - the player's open, sub_003639C0(this, name, loop) ret 8: builds d:\video\<name>.bik, opens it, makes a
//    LIN_X8R8G8B8 texture of its size; the Bink at this+0, the texture header at [[this+4]+0x14]+8
//  - its update, sub_003638A0(this): BinkDoFrame, LockRect, BinkCopyToBuffer, BinkNextFrame, BinkWait
//  - its close, sub_00363B90(this): BinkClose, the texture released
// The title still decodes every Bink frame (its audio and timing); the renderer shows the HD file's same frame.
#include <mutex>
#include <string>
#include <unordered_map>

#include "app/app.h"
#include "core/log.h"
#include "gpu/video_hd.h"
#include "xdk/hle.h"

using kernel::R32;

namespace {
constexpr uint32_t kBinkFrameNum = 0xC;
std::mutex g_lock;
std::unordered_map<uint32_t, std::string> g_paths;  // Bink handle -> the path it was opened from
std::unordered_map<uint32_t, uint32_t> g_players;   // player -> its texture header

std::string GuestString(uint32_t va) {
    std::string s;
    if (!va) return s;
    const char* p = static_cast<const char*>(kernel::HostPtr(va));
    for (size_t i = 0; i < 260 && p[i]; i++) s += p[i];
    return s;
}

uint32_t PlayerTexture(uint32_t self) {
    uint32_t wrap = R32(self + 4);
    if (!wrap) return 0;
    uint32_t inner = R32(wrap + 0x14);
    return inner ? R32(inner + 8) : 0;
}
}  // namespace

X86_EXTERN_FUNC(__imp__sub_003AB590);
X86_FUNC(sub_003AB590) {
    std::string path = GuestString(R32(ctx.esp + 4));
    __imp__sub_003AB590(ctx, base);
    if (ctx.eax) {
        std::lock_guard<std::mutex> l(g_lock);
        g_paths[ctx.eax] = path;
    }
}

X86_EXTERN_FUNC(__imp__sub_003639C0);
X86_FUNC(sub_003639C0) {
    uint32_t self = ctx.ecx;
    __imp__sub_003639C0(ctx, base);
    if (!(ctx.eax & 0xFF)) return;
    uint32_t bink = R32(self), header = PlayerTexture(self);
    std::string path;
    {
        std::lock_guard<std::mutex> l(g_lock);
        auto it = g_paths.find(bink);
        if (it != g_paths.end()) path = it->second;
        if (header) g_players[self] = header;
    }
    if (header && !path.empty() && gpu::video::Open(header, path))
        LOGR("movies: %s plays from its HD file", path.c_str());
}

X86_EXTERN_FUNC(__imp__sub_003638A0);
X86_FUNC(sub_003638A0) {
    uint32_t self = ctx.ecx;
    uint32_t bink = R32(self);
    uint32_t shown = bink ? R32(bink + kBinkFrameNum) : 0;  // the frame this update decodes and copies
    __imp__sub_003638A0(ctx, base);
    uint32_t header = 0;
    {
        std::lock_guard<std::mutex> l(g_lock);
        auto it = g_players.find(self);
        if (it != g_players.end()) header = it->second;
    }
    if (header && shown) gpu::video::Frame(header, shown);
}

X86_EXTERN_FUNC(__imp__sub_00363B90);
X86_FUNC(sub_00363B90) {
    uint32_t self = ctx.ecx;
    uint32_t header = 0;
    {
        std::lock_guard<std::mutex> l(g_lock);
        auto it = g_players.find(self);
        if (it != g_players.end()) {
            header = it->second;
            g_players.erase(it);
        }
        g_paths.erase(R32(self));
    }
    if (header) gpu::video::Close(header);
    __imp__sub_00363B90(ctx, base);
}
