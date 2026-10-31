#pragma once
// Compiled shaders kept on disk, so a shader the game first needs mid-drive is read back instead of compiled there:
// D3DCompile of a new program costs tens of milliseconds, and a city's first frames compile dozens at once (a third
// of a second stall at a cruise's start, long enough to starve the music stream).
//
// An entry is named by a hash of the source, entry point, profile and flags, and carries a second, independent hash
// of the same inputs that must match on read, so a name collision compiles anew instead of loading the wrong code.
// Writes go to a temporary file and are renamed into place; an unreadable, truncated or mismatched entry is ignored.
#include <cstdint>
#include <filesystem>
#include <string_view>

#include <d3dcommon.h>
#include <wrl/client.h>

namespace gpu::shader_cache {

// where entries go; empty turns the disk cache off (Compile then only compiles)
void SetDirectory(const std::filesystem::path& dir);
const std::filesystem::path& Directory();

// D3DCompile with the disk cache in front: S_OK with the code, or the compiler's error with its message in errors
HRESULT Compile(std::string_view source, const char* entry, const char* profile, uint32_t flags,
                Microsoft::WRL::ComPtr<ID3DBlob>& code, Microsoft::WRL::ComPtr<ID3DBlob>* errors = nullptr);

struct Stats {
    uint32_t hits = 0, compiled = 0, stored = 0;
};
Stats Counters();

}  // namespace gpu::shader_cache
