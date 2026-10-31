// The title's effect compiler (shaderlibx/*.fx, parsed and assembled at load by the XGraphics shader assembler
// linked into the XBE, 0x39xxxx) reports every assembler error through 0x39D3E9(context, severity, code, format, ...)
// and then leaves the pass without its shader, drawing with the code's default texture stages. Its messages, and
// D3DXCompileEffect's, reached only DbgPrint/info logs, which release builds drop; they are logged at report level.
// (Car paint lost its pixel shader through the state-block capture instead: d3d8.cpp SetPixelShader,
// docs/notes/neon_vinyl_20261028.md.)
#include <algorithm>
#include <atomic>
#include <string>

#include "core/log.h"
#include "kernel/memory.h"
#include "xdk/hle.h"

using kernel::R32;

namespace {
// the format with %s, %c and integer conversions taken from the guest's cdecl arguments
std::string Format(uint32_t fmt, uint32_t args) {
    auto text = [](uint32_t va, uint32_t max) {
        std::string s;
        for (uint32_t i = 0; va && i < max; ++i) {
            const char c = char(kernel::G<uint8_t>(va + i));
            if (!c) break;
            s.push_back(c);
        }
        return s;
    };
    const std::string f = text(fmt, 256);
    std::string out;
    for (size_t i = 0; i < f.size(); ++i) {
        if (f[i] != '%' || i + 1 >= f.size()) {
            out.push_back(f[i]);
            continue;
        }
        size_t j = i + 1;
        while (j < f.size() && std::string("0123456789.-+ #lh").find(f[j]) != std::string::npos) ++j;
        const char c = j < f.size() ? f[j] : 0;
        if (c == '%') out.push_back('%');
        else if (c == 's') out += text(R32(args), 128), args += 4;
        else if (c == 'c') out.push_back(char(R32(args))), args += 4;
        else if (c) out += c == 'x' || c == 'X' ? std::to_string(R32(args)) : std::to_string(int32_t(R32(args))), args += 4;
        i = j;
    }
    return out;
}
std::atomic<int> g_logged{0};

// printable guest text, at most max bytes, stopping at a NUL; empty when the address is not readable text
std::string GuestText(uint32_t va, uint32_t max) {
    std::string s;
    if (va < 0x10000) return s;
    for (uint32_t i = 0; i < max; ++i) {
        const char c = char(kernel::G<uint8_t>(va + i));
        if (!c) break;
        if ((c < 32 && c != '\n' && c != '\r' && c != '\t') || c == 127) return {};
        s.push_back(c == '\n' || c == '\r' || c == '\t' ? ' ' : c);
    }
    return s;
}
// an effect's first line (its texture comments name it well enough to tell the .fx files apart)
std::string SourceHead(uint32_t src, uint32_t len) {
    std::string s = GuestText(src, std::min<uint32_t>(len, 96));
    return s.empty() ? "?" : s;
}
}  // namespace

// D3DXCompileEffect(source, length, compiled*, errors*) at 0x37A120: on failure 0x4EB10 releases the error buffer
// unread, leaving the effect without its shaders. Log the result and the buffer's text (found among its first fields).
X86_EXTERN_FUNC(__imp__sub_0037A120);
X86_FUNC(sub_0037A120) {
    const uint32_t sp = ctx.esp, src = R32(sp + 4), len = R32(sp + 8), errors_out = R32(sp + 0x10);
    __imp__sub_0037A120(ctx, base);
    if (int32_t(ctx.eax) >= 0) return;
    std::string why;
    if (const uint32_t buffer = errors_out ? R32(errors_out) : 0)
        for (uint32_t field = 0; field < 0x14 && why.empty(); field += 4) why = GuestText(R32(buffer + field), 400);
    LOGR("effect compile failed %08X for \"%s\": %s", ctx.eax, SourceHead(src, len).c_str(),
         why.empty() ? "(no error text)" : why.c_str());
}
