#pragma once
#include <cstdint>

namespace xnet {
// WSADATA.wVersion is the negotiated version, while wHighVersion is the implementation's maximum.
// Several titles require exactly Winsock 2.0 even when the host also supports 2.2.
constexpr uint16_t NegotiateWinsockVersion(uint16_t requested) {
    unsigned major = requested & 0xFF, minor = requested >> 8;
    if (major < 1) return 0;
    if (major == 1) return uint16_t(1 | ((minor ? 1 : 0) << 8));
    if (major == 2 && minor < 2) return requested;
    return 0x0202;
}
}  // namespace xnet
