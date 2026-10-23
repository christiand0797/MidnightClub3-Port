#include "xdk/winsock_version.h"
#include <cassert>

int main() {
    // MC3 requests MAKEWORD(2, 0) and aborts if the negotiated version is 2.2.
    static_assert(xnet::NegotiateWinsockVersion(0x0002) == 0x0002);
    static_assert(xnet::NegotiateWinsockVersion(0x0102) == 0x0102);
    static_assert(xnet::NegotiateWinsockVersion(0x0202) == 0x0202);
    static_assert(xnet::NegotiateWinsockVersion(0x0302) == 0x0202);
    static_assert(xnet::NegotiateWinsockVersion(0x0001) == 0x0001);
    static_assert(xnet::NegotiateWinsockVersion(0x0201) == 0x0101);
    static_assert(xnet::NegotiateWinsockVersion(0x0003) == 0x0202);
    static_assert(xnet::NegotiateWinsockVersion(0) == 0);
    assert(xnet::NegotiateWinsockVersion(0x0002) != 0x0202);
}
