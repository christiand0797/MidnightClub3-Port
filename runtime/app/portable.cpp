#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commdlg.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <algorithm>
#include <cwctype>
#include <fstream>
#include <iterator>

#include "app/portable.h"
#include "core/host_path.h"
#include "core/log.h"
#include "kernel/crypto.h"

namespace app::portable {
namespace {
namespace fs = std::filesystem;
constexpr auto kDiscPath = L"config\\disc-path.txt";

std::string Hash(const std::vector<uint8_t>& data) {
    alignas(8) uint8_t context[128], digest[20];
    crypto::ShaInit(context);
    crypto::ShaUpdate(context, data.data(), data.size());
    crypto::ShaFinal(context, digest);
    std::string hex;
    for (auto byte : digest) {
        hex += "0123456789abcdef"[byte >> 4];
        hex += "0123456789abcdef"[byte & 15];
    }
    return hex;
}

class ComScope {
public:
    ComScope() : status_(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)) {}
    ~ComScope() { if (SUCCEEDED(status_)) CoUninitialize(); }
    bool Available() const { return SUCCEEDED(status_) || status_ == RPC_E_CHANGED_MODE; }
private:
    HRESULT status_;
};
}  // namespace

fs::path ReadDiscPath(const fs::path& exe_dir) {
    std::ifstream file(hostpath::Native(exe_dir / kDiscPath), std::ios::binary);
    if (!file) return {};
    std::string text;
    // Do not let a damaged configuration allocate an arbitrarily large buffer.
    char ch;
    while (text.size() <= 65536 && file.get(ch)) text += ch;
    if (text.size() > 65536) return {};
    if (text.starts_with("\xef\xbb\xbf")) text.erase(0, 3);
    while (!text.empty() && (text.back() == '\r' || text.back() == '\n')) text.pop_back();
    if (text.empty() || text.find_first_of(std::string("\0\r\n", 3)) != std::string::npos) return {};
    try {
        fs::path path{std::u8string(reinterpret_cast<const char8_t*>(text.data()), text.size())};
        return hostpath::Native((path.is_absolute() ? path : exe_dir / path).lexically_normal());
    } catch (const fs::filesystem_error&) {
        return {};
    }
}

bool RememberDiscPath(const fs::path& exe_dir, const fs::path& disc_path) {
    std::error_code ec;
    const auto absolute = hostpath::Native(fs::absolute(disc_path, ec).lexically_normal());
    if (ec) return false;
    const auto base = hostpath::Native(fs::absolute(exe_dir, ec).lexically_normal());
    if (ec) return false;
    auto stored = absolute.lexically_relative(base);
    // Keep only paths inside the portable folder relative: a relocated external drive needs picking again.
    if (stored.empty() || *stored.begin() == L"..") stored = absolute;
    const auto bytes = stored.u8string();
    const auto destination = hostpath::Native(exe_dir / kDiscPath);
    fs::create_directories(destination.parent_path(), ec);
    if (ec) return false;
    const fs::path temporary = destination.wstring() + L".tmp." + std::to_wstring(GetCurrentProcessId());
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        if (!file) return false;
        file.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        file.put('\n');
        file.flush();
        if (!file) { file.close(); fs::remove(temporary, ec); return false; }
    }
    if (MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return true;
    fs::remove(temporary, ec);
    return false;
}

std::vector<fs::path> DiscCandidates(const fs::path& folder) {
    std::vector<fs::path> candidates;
    std::error_code ec;
    fs::directory_iterator it(folder, fs::directory_options::skip_permission_denied, ec), end;
    while (!ec && it != end) {
        auto extension = it->path().extension().wstring();
        std::transform(extension.begin(), extension.end(), extension.begin(), [](wchar_t c) { return wchar_t(std::towlower(c)); });
        if (extension == L".iso" && it->is_regular_file(ec)) candidates.push_back(it->path());
        it.increment(ec);
    }
    std::sort(candidates.begin(), candidates.end());
    return candidates;
}

bool OpenDisc(const fs::path& path, std::string_view expected_sha1, std::vector<uint8_t>& xbe,
              std::unique_ptr<kernel::Disc>& disc, std::string& error) {
    xbe.clear();
    disc.reset();
    auto candidate = std::make_unique<kernel::Disc>();
    if (!candidate->Open(path, error)) return false;
    const auto* entry = candidate->Find("default.xbe");
    if (!entry || entry->dir || entry->size > (64u << 20) || !candidate->Load("default.xbe", xbe)) {
        error = "The image has no readable default.xbe (maximum 64 MiB).";
        return false;
    }
    if (Hash(xbe) != expected_sha1) {
        xbe.clear();
        error = "This image is a different game or release. Select the original Xbox USA release of Midnight Club 3: DUB Edition Remix.";
        return false;
    }
    disc = std::move(candidate);
    return true;
}

fs::path PickDisc(const fs::path& initial_dir, const wchar_t* title) {
    std::vector<wchar_t> filename(32768);
    OPENFILENAMEW dialog{sizeof(dialog)};
    dialog.lpstrFilter = L"Original Xbox disc image (*.iso)\0*.iso\0All files (*.*)\0*.*\0\0";
    dialog.lpstrFile = filename.data();
    dialog.nMaxFile = DWORD(filename.size());
    dialog.lpstrInitialDir = initial_dir.c_str();
    dialog.lpstrTitle = title;
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER | OFN_NOCHANGEDIR | OFN_DONTADDTORECENT;
    return GetOpenFileNameW(&dialog) ? fs::path(filename.data()) : fs::path{};
}

bool WriteShortcut(const fs::path& exe, const fs::path& shortcut) {
    ComScope com;
    if (!com.Available()) return false;
    IShellLinkW* link = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link)))) return false;
    // Disc discovery reads config/disc-path.txt, so no private absolute game path is embedded in the shortcut.
    HRESULT status = link->SetPath(exe.c_str());
    if (SUCCEEDED(status)) status = link->SetWorkingDirectory(exe.parent_path().c_str());
    if (SUCCEEDED(status)) status = link->SetDescription(L"Midnight Club 3: DUB Edition Remix — native PC port");
    if (SUCCEEDED(status)) status = link->SetIconLocation(exe.c_str(), 0);
    IPersistFile* persist = nullptr;
    if (SUCCEEDED(status)) status = link->QueryInterface(IID_PPV_ARGS(&persist));
    if (SUCCEEDED(status)) status = persist->Save(shortcut.c_str(), TRUE);
    if (persist) persist->Release();
    link->Release();
    return SUCCEEDED(status);
}

void EnsureDesktopShortcut() {
    std::vector<wchar_t> filename(32768);
    const DWORD size = GetModuleFileNameW(nullptr, filename.data(), DWORD(filename.size()));
    if (!size || size >= filename.size()) return;
    const fs::path exe = hostpath::Native(fs::path(filename.data()));
    // The marker belongs to this extracted location. Moving the folder recreates the shortcut on next launch.
    const auto marker = hostpath::Native(exe.parent_path() / "config" / "desktop-shortcut.txt");
    const auto expected = exe.u8string();
    std::ifstream previous(marker, std::ios::binary);
    const std::string previous_path{std::istreambuf_iterator<char>(previous), {}};
    PWSTR desktop = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Desktop, KF_FLAG_DEFAULT, nullptr, &desktop))) return;
    const fs::path shortcut = fs::path(desktop) / L"Midnight Club 3 Remix (Native).lnk";
    CoTaskMemFree(desktop);
    std::error_code ec;
    if (previous_path == std::string(reinterpret_cast<const char*>(expected.data()), expected.size()) && fs::is_regular_file(shortcut, ec)) return;
    if (!WriteShortcut(exe, shortcut)) { LOGW("setup: desktop shortcut could not be written"); return; }
    fs::create_directories(marker.parent_path(), ec);
    std::ofstream record(marker, std::ios::binary | std::ios::trunc);
    record.write(reinterpret_cast<const char*>(expected.data()), expected.size());
    LOGR("setup: desktop shortcut ready");
}

}  // namespace app::portable
