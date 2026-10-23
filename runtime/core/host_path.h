#pragma once
#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace hostpath {

// Host paths stay native/wide for I/O. UTF-8 is only their explicit text representation for logs/metadata.
inline std::string Utf8(const std::filesystem::path& path) {
    auto bytes = path.u8string();
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

inline std::filesystem::path FromUtf8(std::string_view text) {
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(text.data()), text.size()));
}

inline std::filesystem::path Native(const std::filesystem::path& path) {
#ifdef _WIN32
    auto prepared = path;
    prepared.make_preferred();  // extended Windows paths do not accept '/' as a separator
    // Extended paths do not need the machine's LongPathsEnabled registry setting. Preserve existing prefixes.
    auto name = prepared.native();
    if (name.size() >= 248 && !name.starts_with(L"\\\\?\\") && !name.starts_with(L"\\\\.\\")) {
        std::error_code ec;
        auto absolute = std::filesystem::absolute(prepared, ec).lexically_normal().native();
        if (!ec) {
            std::replace(absolute.begin(), absolute.end(), L'/', L'\\');
            if (absolute.starts_with(L"\\\\")) return std::filesystem::path(L"\\\\?\\UNC\\" + absolute.substr(2));
            return std::filesystem::path(L"\\\\?\\" + absolute);
        }
    }
    return prepared;
#else
    return path;
#endif
}

inline FILE* Open(const std::filesystem::path& path, const char* mode) {
#ifdef _WIN32
    std::wstring wide_mode(mode, mode + std::char_traits<char>::length(mode));
    return _wfopen(Native(path).c_str(), wide_mode.c_str());
#else
    return std::fopen(path.c_str(), mode);
#endif
}

#ifdef _WIN32
// The callback contract is GetModuleFileNameW's: a full buffer reports its capacity. The seam permits a real
// long/Unicode-path regression without moving or launching a game executable.
template<class Reader> std::filesystem::path ReadExecutablePath(Reader read) {
    constexpr uint32_t limit = 32768;
    std::vector<wchar_t> buffer(256);
    for (;;) {
        uint32_t count = read(buffer.data(), uint32_t(buffer.size()));
        if (!count) throw std::runtime_error("Cannot determine the executable's location.");
        if (count < buffer.size()) return Native(std::filesystem::path(std::wstring(buffer.data(), count)));
        if (buffer.size() == limit) throw std::runtime_error("The executable path exceeds the Windows path limit.");
        buffer.resize(std::min<uint32_t>(uint32_t(buffer.size()) * 2, limit));
    }
}
std::filesystem::path ExecutablePath();
#endif

}  // namespace hostpath
