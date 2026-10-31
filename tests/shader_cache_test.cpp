#include "gpu/shader_cache.h"

#include <cassert>
#include <cstring>
#include <filesystem>
#include <fstream>

#include <d3dcompiler.h>

using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;
namespace sc = gpu::shader_cache;

int main() {
    const fs::path dir = fs::temp_directory_path() / "mc3_shader_cache_test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    sc::SetDirectory(dir);
    const char* src = "float4 main(float4 p : POSITION) : SV_Position { return p * 2; }";
    ComPtr<ID3DBlob> first, second, err;
    // first use compiles and stores one entry
    assert(SUCCEEDED(sc::Compile(src, "main", "vs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL1, first, &err)) && first);
    auto c = sc::Counters();
    assert(c.compiled == 1 && c.stored == 1 && c.hits == 0);
    size_t files = 0;
    fs::path entry;
    for (const auto& e : fs::directory_iterator(dir)) { ++files; entry = e.path(); }
    assert(files == 1 && entry.extension() == ".dxbc");
    // the second use reads the same bytes back
    assert(SUCCEEDED(sc::Compile(src, "main", "vs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL1, second)) && second);
    c = sc::Counters();
    assert(c.hits == 1 && c.compiled == 1);
    assert(first->GetBufferSize() == second->GetBufferSize() &&
           !std::memcmp(first->GetBufferPointer(), second->GetBufferPointer(), first->GetBufferSize()));
    // other flags are another shader
    assert(SUCCEEDED(sc::Compile(src, "main", "vs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, second)));
    assert(sc::Counters().compiled == 2);
    // a truncated entry is ignored and compiled again
    fs::resize_file(entry, 20);
    assert(SUCCEEDED(sc::Compile(src, "main", "vs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL1, second)) && second);
    assert(sc::Counters().compiled == 3 && second->GetBufferSize() == first->GetBufferSize());
    // a source that does not compile reports the compiler's error and stores nothing
    const auto before = sc::Counters().stored;
    assert(FAILED(sc::Compile("float4 main( : SV_Position", "main", "vs_5_0", 0, second, &err)) && err);
    assert(sc::Counters().stored == before);
    // no directory: compile only
    sc::SetDirectory({});
    const auto stored = sc::Counters().stored, compiled = sc::Counters().compiled;
    assert(SUCCEEDED(sc::Compile(src, "main", "vs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL1, second)) && second);
    assert(sc::Counters().compiled == compiled + 1 && sc::Counters().stored == stored);
    fs::remove_all(dir, ec);
}
