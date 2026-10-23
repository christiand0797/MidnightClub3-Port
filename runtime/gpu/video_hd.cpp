#include "gpu/video_hd.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <propvarutil.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <exception>
#include <filesystem>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include "app/app.h"
#include "core/log.h"
#include "core/host_path.h"
#include "gpu/texture.h"

namespace fs = std::filesystem;
using Microsoft::WRL::ComPtr;

namespace gpu::video {
namespace {

// one HD movie: a thread decodes frames in order up to the one the title is showing
struct Movie {
    std::string name;
    fs::path file;
    uint32_t data_va = 0;
    std::mutex lock;
    std::condition_variable wake;
    std::thread worker;
    bool stop = false;
    int64_t want = 0;               // the frame the title shows, 0 based
    std::vector<uint8_t> px;        // the latest decoded frame at or before want, BGRA
    uint32_t width = 0, height = 0;
    uint64_t serial = 0;
};

std::mutex g_lock;
std::unordered_map<uint32_t, std::shared_ptr<Movie>> g_by_data;  // TextureDesc::data
std::unordered_map<uint32_t, uint32_t> g_header_data;            // texture header to its data
std::atomic<uint64_t> g_serial{0};  // renderer cache tokens must stay unique when a data address is reused

// d:\video\Story\Sgameint.bik -> <exe>/video_hd/story/sgameint.mp4
fs::path HdFile(const std::string& title_path) {
    std::string p = title_path;
    std::transform(p.begin(), p.end(), p.begin(), [](char c) { return c == '\\' ? '/' : char(std::tolower(uint8_t(c))); });
    size_t at = p.find("video/");
    if (at != std::string::npos) p = p.substr(at + 6);
    if (size_t c = p.find(':'); c != std::string::npos) p = p.substr(c + 1);
    while (!p.empty() && p[0] == '/') p.erase(0, 1);
    fs::path rel(p);
    rel.replace_extension(".mp4");
    return app::ExeDir() / "video_hd" / rel;
}

struct ComApartment {
    HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ~ComApartment() {
        if (SUCCEEDED(result)) CoUninitialize();
    }
};

struct VideoLayout {
    uint32_t width = 0, height = 0;
    int32_t stride = 0;
    uint64_t extent = 0;
    double fps = 0;
};

bool ReadLayout(IMFSourceReader* reader, DWORD video, VideoLayout& out) {
    ComPtr<IMFMediaType> type;
    GUID subtype{};
    UINT32 num = 0, den = 0, stride = 0;
    if (FAILED(reader->GetCurrentMediaType(video, &type)) ||
        FAILED(type->GetGUID(MF_MT_SUBTYPE, &subtype)) || subtype != MFVideoFormat_RGB32 ||
        FAILED(MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &out.width, &out.height)) ||
        FAILED(MFGetAttributeRatio(type.Get(), MF_MT_FRAME_RATE, &num, &den)) || !num || !den ||
        !out.width || !out.height || out.width > 16384 || out.height > 16384)
        return false;
    out.stride = int32_t(out.width * 4);
    if (SUCCEEDED(type->GetUINT32(MF_MT_DEFAULT_STRIDE, &stride))) out.stride = int32_t(stride);
    const uint64_t pitch = out.stride < 0 ? uint64_t(-int64_t(out.stride)) : uint64_t(out.stride);
    const uint32_t row = out.width * 4;
    out.extent = pitch * (out.height - 1) + row;
    out.fps = double(num) / den;
    return pitch >= row && out.extent <= MAXDWORD && std::isfinite(out.fps) && out.fps > 0 && out.fps <= 1000;
}

void DecodeMovie(const std::shared_ptr<Movie>& m, const fs::path& file) {
    auto check = [&](HRESULT hr, const char* operation) {
        if (SUCCEEDED(hr)) return true;
        LOGE("movies: %s: %s failed (0x%08lX)", m->name.c_str(), operation, (unsigned long)hr);
        std::lock_guard<std::mutex> l(m->lock);
        m->serial = 0;  // the renderer resumes the original Bink picture
        m->px.clear();
        return false;
    };
    ComApartment apartment;
    if (!check(apartment.result, "COM initialization")) return;
    static std::once_flag mf_started;
    static HRESULT mf_result = E_FAIL;
    std::call_once(mf_started, [] { mf_result = MFStartup(MF_VERSION, MFSTARTUP_LITE); });
    if (!check(mf_result, "Media Foundation initialization")) return;
    ComPtr<IMFAttributes> attr;
    if (!check(MFCreateAttributes(&attr, 1), "creating reader attributes") ||
        !check(attr->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE), "enabling RGB conversion"))
        return;
    ComPtr<IMFSourceReader> reader;
    if (!check(MFCreateSourceReaderFromURL(file.wstring().c_str(), attr.Get(), &reader), "opening the HD file"))
        return;
    const DWORD video = DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
    if (!check(reader->SetStreamSelection(DWORD(MF_SOURCE_READER_ALL_STREAMS), FALSE), "deselecting streams") ||
        !check(reader->SetStreamSelection(video, TRUE), "selecting video"))
        return;
    ComPtr<IMFMediaType> want;
    if (!check(MFCreateMediaType(&want), "creating the RGB32 media type") ||
        !check(want->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video), "setting the video type") ||
        !check(want->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32), "setting the RGB32 subtype") ||
        !check(reader->SetCurrentMediaType(video, nullptr, want.Get()), "selecting RGB32 output"))
        return;
    VideoLayout layout;
    if (!check(ReadLayout(reader.Get(), video, layout) ? S_OK : E_INVALIDARG, "validating RGB32 layout")) return;
    LOGR("movies: %s from %s, %ux%u at %.2f fps", m->name.c_str(), hostpath::Utf8(file.filename()).c_str(),
         layout.width, layout.height, layout.fps);

    int64_t at = -1;  // the last frame decoded
    std::vector<uint8_t> frame(size_t(layout.width) * layout.height * 4);
    for (;;) {
        int64_t target;
        {
            std::unique_lock<std::mutex> l(m->lock);
            m->wake.wait(l, [&] { return m->stop || m->want > at || m->want + 2 < at; });
            if (m->stop) break;
            target = m->want;
        }
        if (target + 2 < at) {  // the title went back (a loop, a restart): seek to the start
            PROPVARIANT pos;
            InitPropVariantFromInt64(0, &pos);
            const HRESULT seek = reader->SetCurrentPosition(GUID_NULL, pos);
            PropVariantClear(&pos);
            if (!check(seek, "restarting the movie")) return;
            at = -1;
        }
        DWORD stream = 0, flags = 0;
        LONGLONG ts = 0;
        ComPtr<IMFSample> sample;
        if (!check(reader->ReadSample(video, 0, &stream, &flags, &ts, &sample), "reading a video sample") ||
            !check((flags & MF_SOURCE_READERF_ERROR) ? E_FAIL : S_OK, "decoding video"))
            return;
        if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) {
            if (!check(ReadLayout(reader.Get(), video, layout) ? S_OK : E_INVALIDARG, "validating changed RGB32 layout"))
                return;
            frame.resize(size_t(layout.width) * layout.height * 4);
        }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
            std::unique_lock<std::mutex> l(m->lock);  // past the HD file's end: hold the last frame
            m->wake.wait(l, [&] { return m->stop || m->want + 2 < at; });
            if (m->stop) break;
            continue;
        }
        if (!sample) continue;
        const double frame_number = double(ts) / 1e7 * layout.fps;
        if (!check(std::isfinite(frame_number) && frame_number >= 0 && frame_number <= MAXDWORD ? S_OK : E_INVALIDARG,
                   "validating the frame timestamp"))
            return;
        at = std::llround(frame_number);
        if (at < target) continue;  // catching up: only the frame shown is converted
        ComPtr<IMFMediaBuffer> buf;
        if (!check(sample->ConvertToContiguousBuffer(&buf), "reading decoded frame bytes")) return;
        BYTE* p = nullptr;
        DWORD len = 0;
        if (!check(buf->Lock(&p, nullptr, &len), "locking decoded frame bytes")) return;
        if (!p || layout.extent > len) {
            buf->Unlock();
            check(E_INVALIDARG, "validating decoded frame buffer extent");
            return;
        }
        const uint32_t row = layout.width * 4;
        const size_t pitch = layout.stride < 0 ? size_t(-int64_t(layout.stride)) : size_t(layout.stride);
        for (uint32_t y = 0; y < layout.height; y++) {
            const BYTE* src = p + size_t(layout.stride >= 0 ? y : layout.height - 1 - y) * pitch;
            uint8_t* dst = &frame[size_t(y) * row];
            std::memcpy(dst, src, row);
            for (uint32_t x = 3; x < row; x += 4) dst[x] = 255;  // RGB32 leaves alpha undefined; the title's is X8
        }
        if (!check(buf->Unlock(), "unlocking decoded frame bytes")) return;
        std::lock_guard<std::mutex> l(m->lock);
        if (m->stop) break;
        m->px.swap(frame);
        frame.resize(size_t(layout.width) * layout.height * 4);
        m->width = layout.width;
        m->height = layout.height;
        m->serial = ++g_serial;
    }
}

void Decode(std::shared_ptr<Movie> m, fs::path file) {
    try {
        DecodeMovie(m, file);
    } catch (const std::exception& e) {
        LOGE("movies: %s: decoder failed (%s)", m->name.c_str(), e.what());
        std::lock_guard<std::mutex> l(m->lock);
        m->serial = 0;
        m->px.clear();
    }
}

void Stop(const std::shared_ptr<Movie>& m) {
    {
        std::lock_guard<std::mutex> l(m->lock);
        m->stop = true;
    }
    m->wake.notify_all();
    if (m->worker.joinable()) m->worker.join();
}

}  // namespace

bool Enabled() { return app::Get().graphics.hd_movies; }

bool Open(uint32_t header_va, const std::string& path) {
    Close(header_va);
    const bool enabled = Enabled();
    TextureDesc d;
    if (!Describe(header_va, d)) return false;
    fs::path file = HdFile(path);
    std::error_code ec;
    if (!fs::exists(file, ec)) {
        LOGI("movies: %s has no HD file (%s)", path.c_str(), hostpath::Utf8(file).c_str());
        return false;
    }
    auto m = std::make_shared<Movie>();
    m->name = path;
    m->file = file;
    m->data_va = d.data;
    std::lock_guard<std::mutex> l(g_lock);
    if (auto old = g_by_data.find(d.data); old != g_by_data.end()) {
        auto previous = old->second;
        g_by_data.erase(old);
        for (auto h = g_header_data.begin(); h != g_header_data.end();) {
            if (h->second == d.data) h = g_header_data.erase(h);
            else ++h;
        }
        // Decode only takes its Movie's lock: joining here cannot re-enter the registration maps.
        Stop(previous);
    }
    g_by_data[d.data] = m;
    g_header_data[header_va] = d.data;
    // Remember a disabled clip too, so enabling HD during playback can start its decoder.
    if (enabled) m->worker = std::thread(Decode, m, file);
    return enabled;
}

void Frame(uint32_t header_va, uint32_t frame) {
    std::shared_ptr<Movie> m;
    {
        std::lock_guard<std::mutex> l(g_lock);
        auto h = g_header_data.find(header_va);
        if (h == g_header_data.end()) return;
        auto it = g_by_data.find(h->second);
        if (it == g_by_data.end()) return;
        m = it->second;
    }
    {
        std::lock_guard<std::mutex> l(m->lock);
        m->want = frame ? int64_t(frame) - 1 : 0;
        if (!m->stop && Enabled() && !m->worker.joinable())
            m->worker = std::thread(Decode, m, m->file);
    }
    m->wake.notify_all();
}

void Close(uint32_t header_va) {
    std::shared_ptr<Movie> m;
    {
        std::lock_guard<std::mutex> l(g_lock);
        auto h = g_header_data.find(header_va);
        if (h == g_header_data.end()) return;
        auto it = g_by_data.find(h->second);
        if (it != g_by_data.end()) {
            m = it->second;
            g_by_data.erase(it);
        }
        g_header_data.erase(h);
    }
    if (m) Stop(m);
}

bool Playing(uint32_t data_va) {
    if (!Enabled()) return false;
    std::lock_guard<std::mutex> l(g_lock);
    auto it = g_by_data.find(data_va);
    if (it == g_by_data.end()) return false;
    std::lock_guard<std::mutex> ml(it->second->lock);
    return it->second->serial != 0;  // until the first HD frame the title's own picture shows
}

bool Picture(uint32_t data_va, uint64_t& serial, const std::function<void(const uint8_t*, uint32_t, uint32_t)>& fn) {
    if (!Enabled()) return false;
    std::shared_ptr<Movie> m;
    {
        std::lock_guard<std::mutex> l(g_lock);
        auto it = g_by_data.find(data_va);
        if (it == g_by_data.end()) return false;
        m = it->second;
    }
    std::lock_guard<std::mutex> l(m->lock);
    if (!m->serial) return false;
    if (m->serial != serial) {
        fn(m->px.data(), m->width, m->height);
        serial = m->serial;
    }
    return true;
}

}  // namespace gpu::video
