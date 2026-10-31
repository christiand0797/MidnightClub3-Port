#include "gpu/shader_cache.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>

#include <d3dcompiler.h>

namespace gpu::shader_cache {
namespace {

using Microsoft::WRL::ComPtr;

// a new layout or compiler setup makes every older entry a miss
constexpr uint32_t kVersion = 1;
constexpr char kMagic[4] = {'M', 'C', '3', 'S'};

struct Header {
    char magic[4];
    uint32_t version;
    uint64_t check;  // the second hash of the inputs
    uint64_t bytes;  // the code that follows
};

std::mutex g_lock;
std::filesystem::path g_dir;
std::atomic<uint32_t> g_hits{0}, g_compiled{0}, g_stored{0};

// FNV-1a over the inputs from a seed: two seeds give the name and the check
uint64_t Fnv(uint64_t h, const void* data, size_t n) {
    const auto* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 1099511628211ull;
    return h;
}
uint64_t Key(uint64_t seed, std::string_view source, const char* entry, const char* profile, uint32_t flags) {
    const uint32_t version = kVersion;
    uint64_t h = Fnv(seed, &version, sizeof(version));
    h = Fnv(h, &flags, sizeof(flags));
    h = Fnv(h, profile, std::strlen(profile) + 1);
    h = Fnv(h, entry, std::strlen(entry) + 1);
    return Fnv(h, source.data(), source.size());
}

bool Load(const std::filesystem::path& file, uint64_t check, ComPtr<ID3DBlob>& code) {
    std::ifstream in(file, std::ios::binary);
    Header h{};
    if (!in.read(reinterpret_cast<char*>(&h), sizeof(h))) return false;
    if (std::memcmp(h.magic, kMagic, 4) || h.version != kVersion || h.check != check || !h.bytes ||
        h.bytes > (64u << 20))
        return false;
    ComPtr<ID3DBlob> blob;
    if (FAILED(D3DCreateBlob(size_t(h.bytes), &blob))) return false;
    if (!in.read(static_cast<char*>(blob->GetBufferPointer()), std::streamsize(h.bytes))) return false;
    code = std::move(blob);
    return true;
}

void Store(const std::filesystem::path& file, uint64_t check, ID3DBlob* code) {
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    std::filesystem::path part = file;
    part += ".part" + std::to_string(std::hash<std::thread::id>{}(std::this_thread::get_id()));
    {
        std::ofstream out(part, std::ios::binary | std::ios::trunc);
        Header h{};
        std::memcpy(h.magic, kMagic, 4);
        h.version = kVersion;
        h.check = check;
        h.bytes = code->GetBufferSize();
        out.write(reinterpret_cast<const char*>(&h), sizeof(h));
        out.write(static_cast<const char*>(code->GetBufferPointer()), std::streamsize(code->GetBufferSize()));
        if (!out) {
            out.close();
            std::filesystem::remove(part, ec);
            return;
        }
    }
    std::filesystem::rename(part, file, ec);  // another copy of the game may have stored it first: either is fine
    if (ec) std::filesystem::remove(part, ec);
    else g_stored++;
}

}  // namespace

void SetDirectory(const std::filesystem::path& dir) {
    std::lock_guard lock(g_lock);
    g_dir = dir;
}
const std::filesystem::path& Directory() { return g_dir; }

HRESULT Compile(std::string_view source, const char* entry, const char* profile, uint32_t flags, ComPtr<ID3DBlob>& code,
                ComPtr<ID3DBlob>* errors) {
    std::filesystem::path dir;
    {
        std::lock_guard lock(g_lock);
        dir = g_dir;
    }
    const uint64_t name = Key(14695981039346656037ull, source, entry, profile, flags);
    const uint64_t check = Key(0x9E3779B97F4A7C15ull, source, entry, profile, flags);
    char leaf[32];
    std::snprintf(leaf, sizeof(leaf), "%016llx.dxbc", static_cast<unsigned long long>(name));
    const std::filesystem::path file = dir.empty() ? std::filesystem::path() : dir / leaf;
    if (!file.empty() && Load(file, check, code)) {
        g_hits++;
        return S_OK;
    }
    ComPtr<ID3DBlob> err;
    const HRESULT hr = D3DCompile(source.data(), source.size(), nullptr, nullptr, nullptr, entry, profile, flags, 0,
                                  &code, &err);
    if (errors) *errors = err;
    if (FAILED(hr)) return hr;
    g_compiled++;
    if (!file.empty()) Store(file, check, code.Get());
    return hr;
}

Stats Counters() { return {g_hits.load(), g_compiled.load(), g_stored.load()}; }

}  // namespace gpu::shader_cache
