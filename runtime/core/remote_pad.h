#pragma once
// Controller state sent to this PC by a local helper (tools/multiplayer/stream/stream_proxy.py relays a browser
// player's gamepad). Datagrams on 127.0.0.1:(kRemotePadPort + instance), little-endian, 24 bytes:
//   "MC3I" | port u8 | flags u8 | buttons u16 | analog[8] | lx, ly, rx, ry i16
// buttons and analog are the console report (input::Pad); flags bit 0 = connected. A port with no datagram for
// kRemotePadStaleMs reads as unplugged again, so a closed browser tab never leaves a key held.
#include <cstdint>
#include <cstring>

#include "core/input.h"

namespace input {

constexpr uint16_t kRemotePadPort = 30771;
constexpr int kRemotePadBytes = 24;
constexpr uint64_t kRemotePadStaleMs = 500;

// false for anything that is not a well-formed report for ports 0-3
inline bool ParseRemotePad(const uint8_t* d, int n, int& port, Pad& pad) {
    if (n != kRemotePadBytes || std::memcmp(d, "MC3I", 4) != 0 || d[4] > 3) return false;
    auto i16 = [&](int at) { return int16_t(uint16_t(d[at] | d[at + 1] << 8)); };
    port = d[4];
    pad = Pad{};
    pad.connected = (d[5] & 1) != 0;
    pad.buttons = uint16_t((d[6] | d[7] << 8) & 0xFF);  // only the 8 digital buttons exist
    std::memcpy(pad.analog, d + 8, 8);
    pad.lx = i16(16);
    pad.ly = i16(18);
    pad.rx = i16(20);
    pad.ry = i16(22);
    return true;
}

// a fresh remote report adds to the port's own controller/keyboard; sticks replace only when deflected
inline Pad MergeRemote(Pad local, const Pad& remote) {
    if (!remote.connected) return local;
    local.connected = true;
    local.buttons |= remote.buttons;
    for (int i = 0; i < 8; i++)
        if (remote.analog[i] > local.analog[i]) local.analog[i] = remote.analog[i];
    if (remote.lx || remote.ly) local.lx = remote.lx, local.ly = remote.ly;
    if (remote.rx || remote.ry) local.rx = remote.rx, local.ry = remote.ry;
    return local;
}

void StartRemotePads();          // once; listens only on loopback
Pad RemotePad(int port);         // the latest fresh report, or an unplugged pad

}  // namespace input
