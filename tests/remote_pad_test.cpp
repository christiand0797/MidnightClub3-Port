// Remote controller reports (runtime/core/remote_pad.h): parse exactly 24-byte "MC3I" reports, merge like a
// second controller on the same port.
#include <cassert>
#include <cstdint>
#include <cstdio>

#include "core/remote_pad.h"

using input::Pad;

static void Report(uint8_t* d, uint8_t port, uint8_t flags, uint16_t buttons, uint8_t rt, int16_t lx) {
    for (int i = 0; i < 24; i++) d[i] = 0;
    d[0] = 'M', d[1] = 'C', d[2] = '3', d[3] = 'I';
    d[4] = port, d[5] = flags;
    d[6] = uint8_t(buttons), d[7] = uint8_t(buttons >> 8);
    d[8] = 0xFF;  // A
    d[15] = rt;   // right trigger
    d[16] = uint8_t(lx), d[17] = uint8_t(uint16_t(lx) >> 8);
}

int main() {
    uint8_t d[24];
    int port = -1;
    Pad p;

    Report(d, 1, 1, 0x0110, 200, -32768);
    assert(input::ParseRemotePad(d, 24, port, p));
    assert(port == 1 && p.connected && p.analog[0] == 0xFF && p.analog[7] == 200 && p.lx == -32768);
    assert(p.buttons == 0x10);  // bit 8 is not a console button and is dropped

    assert(!input::ParseRemotePad(d, 23, port, p));  // short
    assert(!input::ParseRemotePad(d, 25, port, p));  // long
    Report(d, 4, 1, 0, 0, 0);
    assert(!input::ParseRemotePad(d, 24, port, p));  // only four ports
    Report(d, 0, 1, 0, 0, 0);
    d[3] = 'P';
    assert(!input::ParseRemotePad(d, 24, port, p));  // not a controller report (e.g. a relay datagram)

    Pad local;
    local.connected = true;
    local.analog[7] = 255;
    local.lx = 1000;
    Pad remote;
    remote.connected = true;
    remote.buttons = 0x10;
    remote.analog[6] = 128;
    Pad m = input::MergeRemote(local, remote);
    assert(m.buttons == 0x10 && m.analog[6] == 128 && m.analog[7] == 255 && m.lx == 1000);  // centred stick keeps local
    remote.lx = -5000;
    m = input::MergeRemote(local, remote);
    assert(m.lx == -5000 && m.ly == 0);

    Pad unplugged;
    m = input::MergeRemote(Pad{}, unplugged);
    assert(!m.connected);  // a stale browser leaves the port exactly as it was
    m = input::MergeRemote(Pad{}, remote);
    assert(m.connected);   // a browser alone plugs a controller into an empty port

    std::puts("remote_pad: ok");
    return 0;
}
