#pragma once
// Opt-in pre-initialization barrier. Reads only; it never creates, clears or replaces release files.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace app::qa_startup {

using Clock = std::chrono::steady_clock;
constexpr auto kTimeout = std::chrono::seconds(120);
constexpr auto kPoll = std::chrono::milliseconds(250);

struct Config {
    bool file_configured = false, token_configured = false;
    std::filesystem::path release;
    std::string token;
};
template<class Normalize>
void PrepareRelease(Config& config, Normalize normalize) {
    // Long-path normalization may make relative paths absolute. Never normalize before this raw-path check.
    if (config.release.is_absolute()) config.release = normalize(config.release);
}
struct EnvironmentValue { bool configured = false; std::wstring value; bool error = false; };
inline EnvironmentValue ReadEnvironment(const wchar_t* name) {
    SetLastError(ERROR_SUCCESS);
    const DWORD needed = GetEnvironmentVariableW(name, nullptr, 0);
    if (!needed) {
        const DWORD error = GetLastError();
        if (error == ERROR_ENVVAR_NOT_FOUND) return {};
        return {true, {}, error != ERROR_SUCCESS};  // An explicitly empty value is configured, not absent.
    }
    if (needed > 32768) return {true, {}, true};
    std::vector<wchar_t> buffer(needed);
    const DWORD count = GetEnvironmentVariableW(name, buffer.data(), DWORD(buffer.size()));
    if (!count || count >= buffer.size()) return {true, {}, true};
    return {true, std::wstring(buffer.data(), count), false};
}
inline Config FromEnvironment() {
    auto file = ReadEnvironment(L"XBE_QA_STARTUP_RELEASE_FILE");
    auto token = ReadEnvironment(L"XBE_QA_STARTUP_TOKEN");
    Config config{file.configured, token.configured, std::filesystem::path(file.value), {}};
    if (file.error || token.error) { config.file_configured = config.token_configured = true; return config; }
    for (wchar_t c : token.value) {
        if (c > 127) { config.token.clear(); return config; }
        config.token.push_back(char(c));
    }
    return config;
}
enum class Probe { Missing, Match, Invalid, Error };
enum class Outcome { Disabled, Released, Rejected, TimedOut };
struct Result {
    Outcome outcome;
    const char* reason;
    bool Proceed() const { return outcome == Outcome::Disabled || outcome == Outcome::Released; }
};

inline bool TokenValid(std::string_view token) {
    if (token.size() != 32) return false;
    for (char c : token)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return false;
    return true;
}

inline Probe ReadRelease(const std::filesystem::path& path, std::string_view expected) {
    // OPEN_REPARSE_POINT inspects the final path itself; never follow a release-file symlink.
    // Sharing only for readers rejects a writer still modifying the file and pins these bytes while read.
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ? Probe::Missing : Probe::Error;
    }
    struct Close { HANDLE file; ~Close() { CloseHandle(file); } } close{file};
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(file, &info)) return Probe::Error;
    if (GetFileType(file) != FILE_TYPE_DISK || (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))
        || info.nFileSizeHigh || info.nFileSizeLow != 32) return Probe::Invalid;
    std::array<char, 32> bytes{};
    DWORD count = 0;
    if (!ReadFile(file, bytes.data(), DWORD(bytes.size()), &count, nullptr)) return Probe::Error;
    return count == bytes.size() && std::string_view(bytes.data(), bytes.size()) == expected ? Probe::Match : Probe::Invalid;
}

// The injected seams exercise deadlines, publication races and errors without sleeping or starting a game.
// The initial probe MUST find no file, even if a pre-existing file already contains the right token.
template<class Now, class Sleep, class Read, class Waiting>
Result Run(const Config& config, Now now, Sleep sleep, Read read, Waiting waiting) {
    if (!config.file_configured && !config.token_configured) return {Outcome::Disabled, "not configured"};
    if (!config.file_configured || !config.token_configured || config.release.empty()
        || !config.release.is_absolute() || config.release.filename().empty() || !TokenValid(config.token))
        return {Outcome::Rejected, "both variables, an absolute release-file path and an exact32hex token are required"};
    try {
        const auto start = now();
        const auto deadline = start + kTimeout;
        if (read(config.release, config.token) != Probe::Missing)
            return {Outcome::Rejected, "release path pre-exists or cannot be inspected; preserved"};
        waiting();
        auto previous = start;
        for (;;) {
            const auto before = now();
            if (before < previous) return {Outcome::Rejected, "monotonic clock moved backwards"};
            if (before >= deadline) return {Outcome::TimedOut, "120-second deadline reached; release path preserved"};
            const Probe probe = read(config.release, config.token);
            const auto after = now();
            if (after < before) return {Outcome::Rejected, "monotonic clock moved backwards"};
            if (after >= deadline) return {Outcome::TimedOut, "120-second deadline reached; release path preserved"};
            if (probe == Probe::Match) return {Outcome::Released, "new exact release token accepted"};
            if (probe != Probe::Missing) return {Outcome::Rejected, "invalid/foreign release or read error; preserved"};
            previous = after;
            sleep(std::min(Clock::duration(kPoll), deadline - after));
        }
    } catch (...) {
        return {Outcome::Rejected, "startup gate clock/filesystem/wait error; release path preserved"};
    }
}

template<class Waiting>
Result Wait(const Config& config, Waiting waiting) {
    return Run(config, [] { return Clock::now(); }, [](auto duration) { std::this_thread::sleep_for(duration); },
               ReadRelease, waiting);
}

}  // namespace app::qa_startup
