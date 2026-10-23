#include "gpu/texture_pack.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include "app/app.h"
#include "core/log.h"
#include "gpu/shaders.h"

namespace fs = std::filesystem;
using Microsoft::WRL::ComPtr;

namespace gpu::pack {

namespace {

std::mutex g_lock;
std::unordered_map<uint64_t, fs::path> g_index;  // replacements on disk by hash
std::unordered_set<uint64_t> g_used_hashes;       // statistics only; decoded images belong to the upload caller
std::unordered_set<uint64_t> g_dumped;
std::atomic<uint32_t> g_dumped_count{0};
bool g_indexed = false;

fs::path Root() { return app::ExeDir() / "textures"; }

// the replace folder, read once: any .png/.jpg/.bmp whose name starts with 16 hex digits
void Index() {
    if (g_indexed) return;
    g_indexed = true;
    std::error_code ec;
    fs::path dir = Root() / "replace";
    fs::create_directories(dir, ec);
    fs::create_directories(Root() / "dump", ec);
    for (auto it = fs::recursive_directory_iterator(dir, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (!it->is_regular_file()) continue;
        std::string ext = it->path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (ext != ".png" && ext != ".jpg" && ext != ".jpeg" && ext != ".bmp") continue;
        std::string name = it->path().stem().string();
        if (name.size() < 16) continue;
        uint64_t h = 0;
        if (std::sscanf(name.c_str(), "%16llx", (unsigned long long*)&h) == 1) g_index[h] = it->path();
    }
    LOGR("textures: %zu replacements in %s", g_index.size(), dir.string().c_str());
}

IWICImagingFactory* Wic() {
    static ComPtr<IWICImagingFactory> factory = [] {
        ComPtr<IWICImagingFactory> f;
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f))))
            LOGE("textures: WIC unavailable, no texture packs");
        return f;
    }();
    return factory.Get();
}

// each level a box filter of the one above, down to 1x1
void BuildMips(Image& img) {
    uint32_t w = img.width, h = img.height;
    while (w > 1 || h > 1) {
        const std::vector<uint8_t>& src = img.levels.back();
        uint32_t nw = std::max(w / 2, 1u), nh = std::max(h / 2, 1u);
        std::vector<uint8_t> dst(size_t(nw) * nh * 4);
        for (uint32_t y = 0; y < nh; y++)
            for (uint32_t x = 0; x < nw; x++)
                for (int c = 0; c < 4; c++) {
                    uint32_t sum = 0, n = 0;
                    for (uint32_t dy = 0; dy < 2; dy++)
                        for (uint32_t dx = 0; dx < 2; dx++) {
                            uint32_t sx = std::min(x * 2 + dx, w - 1), sy = std::min(y * 2 + dy, h - 1);
                            sum += src[(size_t(sy) * w + sx) * 4 + c];
                            n++;
                        }
                    dst[(size_t(y) * nw + x) * 4 + c] = uint8_t((sum + n / 2) / n);
                }
        img.levels.push_back(std::move(dst));
        w = nw;
        h = nh;
    }
}

std::unique_ptr<Image> Load(const fs::path& path) {
    IWICImagingFactory* f = Wic();
    if (!f) return nullptr;
    ComPtr<IWICBitmapDecoder> dec;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> conv;
    if (FAILED(f->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec)) ||
        FAILED(dec->GetFrame(0, &frame)) || FAILED(f->CreateFormatConverter(&conv)) ||
        FAILED(conv->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                WICBitmapPaletteTypeCustom)))
        return nullptr;
    auto img = std::make_unique<Image>();
    if (FAILED(conv->GetSize(&img->width, &img->height)) || !img->width || !img->height || img->width > 16384 ||
        img->height > 16384)
        return nullptr;
    img->levels.emplace_back(size_t(img->width) * img->height * 4);
    if (FAILED(conv->CopyPixels(nullptr, img->width * 4, UINT(img->levels[0].size()), img->levels[0].data())))
        return nullptr;
    BuildMips(*img);
    return img;
}

bool SavePng(const fs::path& path, uint32_t w, uint32_t h, const uint8_t* bgra) {
    IWICImagingFactory* f = Wic();
    if (!f) return false;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> enc;
    ComPtr<IWICBitmapFrameEncode> frame;
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
    if (FAILED(f->CreateStream(&stream)) || FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) ||
        FAILED(f->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) ||
        FAILED(enc->Initialize(stream.Get(), WICBitmapEncoderNoCache)) || FAILED(enc->CreateNewFrame(&frame, nullptr)) ||
        FAILED(frame->Initialize(nullptr)) || FAILED(frame->SetSize(w, h)) || FAILED(frame->SetPixelFormat(&fmt)) ||
        FAILED(frame->WritePixels(h, w * 4, w * h * 4, const_cast<uint8_t*>(bgra))) || FAILED(frame->Commit()) ||
        FAILED(enc->Commit()))
        return false;
    return true;
}

// BC1/BC2/BC3 blocks to BGRA, for the dumps
void Color565(uint16_t c, uint8_t out[4]) {
    out[2] = uint8_t(((c >> 11) & 31) * 255 / 31);
    out[1] = uint8_t(((c >> 5) & 63) * 255 / 63);
    out[0] = uint8_t((c & 31) * 255 / 31);
    out[3] = 255;
}

void DecodeColorBlock(const uint8_t* b, bool four, uint8_t out[16][4]) {
    uint16_t c0 = uint16_t(b[0] | b[1] << 8), c1 = uint16_t(b[2] | b[3] << 8);
    uint8_t pal[4][4];
    Color565(c0, pal[0]);
    Color565(c1, pal[1]);
    if (four || c0 > c1) {
        for (int c = 0; c < 3; c++) {
            pal[2][c] = uint8_t((2 * pal[0][c] + pal[1][c]) / 3);
            pal[3][c] = uint8_t((pal[0][c] + 2 * pal[1][c]) / 3);
        }
        pal[2][3] = pal[3][3] = 255;
    } else {
        for (int c = 0; c < 3; c++) pal[2][c] = uint8_t((pal[0][c] + pal[1][c]) / 2);
        pal[2][3] = 255;
        pal[3][0] = pal[3][1] = pal[3][2] = pal[3][3] = 0;
    }
    uint32_t idx = uint32_t(b[4] | b[5] << 8 | b[6] << 16 | uint32_t(b[7]) << 24);
    for (int i = 0; i < 16; i++) std::memcpy(out[i], pal[(idx >> (i * 2)) & 3], 4);
}

std::vector<uint8_t> DecodeBlocks(Host host, const std::vector<uint8_t>& px, uint32_t pitch, uint32_t w, uint32_t h) {
    std::vector<uint8_t> out(size_t(w) * h * 4);
    uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4, bytes = host == Host::BC1 ? 8 : 16;
    for (uint32_t by = 0; by < bh; by++)
        for (uint32_t bx = 0; bx < bw; bx++) {
            size_t at = size_t(by) * pitch + size_t(bx) * bytes;
            if (at + bytes > px.size()) continue;
            const uint8_t* b = &px[at];
            uint8_t texel[16][4];
            DecodeColorBlock(host == Host::BC1 ? b : b + 8, host != Host::BC1, texel);
            if (host == Host::BC2) {
                for (int i = 0; i < 16; i++) texel[i][3] = uint8_t(((b[i / 2] >> ((i & 1) * 4)) & 15) * 17);
            } else if (host == Host::BC3) {
                uint8_t a[8] = {b[0], b[1]};
                if (a[0] > a[1])
                    for (int i = 1; i < 7; i++) a[i + 1] = uint8_t(((7 - i) * a[0] + i * a[1]) / 7);
                else {
                    for (int i = 1; i < 5; i++) a[i + 1] = uint8_t(((5 - i) * a[0] + i * a[1]) / 5);
                    a[6] = 0;
                    a[7] = 255;
                }
                uint64_t bits = 0;
                for (int i = 0; i < 6; i++) bits |= uint64_t(b[2 + i]) << (8 * i);
                for (int i = 0; i < 16; i++) texel[i][3] = a[(bits >> (3 * i)) & 7];
            }
            for (int i = 0; i < 16; i++) {
                uint32_t x = bx * 4 + i % 4, y = by * 4 + i / 4;
                if (x < w && y < h) std::memcpy(&out[(size_t(y) * w + x) * 4], texel[i], 4);
            }
        }
    return out;
}

}  // namespace

bool DumpEnabled() { return app::Get().graphics.texture_dump; }

bool ReplaceEnabled() {
    if (!app::Get().graphics.texture_pack) return false;
    std::lock_guard<std::mutex> lock(g_lock);
    Index();
    return !g_index.empty();
}

uint64_t ContentHash(const TextureDesc& d, const uint8_t* guest, uint32_t bytes, uint64_t palette) {
    uint32_t shape[6] = {d.format, d.width, d.height, d.depth, d.levels, d.cube ? 1u : 0u};
    uint64_t h = Hash(shape, sizeof(shape));
    h = Hash(guest, bytes, h);
    return palette ? Hash(&palette, sizeof(palette), h) : h;
}

std::unique_ptr<Image> Find(uint64_t hash) {
    fs::path path;
    {
        std::lock_guard<std::mutex> lock(g_lock);
        Index();
        auto at = g_index.find(hash);
        if (at == g_index.end()) return nullptr;
        path = at->second;
    }
    std::unique_ptr<Image> img = Load(path);
    if (!img) LOGE("textures: could not read %s", path.string().c_str());
    else {
        std::lock_guard<std::mutex> lock(g_lock);
        if (g_used_hashes.insert(hash).second)
            LOGI("textures: %016llX from %s, %ux%u", (unsigned long long)hash, path.filename().string().c_str(),
                 img->width, img->height);
    }
    return img;
}

void Dump(uint64_t hash, const TextureDesc& d, Host host, const std::vector<uint8_t>& px, uint32_t pitch) {
    if (d.cube || d.depth > 1 || d.width < 8 || d.height < 8) return;
    {
        std::lock_guard<std::mutex> lock(g_lock);
        Index();
        if (!g_dumped.insert(hash).second) return;
    }
    std::vector<uint8_t> bgra;
    if (host == Host::BC1 || host == Host::BC2 || host == Host::BC3) {
        bgra = DecodeBlocks(host, px, pitch, d.width, d.height);
    } else if (host == Host::BGRA8) {
        bgra.resize(size_t(d.width) * d.height * 4);
        for (uint32_t y = 0; y < d.height && size_t(y) * pitch < px.size(); y++)
            std::memcpy(&bgra[size_t(y) * d.width * 4], &px[size_t(y) * pitch],
                        std::min<size_t>(size_t(d.width) * 4, px.size() - size_t(y) * pitch));
    } else {
        return;
    }
    char name[32];
    std::snprintf(name, sizeof(name), "%016llX.png", (unsigned long long)hash);
    if (SavePng(Root() / "dump" / name, d.width, d.height, bgra.data())) g_dumped_count++;
}

Stats GetStats() {
    std::lock_guard<std::mutex> lock(g_lock);
    Index();
    return {uint32_t(g_index.size()), uint32_t(g_used_hashes.size()), g_dumped_count.load()};
}

}  // namespace gpu::pack
