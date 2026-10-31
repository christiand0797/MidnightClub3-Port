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
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include "app/app.h"
#include "core/log.h"
#include "core/host_path.h"
#include "gpu/shaders.h"

namespace fs = std::filesystem;
using Microsoft::WRL::ComPtr;

namespace gpu::pack {

namespace {

std::mutex g_lock;
std::unordered_map<uint64_t, fs::path> g_index;      // replacements on disk by hash
std::unordered_set<uint64_t> g_used_hashes;           // statistics only; decoded images belong to the upload caller
std::unordered_set<uint64_t> g_dumped;
std::atomic<uint32_t> g_dumped_count{0};
bool g_indexed = false;

// PBR companion maps in textures/pbr/<hash>_{n,ao,r,m,h}.png
std::unordered_map<uint64_t, fs::path> g_pbr_index;  // <hash>_n.png on disk by hash
std::unordered_set<uint64_t> g_pbr_missing;          // hashes checked and absent on disk
std::unordered_set<uint64_t> g_pbr_failed;           // hashes whose _n.png failed to decode
std::unordered_set<uint64_t> g_pbr_pending;          // hashes currently queued/decoding on background workers
std::unordered_map<uint64_t, std::unique_ptr<PbrMaps>> g_pbr_ready;  // decoded and waiting for GPU upload
std::unordered_set<uint64_t> g_pbr_used;             // unique hashes whose PBR maps were loaded this run
std::deque<std::pair<uint64_t, fs::path>> g_pbr_queue;
uint32_t g_pbr_workers = 0;
constexpr uint32_t kMaxPbrWorkers = 2;

// replacement images decoded off the render thread (PollImage)
std::unordered_set<uint64_t> g_image_failed;
std::unordered_set<uint64_t> g_image_pending;
std::unordered_map<uint64_t, std::unique_ptr<Image>> g_image_ready;
std::deque<std::pair<uint64_t, fs::path>> g_image_queue;
uint32_t g_image_workers = 0;
constexpr uint32_t kMaxImageWorkers = 3;

fs::path Root() { return app::ExeDir() / "textures"; }

fs::path PbrDir() {
    const std::string& custom = app::Get().pbr.pack_path;
    if (!custom.empty()) {
        std::error_code ec;
        fs::path p = hostpath::FromUtf8(custom);
        if (p.is_relative()) p = app::ExeDir() / p;
        if (fs::is_directory(p, ec)) return p;
    }
    return Root() / "pbr";
}

// the replace and pbr folders, read once: any .png/.jpg/.bmp whose name starts with 16 hex digits
void Index() {
    if (g_indexed) return;
    g_indexed = true;
    std::error_code ec;
    fs::path dir = Root() / "replace";
    fs::create_directories(dir, ec);
    fs::create_directories(Root() / "dump", ec);
    for (auto it = fs::recursive_directory_iterator(dir, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (!it->is_regular_file()) continue;
        std::string ext = hostpath::Utf8(it->path().extension());
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (ext != ".png" && ext != ".jpg" && ext != ".jpeg" && ext != ".bmp") continue;
        std::string name = hostpath::Utf8(it->path().stem());
        if (name.size() < 16) continue;
        uint64_t h = 0;
        if (std::sscanf(name.c_str(), "%16llx", (unsigned long long*)&h) == 1) g_index[h] = it->path();
    }
    // Preserve the experiment for review, but HD replacement indexing must never scan/adopt its maps.
    if constexpr (LegacyPbrCompanionsAllowed()) {
        fs::path pbr_dir = PbrDir();
        ec.clear();
        if (fs::is_directory(pbr_dir, ec)) {
            for (auto it = fs::directory_iterator(pbr_dir, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
                if (!it->is_regular_file()) continue;
                std::string ext = hostpath::Utf8(it->path().extension());
                std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                if (ext != ".png") continue;
                std::string name = hostpath::Utf8(it->path().stem());
                if (name.size() != 18 || (name.compare(16, 2, "_n") != 0 && name.compare(16, 2, "_N") != 0)) continue;
                uint64_t h = 0;
                if (std::sscanf(name.c_str(), "%16llx", (unsigned long long*)&h) == 1) g_pbr_index[h] = it->path();
            }
        }
    }
    LOGR("textures: %zu replacements in %s; filename PBR companions quarantined", g_index.size(),
         hostpath::Utf8(dir).c_str());
}

IWICImagingFactory* Wic() {
    thread_local ComPtr<IWICImagingFactory> factory = [] {
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

// renormalized tangent-space normal mip chain (BGRA8: B = nz, G = ny, R = nx, A = 255)
void BuildNormalMips(Image& img) {
    uint32_t w = img.width, h = img.height;
    while (w > 1 || h > 1) {
        const std::vector<uint8_t>& src = img.levels.back();
        uint32_t nw = std::max(w / 2, 1u), nh = std::max(h / 2, 1u);
        std::vector<uint8_t> dst(size_t(nw) * nh * 4);
        for (uint32_t y = 0; y < nh; y++) {
            for (uint32_t x = 0; x < nw; x++) {
                float vx = 0.0f, vy = 0.0f, vz = 0.0f;
                for (uint32_t dy = 0; dy < 2; dy++) {
                    for (uint32_t dx = 0; dx < 2; dx++) {
                        uint32_t sx = std::min(x * 2 + dx, w - 1), sy = std::min(y * 2 + dy, h - 1);
                        const uint8_t* p = &src[(size_t(sy) * w + sx) * 4];
                        vz += float(p[0]) * (2.0f / 255.0f) - 1.0f;
                        vy += float(p[1]) * (2.0f / 255.0f) - 1.0f;
                        vx += float(p[2]) * (2.0f / 255.0f) - 1.0f;
                    }
                }
                float len = std::sqrt(vx * vx + vy * vy + vz * vz);
                if (len > 1e-6f) {
                    vx /= len;
                    vy /= len;
                    vz = std::max(vz / len, 0.0f);
                } else {
                    vx = 0.0f;
                    vy = 0.0f;
                    vz = 1.0f;
                }
                uint8_t* out = &dst[(size_t(y) * nw + x) * 4];
                out[0] = uint8_t(std::clamp(std::lround((vz * 0.5f + 0.5f) * 255.0f), 128l, 255l));
                out[1] = uint8_t(std::clamp(std::lround((vy * 0.5f + 0.5f) * 255.0f), 0l, 255l));
                out[2] = uint8_t(std::clamp(std::lround((vx * 0.5f + 0.5f) * 255.0f), 0l, 255l));
                out[3] = 255;
            }
        }
        img.levels.push_back(std::move(dst));
        w = nw;
        h = nh;
    }
}

bool LoadRawBgra(const fs::path& path, uint32_t& width, uint32_t& height, std::vector<uint8_t>& out) {
    IWICImagingFactory* f = Wic();
    if (!f) return false;
    ComPtr<IWICBitmapDecoder> dec;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> conv;
    if (FAILED(f->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec)) ||
        FAILED(dec->GetFrame(0, &frame)) || FAILED(f->CreateFormatConverter(&conv)) ||
        FAILED(conv->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                WICBitmapPaletteTypeCustom)))
        return false;
    if (FAILED(conv->GetSize(&width, &height)) || !width || !height || width > 16384 || height > 16384)
        return false;
    out.resize(size_t(width) * height * 4);
    return SUCCEEDED(conv->CopyPixels(nullptr, width * 4, UINT(out.size()), out.data()));
}

bool LoadGrayChannel(const fs::path& path, uint32_t expected_w, uint32_t expected_h, std::vector<uint8_t>& out) {
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) return false;
    IWICImagingFactory* f = Wic();
    if (!f) return false;
    ComPtr<IWICBitmapDecoder> dec;
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(f->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec)) ||
        FAILED(dec->GetFrame(0, &frame)))
        return false;
    UINT w = 0, h = 0;
    if (FAILED(frame->GetSize(&w, &h)) || w != expected_w || h != expected_h) return false;
    ComPtr<IWICFormatConverter> conv;
    if (SUCCEEDED(f->CreateFormatConverter(&conv)) &&
        SUCCEEDED(conv->Initialize(frame.Get(), GUID_WICPixelFormat8bppGray, WICBitmapDitherTypeNone, nullptr, 0,
                                   WICBitmapPaletteTypeCustom))) {
        out.resize(size_t(w) * h);
        if (SUCCEEDED(conv->CopyPixels(nullptr, w, UINT(out.size()), out.data()))) return true;
    }
    std::vector<uint8_t> bgra;
    uint32_t bw = 0, bh = 0;
    if (!LoadRawBgra(path, bw, bh, bgra) || bw != expected_w || bh != expected_h) return false;
    out.resize(size_t(w) * h);
    for (size_t i = 0, n = out.size(); i < n; ++i) out[i] = bgra[i * 4];
    return true;
}

std::unique_ptr<Image> Load(const fs::path& path) {
    auto img = std::make_unique<Image>();
    std::vector<uint8_t> top;
    if (!LoadRawBgra(path, img->width, img->height, top)) return nullptr;
    img->levels.push_back(std::move(top));
    BuildMips(*img);
    return img;
}

std::unique_ptr<PbrMaps> LoadPbr(const fs::path& nrm_path) {
    auto maps = std::make_unique<PbrMaps>();
    std::vector<uint8_t> nrm_top;
    if (!LoadRawBgra(nrm_path, maps->nrm.width, maps->nrm.height, nrm_top)) return nullptr;
    const uint32_t w = maps->nrm.width, h = maps->nrm.height;
    for (size_t i = 0, n = size_t(w) * h; i < n; ++i) nrm_top[i * 4 + 3] = 255;
    maps->nrm.levels.push_back(std::move(nrm_top));
    BuildNormalMips(maps->nrm);

    const fs::path dir = nrm_path.parent_path();
    const std::string stem = hostpath::Utf8(nrm_path.stem());
    const std::string prefix = stem.size() >= 16 ? stem.substr(0, 16) : stem;
    std::vector<uint8_t> ao_ch, r_ch, m_ch, h_ch;
    LoadGrayChannel(dir / (prefix + "_ao.png"), w, h, ao_ch);
    LoadGrayChannel(dir / (prefix + "_r.png"), w, h, r_ch);
    LoadGrayChannel(dir / (prefix + "_m.png"), w, h, m_ch);
    maps->has_height = LoadGrayChannel(dir / (prefix + "_h.png"), w, h, h_ch);

    maps->orm.width = w;
    maps->orm.height = h;
    std::vector<uint8_t> orm_top(size_t(w) * h * 4);
    const size_t pixels = size_t(w) * h;
    for (size_t i = 0; i < pixels; ++i) {
        // BGRA8 memory layout -> shader .r = AO (byte 2), .g = Roughness (byte 1), .b = Metalness (byte 0), .a = Height (byte 3)
        orm_top[i * 4 + 0] = m_ch.empty() ? 0 : m_ch[i];
        orm_top[i * 4 + 1] = r_ch.empty() ? 128 : r_ch[i];
        orm_top[i * 4 + 2] = ao_ch.empty() ? 255 : ao_ch[i];
        orm_top[i * 4 + 3] = h_ch.empty() ? 128 : h_ch[i];
    }
    maps->orm.levels.push_back(std::move(orm_top));
    BuildMips(maps->orm);
    return maps;
}

fs::path LookupPbrPathLocked(uint64_t hash) {
    auto it = g_pbr_index.find(hash);
    if (it != g_pbr_index.end()) return it->second;
    if (g_pbr_missing.contains(hash)) return {};
    char name[32];
    std::snprintf(name, sizeof(name), "%016llx_n.png", (unsigned long long)hash);
    fs::path p = PbrDir() / name;
    std::error_code ec;
    if (fs::is_regular_file(p, ec)) {
        g_pbr_index[hash] = p;
        return p;
    }
    g_pbr_missing.insert(hash);
    return {};
}

std::unique_ptr<Image> Load(const fs::path& path);

void ImageWorker() {
    for (;;) {
        uint64_t hash = 0;
        fs::path path;
        {
            std::lock_guard<std::mutex> lock(g_lock);
            if (g_image_queue.empty()) {
                --g_image_workers;
                return;
            }
            hash = g_image_queue.front().first;
            path = std::move(g_image_queue.front().second);
            g_image_queue.pop_front();
        }
        std::unique_ptr<Image> img = Load(path);
        if (!img) LOGE("textures: could not read %s", hostpath::Utf8(path).c_str());
        std::lock_guard<std::mutex> lock(g_lock);
        g_image_pending.erase(hash);
        if (!img) {
            g_image_failed.insert(hash);
            continue;
        }
        if (g_used_hashes.insert(hash).second)
            LOGI("textures: %016llX from %s, %ux%u", (unsigned long long)hash, hostpath::Utf8(path.filename()).c_str(),
                 img->width, img->height);
        g_image_ready[hash] = std::move(img);
    }
}

void PbrWorker() {
    for (;;) {
        uint64_t hash = 0;
        fs::path path;
        {
            std::lock_guard<std::mutex> lock(g_lock);
            if (g_pbr_queue.empty()) {
                --g_pbr_workers;
                return;
            }
            hash = g_pbr_queue.front().first;
            path = std::move(g_pbr_queue.front().second);
            g_pbr_queue.pop_front();
        }
        std::unique_ptr<PbrMaps> maps = LoadPbr(path);
        {
            std::lock_guard<std::mutex> lock(g_lock);
            g_pbr_pending.erase(hash);
            if (maps) {
                g_pbr_used.insert(hash);
                g_pbr_ready[hash] = std::move(maps);
            } else {
                g_pbr_failed.insert(hash);
            }
        }
    }
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

bool PbrEnabled() {
    if constexpr (!LegacyPbrCompanionsAllowed()) return false;
    if (!app::Get().pbr.enabled) return false;
    std::lock_guard<std::mutex> lock(g_lock);
    Index();
    return !g_pbr_index.empty();
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
    if (!img) LOGE("textures: could not read %s", hostpath::Utf8(path).c_str());
    else {
        std::lock_guard<std::mutex> lock(g_lock);
        if (g_used_hashes.insert(hash).second)
            LOGI("textures: %016llX from %s, %ux%u", (unsigned long long)hash, hostpath::Utf8(path.filename()).c_str(),
                 img->width, img->height);
    }
    return img;
}

PbrPoll PollImage(uint64_t hash, std::unique_ptr<Image>& out) {
    out.reset();
    std::lock_guard<std::mutex> lock(g_lock);
    Index();
    if (auto ready = g_image_ready.find(hash); ready != g_image_ready.end()) {
        out = std::move(ready->second);
        g_image_ready.erase(ready);
        return PbrPoll::Ready;
    }
    if (g_image_failed.contains(hash)) return PbrPoll::None;
    if (g_image_pending.contains(hash)) return PbrPoll::Pending;
    auto at = g_index.find(hash);
    if (at == g_index.end()) return PbrPoll::None;
    g_image_pending.insert(hash);
    g_image_queue.emplace_back(hash, at->second);
    if (g_image_workers < kMaxImageWorkers) {
        ++g_image_workers;
        std::thread(ImageWorker).detach();
    }
    return PbrPoll::Pending;
}

std::unique_ptr<PbrMaps> FindPbr(uint64_t hash) {
    if constexpr (!LegacyPbrCompanionsAllowed()) return nullptr;
    fs::path path;
    {
        std::lock_guard<std::mutex> lock(g_lock);
        Index();
        path = LookupPbrPathLocked(hash);
        if (path.empty()) return nullptr;
    }
    std::unique_ptr<PbrMaps> maps = LoadPbr(path);
    if (maps) {
        std::lock_guard<std::mutex> lock(g_lock);
        g_pbr_used.insert(hash);
    }
    return maps;
}

PbrPoll PollPbr(uint64_t hash, std::unique_ptr<PbrMaps>& out) {
    out.reset();
    if constexpr (!LegacyPbrCompanionsAllowed()) return PbrPoll::None;
    std::lock_guard<std::mutex> lock(g_lock);
    Index();
    auto ready = g_pbr_ready.find(hash);
    if (ready != g_pbr_ready.end()) {
        out = std::move(ready->second);
        g_pbr_ready.erase(ready);
        return PbrPoll::Ready;
    }
    if (g_pbr_failed.contains(hash)) return PbrPoll::None;
    if (g_pbr_pending.contains(hash)) return PbrPoll::Pending;
    fs::path path = LookupPbrPathLocked(hash);
    if (path.empty()) return PbrPoll::None;
    g_pbr_pending.insert(hash);
    g_pbr_queue.emplace_back(hash, std::move(path));
    if (g_pbr_workers < kMaxPbrWorkers) {
        ++g_pbr_workers;
        std::thread(PbrWorker).detach();
    }
    return PbrPoll::Pending;
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
    return {uint32_t(g_index.size()), uint32_t(g_used_hashes.size()), g_dumped_count.load(),
            uint32_t(g_pbr_index.size()), uint32_t(g_pbr_used.size())};
}

}  // namespace gpu::pack
