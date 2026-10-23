#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cwctype>
#include <fstream>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>
#include <vector>

#include "app/portable_content.h"
#include "core/log.h"

namespace app::portable {
namespace {
namespace fs = std::filesystem;
constexpr uint64_t kChunkLimit = 1900ull << 20;
constexpr uint64_t kArchiveLimit = 512ull << 30;
constexpr uint64_t kMetadataLimit = 1ull << 20;

void Require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
std::string Hex(const uint8_t* data, size_t count) {
    std::string hex;
    for (size_t i = 0; i < count; i++) { hex += "0123456789abcdef"[data[i] >> 4]; hex += "0123456789abcdef"[data[i] & 15]; }
    return hex;
}
bool ValidHash(std::string_view hash) {
    return hash.size() == 64 && std::all_of(hash.begin(), hash.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}
uint64_t Number(std::string_view text) {
    uint64_t result = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
    Require(parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size(), "Invalid content size.");
    return result;
}
class Hash {
public:
    Hash() {
        Require(BCryptOpenAlgorithmProvider(&algorithm_, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0, "SHA-256 is unavailable.");
        if (BCryptCreateHash(algorithm_, &hash_, nullptr, 0, nullptr, 0, 0) < 0) {
            BCryptCloseAlgorithmProvider(algorithm_, 0); algorithm_ = nullptr;
            throw std::runtime_error("Cannot create SHA-256 context.");
        }
    }
    ~Hash() { if (hash_) BCryptDestroyHash(hash_); if (algorithm_) BCryptCloseAlgorithmProvider(algorithm_, 0); }
    void Add(const void* data, size_t count) { Require(count <= ULONG_MAX && BCryptHashData(hash_, (PUCHAR)data, ULONG(count), 0) >= 0, "SHA-256 update failed."); }
    std::string Finish() { uint8_t digest[32]; Require(BCryptFinishHash(hash_, digest, sizeof(digest), 0) >= 0, "SHA-256 failed."); return Hex(digest, sizeof(digest)); }
private:
    BCRYPT_ALG_HANDLE algorithm_ = nullptr;
    BCRYPT_HASH_HANDLE hash_ = nullptr;
};
std::string FileHash(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    Require(bool(file), "Cannot read a content file.");
    std::vector<char> buffer(1 << 20);
    Hash hash;
    while (file) { file.read(buffer.data(), buffer.size()); hash.Add(buffer.data(), size_t(file.gcount())); }
    Require(file.eof(), "Content file read failed.");
    return hash.Finish();
}
bool Matches(const fs::path& path, uint64_t size, std::string_view hash) {
    std::error_code ec;
    return fs::is_regular_file(path, ec) && fs::file_size(path, ec) == size && !ec && FileHash(path) == hash;
}
std::string Metadata(const fs::path& path) {
    std::error_code ec;
    Require(fs::file_size(path, ec) <= kMetadataLimit && !ec, "Missing or oversized content metadata.");
    std::ifstream file(path, std::ios::binary);
    Require(bool(file), "Cannot open content metadata.");
    return std::string(std::istreambuf_iterator<char>(file), {});
}
std::vector<std::string> Fields(const std::string& line) {
    std::vector<std::string> fields;
    size_t at = 0;
    for (;;) { const auto end = line.find('\t', at); fields.push_back(line.substr(at, end - at)); if (end == std::string::npos) return fields; at = end + 1; }
}
bool AssetName(std::string_view name) {
    return !name.empty() && name.size() < 200 && name.front() != '.' &&
        std::all_of(name.begin(), name.end(), [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_'; });
}
void NoLink(const fs::path& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    Require(attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_REPARSE_POINT), "Content setup will not write through a link or junction.");
}
struct Chunk { std::string name, hash; uint64_t size; };
struct Manifest { uint64_t size = 0; std::string hash; std::vector<Chunk> chunks; };
Manifest ParseManifest(const std::string& text) {
    std::istringstream input(text);
    std::string line;
    Require(bool(std::getline(input, line)) && line == "MC3-CONTENT\t1", "Unknown content manifest format.");
    Manifest manifest;
    uint64_t total = 0;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        auto fields = Fields(line);
        if (fields[0] == "archive") {
            Require(fields.size() == 3 && !manifest.size && ValidHash(fields[2]), "Invalid content archive record.");
            manifest.size = Number(fields[1]); manifest.hash = fields[2];
            Require(manifest.size > 12 && manifest.size <= kArchiveLimit, "Content archive is too large.");
        } else {
            Require(fields.size() == 4 && fields[0] == "chunk" && AssetName(fields[1]) && ValidHash(fields[3]), "Invalid content chunk record.");
            const uint64_t size = Number(fields[2]);
            Require(size > 0 && size <= kChunkLimit && total <= kArchiveLimit - size && manifest.chunks.size() < 512, "Content chunk is too large.");
            Require(std::none_of(manifest.chunks.begin(), manifest.chunks.end(), [&](const Chunk& chunk) { return chunk.name == fields[1]; }), "Duplicate content chunk.");
            total += size; manifest.chunks.push_back({fields[1], fields[3], size});
        }
    }
    Require(manifest.size && manifest.size == total && !manifest.chunks.empty(), "Incomplete content manifest.");
    return manifest;
}

class Progress {
public:
    Progress() {
        WNDCLASSW cls{}; cls.lpfnWndProc = Procedure; cls.hInstance = GetModuleHandleW(nullptr);
        cls.lpszClassName = L"MC3NativeContentSetup"; cls.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)); cls.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        RegisterClassW(&cls);
        hwnd_ = CreateWindowExW(WS_EX_APPWINDOW, cls.lpszClassName, L"Midnight Club 3 — first launch setup", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
                                CW_USEDEFAULT, CW_USEDEFAULT, 660, 200, nullptr, nullptr, cls.hInstance, this);
        Require(hwnd_ != nullptr, "Cannot open setup progress window.");
        text_ = CreateWindowW(L"STATIC", L"Preparing game content…", WS_CHILD | WS_VISIBLE, 24, 24, 600, 74, hwnd_, nullptr, cls.hInstance, nullptr);
        CreateWindowW(L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, 520, 110, 100, 30, hwnd_, (HMENU)1, cls.hInstance, nullptr);
        ShowWindow(hwnd_, SW_SHOW); UpdateWindow(hwnd_);
    }
    ~Progress() { if (hwnd_) DestroyWindow(hwnd_); }
    void Update(const std::wstring& status) {
        SetWindowTextW(text_, status.c_str());
        MSG message;
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
        Require(!cancelled_, "Setup cancelled. Run mc3.exe again to resume.");
    }
private:
    static LRESULT CALLBACK Procedure(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
        auto* self = reinterpret_cast<Progress*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE) { self = static_cast<Progress*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams); SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)self); }
        if (self && (message == WM_CLOSE || (message == WM_COMMAND && LOWORD(wp) == 1))) { self->cancelled_ = true; return 0; }
        return DefWindowProcW(hwnd, message, wp, lp);
    }
    HWND hwnd_ = nullptr, text_ = nullptr;
    bool cancelled_ = false;
};
struct HttpHandle {
    HINTERNET value = nullptr;
    explicit HttpHandle(HINTERNET handle) : value(handle) {}
    ~HttpHandle() { if (value) WinHttpCloseHandle(value); }
    operator HINTERNET() const { return value; }
};
void Download(const std::wstring& url, const fs::path& path, uint64_t size, std::string_view hash, Progress& progress) {
    const fs::path partial = path.wstring() + L".part";
    NoLink(path); NoLink(partial);
    if (Matches(path, size, hash)) return;
    std::error_code ec;
    uint64_t resume = fs::is_regular_file(partial, ec) ? fs::file_size(partial, ec) : 0;
    if (ec || resume >= size) { fs::remove(partial, ec); resume = 0; }
    URL_COMPONENTS components{sizeof(components)};
    components.dwHostNameLength = components.dwUrlPathLength = components.dwExtraInfoLength = components.dwUserNameLength = components.dwPasswordLength = DWORD(-1);
    Require(WinHttpCrackUrl(url.c_str(), 0, 0, &components) && components.nScheme == INTERNET_SCHEME_HTTPS && !components.dwUserNameLength && !components.dwPasswordLength, "Content download requires an HTTPS URL without credentials.");
    std::wstring host(components.lpszHostName, components.dwHostNameLength), request_path(components.lpszUrlPath, components.dwUrlPathLength);
    if (components.dwExtraInfoLength) request_path.append(components.lpszExtraInfo, components.dwExtraInfoLength);
    HttpHandle session(WinHttpOpen(L"MC3-Native-Portable/1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    Require(session.value != nullptr, "Cannot start HTTPS download.");
    WinHttpSetTimeouts(session, 10000, 15000, 30000, 30000);
    HttpHandle connection(WinHttpConnect(session, host.c_str(), components.nPort, 0));
    Require(connection.value != nullptr, "Cannot connect to the content release.");
    HttpHandle request(WinHttpOpenRequest(connection, L"GET", request_path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
    Require(request.value != nullptr, "Cannot request a content chunk.");
    DWORD redirect = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
    WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &redirect, sizeof(redirect));
    if (resume) {
        const std::wstring range = L"Range: bytes=" + std::to_wstring(resume) + L"-\r\n";
        Require(WinHttpAddRequestHeaders(request, range.c_str(), DWORD(range.size()), WINHTTP_ADDREQ_FLAG_ADD), "Cannot resume the content download.");
    }
    Require(WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(request, nullptr), "Content download failed. Check your connection and run mc3.exe again to resume.");
    DWORD status = 0, status_size = sizeof(status);
    Require(WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size, WINHTTP_NO_HEADER_INDEX), "Cannot read the download status.");
    Require(status == 200 || (resume && status == 206), "Content release download is unavailable.");
    if (status == 200) resume = 0;
    else {
        wchar_t content_range[160]; DWORD range_size = sizeof(content_range);
        const std::wstring expected = L"bytes " + std::to_wstring(resume) + L"-";
        Require(WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_RANGE, WINHTTP_HEADER_NAME_BY_INDEX, content_range, &range_size, WINHTTP_NO_HEADER_INDEX) && std::wstring_view(content_range).starts_with(expected), "Invalid resumed download range.");
    }
    std::ofstream output(partial, std::ios::binary | (resume ? std::ios::app : std::ios::trunc));
    Require(bool(output), "Cannot write the content download. Extract to a writable folder.");
    std::vector<char> buffer(1 << 20);
    uint64_t received = resume;
    for (;;) {
        progress.Update(L"Downloading " + path.filename().wstring() + L"\n" + std::to_wstring(received >> 20) + L" / " + std::to_wstring(size >> 20) + L" MiB");
        DWORD count = 0;
        Require(WinHttpReadData(request, buffer.data(), DWORD(buffer.size()), &count), "Content download interrupted. Run mc3.exe again to resume.");
        if (!count) break;
        Require(received <= size && count <= size - received, "Downloaded chunk is larger than its manifest.");
        output.write(buffer.data(), count); Require(bool(output), "Content download write failed (check free disk space).");
        received += count;
    }
    output.flush(); Require(bool(output), "Content download flush failed."); output.close();
    Require(received == size, "Content download was incomplete. Run mc3.exe again to resume.");
    if (FileHash(partial) != hash) { fs::remove(partial, ec); throw std::runtime_error("Content download checksum failed. Run mc3.exe again to retry."); }
    Require(MoveFileExW(partial.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH), "Cannot publish downloaded content chunk.");
}

class Archive {
public:
    Archive(const fs::path& folder, const Manifest& manifest) : folder_(folder), manifest_(manifest) {}
    void Read(void* target, size_t size) {
        auto* bytes = static_cast<char*>(target);
        while (size) {
            if (!remaining_) {
                Require(index_ < manifest_.chunks.size(), "Content archive ended early.");
                const auto& chunk = manifest_.chunks[index_++];
                if (file_.is_open()) file_.close();
                file_.clear(); file_.open(folder_ / chunk.name, std::ios::binary);
                Require(bool(file_), "Cannot read content chunk."); remaining_ = chunk.size;
            }
            const auto count = size_t(std::min<uint64_t>(remaining_, size));
            Require(bool(file_.read(bytes, count)), "Content chunk is truncated.");
            hash_.Add(bytes, count); bytes += count; size -= count; remaining_ -= count; consumed_ += count;
        }
    }
    template<class T> T Value() { T value; Read(&value, sizeof(value)); return value; }
    void Finish() { Require(consumed_ == manifest_.size && hash_.Finish() == manifest_.hash, "Content archive checksum failed."); }
private:
    fs::path folder_; const Manifest& manifest_; size_t index_ = 0; uint64_t remaining_ = 0, consumed_ = 0; std::ifstream file_; Hash hash_;
};
fs::path ContentPath(const fs::path& base, const std::string& name) {
    Require(!name.empty() && name.size() <= 4096 && name.find("//") == std::string::npos && name.find_first_of(std::string("\0\\:\t\r\n<>\"|?*", 13)) == std::string::npos, "Unsafe content path.");
    const fs::path relative{std::u8string(reinterpret_cast<const char8_t*>(name.data()), name.size())};
    Require(!relative.is_absolute() && !relative.has_root_path(), "Absolute content paths are forbidden.");
    for (const auto& part : relative) {
        Require(part != L".." && part != L"." && !part.empty() && part.wstring().back() != L'.' && part.wstring().back() != L' ', "Unsafe content path component.");
        auto stem = part.wstring().substr(0, part.wstring().find('.'));
        std::transform(stem.begin(), stem.end(), stem.begin(), [](wchar_t c) { return wchar_t(std::towupper(c)); });
        Require(stem != L"CON" && stem != L"PRN" && stem != L"AUX" && stem != L"NUL" && !(stem.size() == 4 && (stem.starts_with(L"COM") || stem.starts_with(L"LPT")) && stem[3] >= L'1' && stem[3] <= L'9'), "Reserved Windows content filename.");
    }
    const bool iso = name.starts_with("game/") && relative.extension() == L".iso" && std::distance(relative.begin(), relative.end()) == 2;
    const bool texture = name.starts_with("textures/replace/") && relative.extension() == L".png" && std::distance(relative.begin(), relative.end()) == 3;
    const bool movie = name.starts_with("video_hd/") && relative.extension() == L".mp4";
    Require(iso || texture || movie, "Content archive attempts to write outside game/media folders.");
    fs::path current = base;
    for (const auto& part : relative) {
        current /= part;
        const DWORD attributes = GetFileAttributesW(current.c_str());
        Require(attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_REPARSE_POINT), "Content path crosses a link or junction.");
    }
    return base / relative;
}
void Extract(const fs::path& base, const fs::path& cache, const Manifest& manifest, Progress& progress) {
    Archive archive(cache, manifest);
    char magic[8]; archive.Read(magic, sizeof(magic));
    Require(std::string_view(magic, 8) == "MC3C0001", "Unknown content archive format.");
    const uint32_t count = archive.Value<uint32_t>();
    Require(count && count <= 200000, "Invalid number of content files.");
    std::vector<char> buffer(1 << 20);
    std::error_code ec;
    std::set<std::wstring> paths;
    for (uint32_t index = 0; index < count; index++) {
        const uint32_t path_size = archive.Value<uint32_t>(); const uint64_t size = archive.Value<uint64_t>();
        uint8_t digest[32]; archive.Read(digest, sizeof(digest));
        Require(path_size && path_size <= 4096 && size <= kArchiveLimit, "Invalid content file header.");
        std::string name(path_size, '\0'); archive.Read(name.data(), name.size());
        const fs::path target = ContentPath(base, name);
        auto key = target.wstring();
        std::transform(key.begin(), key.end(), key.begin(), [](wchar_t c) { return wchar_t(std::towlower(c)); });
        Require(paths.insert(key).second, "Duplicate installed content path.");
        progress.Update(L"Installing " + target.filename().wstring() + L"\nFile " + std::to_wstring(index + 1) + L" / " + std::to_wstring(count));
        const std::string hash = Hex(digest, sizeof(digest));
        const bool exists = Matches(target, size, hash);
        fs::create_directories(target.parent_path());
        const fs::path temporary = target.wstring() + L".installing";
        NoLink(temporary);
        std::ofstream output;
        if (!exists) { output.open(temporary, std::ios::binary | std::ios::trunc); Require(bool(output), "Cannot create installed content file."); }
        Hash file_hash;
        for (uint64_t left = size; left;) {
            const auto bytes = size_t(std::min<uint64_t>(buffer.size(), left));
            archive.Read(buffer.data(), bytes); file_hash.Add(buffer.data(), bytes);
            if (!exists) { output.write(buffer.data(), bytes); Require(bool(output), "Content installation write failed (check free disk space)."); }
            left -= bytes;
            progress.Update(L"Installing " + target.filename().wstring() + L"\nFile " + std::to_wstring(index + 1) + L" / " + std::to_wstring(count));
        }
        if (file_hash.Finish() != hash) { output.close(); fs::remove(temporary, ec); throw std::runtime_error("Installed content checksum failed."); }
        if (!exists) {
            output.flush(); Require(bool(output), "Content installation flush failed."); output.close();
            Require(MoveFileExW(temporary.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH), "Cannot publish an installed content file.");
        }
    }
    archive.Finish();
}
}  // namespace

bool InstallBundledContent(const fs::path& exe_dir, std::string& error) {
    std::error_code ec;
    const fs::path source = exe_dir / "content-source.txt";
    if (!fs::is_regular_file(source, ec)) return true;
    try {
        std::istringstream descriptor(Metadata(source));
        std::string line, url, hash; uint64_t size = 0;
        Require(bool(std::getline(descriptor, line)) && line == "MC3-CONTENT-SOURCE\t1", "Unknown content source format.");
        while (std::getline(descriptor, line)) {
            auto fields = Fields(line);
            Require(fields.size() == 2, "Invalid content source record.");
            if (fields[0] == "url") url = fields[1];
            else if (fields[0] == "sha256") hash = fields[1];
            else if (fields[0] == "size") size = Number(fields[1]);
            else throw std::runtime_error("Unknown content source record.");
        }
        Require(url.starts_with("https://github.com/") && url.find("/releases/download/") != std::string::npos && url.find_first_of("\r\n\t?#") == std::string::npos && ValidHash(hash) && size && size <= kMetadataLimit, "Invalid pinned content release.");
        const fs::path marker = exe_dir / "config/content-installed.txt";
        if (fs::is_regular_file(marker, ec) && Metadata(marker) == hash && fs::is_regular_file(exe_dir / "game/game.iso", ec)) return true;
        Progress progress;
        const fs::path manifest_path = exe_dir / "content-manifest.tsv";
        Download(std::wstring(url.begin(), url.end()), manifest_path, size, hash, progress);
        const auto manifest = ParseManifest(Metadata(manifest_path));
        const fs::path cache = exe_dir / ".content-cache";
        NoLink(cache);
        fs::create_directories(cache);
        const auto prefix = url.substr(0, url.rfind('/') + 1);
        for (const auto& chunk : manifest.chunks) {
            const fs::path local = exe_dir / chunk.name;
            const fs::path cached = cache / chunk.name;
            NoLink(local); NoLink(cached);
            if (Matches(local, chunk.size, chunk.hash)) {
                // Offline parts beside the exe are read in place through a cache hard link, or copied across volumes.
                if (!Matches(cached, chunk.size, chunk.hash)) {
                    fs::remove(cached, ec); fs::create_hard_link(local, cached, ec);
                    if (ec) { ec.clear(); fs::copy_file(local, cached, fs::copy_options::overwrite_existing, ec); Require(!ec, "Cannot stage offline content chunk."); }
                }
            } else {
                const auto chunk_url = prefix + chunk.name;
                Download(std::wstring(chunk_url.begin(), chunk_url.end()), cached, chunk.size, chunk.hash, progress);
            }
        }
        Extract(exe_dir, cache, manifest, progress);
        fs::create_directories(marker.parent_path());
        std::ofstream done(marker, std::ios::binary | std::ios::trunc); done << hash; done.flush(); Require(bool(done), "Cannot save content setup completion."); done.close();
        for (const auto& chunk : manifest.chunks) fs::remove(cache / chunk.name, ec);
        fs::remove(cache, ec);
        LOGR("setup: verified content installed (%zu chunks, %llu bytes)", manifest.chunks.size(), (unsigned long long)manifest.size);
        return true;
    } catch (const std::exception& failure) {
        error = failure.what(); LOGE("setup: %s", error.c_str()); return false;
    }
}
}  // namespace app::portable
