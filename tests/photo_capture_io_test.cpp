#include "gpu/photo_capture_io.h"
#include "gpu/photo_capture_shader.h"
#include "gpu/shaders.h"
#include <cassert>
#include <chrono>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>

namespace {
using namespace gpu::photo_capture;
Microsoft::WRL::ComPtr<ID3DBlob> Compile(const std::string& source) {
    Microsoft::WRL::ComPtr<ID3DBlob> code, errors;
    const auto result = D3DCompile(source.data(), source.size(), nullptr, nullptr, nullptr, "main", "ps_5_0",
                                  D3DCOMPILE_OPTIMIZATION_LEVEL1, 0, &code, &errors);
    if (FAILED(result) && errors) std::cerr << static_cast<const char*>(errors->GetBufferPointer());
    assert(SUCCEEDED(result));
    return code;
}

void ShaderAccess() {
    assert(!CompiledTextureStages(nullptr));
    auto direct = Compile("Texture2D a:register(t0); Texture2D b:register(t2); SamplerState s:register(s0); "
                          "float4 main(float2 uv:TEXCOORD):SV_Target{return b.Sample(s,uv);}");
    assert(CompiledTextureStages(direct.Get()) == 4);  // Declared and bound t0 is genuinely unused.
    gpu::PixelKey fixed{};
    fixed.tex[0].kind = fixed.tex[1].kind = gpu::kTex2D;
    fixed.ff[0][0] = 2; fixed.ff[0][1] = 2;  // SELECTARG1 TEXTURE
    fixed.ff[0][4] = 2; fixed.ff[0][5] = 2;
    fixed.ff[1][0] = 1;  // Disabled stage1 despite a valid bound texture.
    auto sampled = Compile(gpu::PixelHlsl(fixed));
    assert(CompiledTextureStages(sampled.Get()) == 1);
    fixed.ff[0][1] = fixed.ff[0][5] = 0;  // Select diffuse; unused arg2 may still name the texture.
    fixed.ff[0][2] = fixed.ff[0][6] = 2;
    auto diffuse = Compile(gpu::PixelHlsl(fixed));
    assert(CompiledTextureStages(diffuse.Get()) == 0);
    gpu::PixelKey programmed{};
    programmed.programmable = 1;
    programmed.tex[0].kind = programmed.tex[1].kind = gpu::kTex2D;
    programmed.tex_modes = 1 | (1u << 5);  // t1 fetch is dead; default r0 alpha uses only t0.
    auto program = Compile(gpu::PixelHlsl(programmed));
    assert(CompiledTextureStages(program.Get()) == 1);
    programmed.tex_modes = 4;  // Passthrough coordinates are not a texture access.
    auto coordinates = Compile(gpu::PixelHlsl(programmed));
    assert(CompiledTextureStages(coordinates.Get()) == 0);
}

void CachedFallback() {
    struct Cache { TextureIdentity identity; bool replacement = false; } cache;
    constexpr uint64_t original = 0x4D145117D9DD0F63ull;
    assert(UploadWithIdentity(cache.identity, original, [&] { cache.replacement = true; return true; }));
    assert(cache.identity.original_hash == original && cache.replacement);
    // Mirrors CreateTexture's whole-entry reset after the old replacement no longer applies.
    assert(UploadWithIdentity(cache.identity, original, [&] { cache = {}; return true; }));
    assert(cache.identity.original_hash == original && !cache.replacement);
    const auto cached_bind = cache.identity;
    assert(cached_bind.original_hash == original);  // No upload occurs on a later bind.
    auto erased = std::make_unique<Cache>();
    assert(!UploadWithIdentity(erased->identity, original, [&] { erased.reset(); return false; }));
    assert(!erased);  // Allocation failure may destroy the entry. No post-failure reference access is allowed.
}

void LimitsAndOrder() {
    assert(PixelBytes(4096,4096) == kMaxFrameBytes);
    assert(!PixelBytes(4096,4097) && !PixelBytes(0,1) && !PixelBytes(~0u,~0u));
    std::vector<uint8_t> pixels;
    std::string events;
    auto copy = [&] { events += 'C'; };
    auto restore = [&] { events += 'P'; };
    assert(ReadPresented(2,2,pixels,copy,[&](auto& out) { events += 'R'; out.resize(16); return true; },restore));
    assert(events == "CRP");
    events.clear();
    assert(!ReadPresented(2,2,pixels,copy,[&](auto&) { events += 'R'; return false; },restore));
    assert(events == "CRP");  // Includes Map/readback false: restore PRESENT before returning failure.
    events.clear();
    assert(!ReadPresented(~0u,~0u,pixels,copy,[&](auto&) { events += 'R'; return true; },restore));
    assert(events.empty());  // Ceiling before allocation/transition/readback.
    try {
        ReadPresented(2,2,pixels,copy,[&](auto&) -> bool { events += 'R'; throw std::runtime_error("fixture"); },restore);
        assert(false);
    } catch (const std::runtime_error&) { assert(events == "CRP"); }
}

void BmpEvidence() {
    namespace fs = std::filesystem;
    const auto parent = fs::weakly_canonical(fs::current_path());
    const auto root = parent / ("photo-io-fixture-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    assert(fs::create_directory(root));
    const auto path = root/"present.bmp";
    const std::vector<uint8_t> pixels{1,2,3,255,4,5,6,255,7,8,9,255,10,11,12,255};
    assert(SaveBmpNew(path,2,2,pixels));
    auto read = [&] { std::ifstream file(path,std::ios::binary); return std::vector<uint8_t>(
                         std::istreambuf_iterator<char>(file),std::istreambuf_iterator<char>()); };
    const auto saved = read();
    assert(saved.size() == 70 && saved[0] == 'B' && saved[1] == 'M');
    int32_t height = 0; std::memcpy(&height,saved.data()+22,4); assert(height == -2);
    assert(std::equal(pixels.begin(),pixels.end(),saved.begin()+54));  // Exact BGRA; no resample/re-encode.
    assert(!SaveBmpNew(path,2,2,std::vector<uint8_t>(16,123)) && read() == saved);
    assert(!SaveBmpNew(root/"bad.bmp",2,2,{1,2,3}) && !fs::exists(root/"bad.bmp"));
    assert(fs::weakly_canonical(root).parent_path() == parent && root.filename().string().starts_with("photo-io-fixture-"));
    fs::remove_all(root);
}
}

int main() {
    ShaderAccess(); CachedFallback(); LimitsAndOrder(); BmpEvidence();
    std::cout << "photo IO/reflection: 4 CPU groups passed; no GPU/device created\n";
}
