#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shobjidl.h>

#include <cassert>
#include <cstring>
#include <fstream>

#include "app/portable.h"
#include "core/log.h"

namespace logx { void Write(Level, const char*, ...) {} }

namespace {
namespace fs = std::filesystem;
void Write(const fs::path& path, const std::string& data) {
    std::ofstream file(path, std::ios::binary);
    file.write(data.data(), data.size());
    assert(bool(file));
}
void SyntheticDisc(const fs::path& path, const char* name = "default.xbe", const char* payload = "abc") {
    // A complete tiny XDVDFS image made entirely of this test's own data.
    std::string iso(35 * 2048, '\0');
    constexpr char magic[] = "MICROSOFT*XBOX*MEDIA";
    auto* volume = iso.data() + 32 * 2048;
    std::memcpy(volume, magic, sizeof(magic) - 1);
    std::memcpy(volume + 0x7EC, magic, sizeof(magic) - 1);
    uint32_t root_sector = 33, root_size = 32, xbe_sector = 34, xbe_size = uint32_t(std::strlen(payload));
    std::memcpy(volume + 0x14, &root_sector, 4);
    std::memcpy(volume + 0x18, &root_size, 4);
    auto* entry = iso.data() + 33 * 2048;
    std::memcpy(entry + 4, &xbe_sector, 4);
    std::memcpy(entry + 8, &xbe_size, 4);
    entry[13] = char(std::strlen(name));
    std::memcpy(entry + 14, name, std::strlen(name));
    std::memcpy(iso.data() + 34 * 2048, payload, xbe_size);
    Write(path, iso);
}
}  // namespace

int main() {
    const auto temp = fs::absolute(fs::temp_directory_path());
    const auto root = temp / ("mc3_portable_test_" + std::to_string(GetCurrentProcessId()) + "_" + std::to_string(GetTickCount64()));
    assert(fs::equivalent(root.parent_path(), temp) && fs::create_directory(root));
    const auto portable = root / L"portable_日本語 spaces";
    const auto relocated = root / "relocated";
    assert(fs::create_directory(portable));
    assert(fs::create_directory(relocated));
    const auto disc_path = portable / L"MC3_日本語 USA.xiso.ISO";
    SyntheticDisc(disc_path);
    Write(portable / "not-a-disc.txt", "do not scan this");
    fs::create_directory(portable / "directory.iso");
    const auto candidates = app::portable::DiscCandidates(portable);
    assert(candidates.size() == 1 && candidates.front() == disc_path);

    assert(app::portable::ReadDiscPath(portable).empty());
    assert(app::portable::RememberDiscPath(portable, disc_path));
    assert(app::portable::ReadDiscPath(portable) == disc_path);
    fs::create_directory(relocated / "config");
    fs::copy_file(portable / "config/disc-path.txt", relocated / "config/disc-path.txt");
    assert(app::portable::ReadDiscPath(relocated) == relocated / disc_path.filename());
    const auto external = root / "external.iso";
    assert(app::portable::RememberDiscPath(portable, external));
    assert(app::portable::ReadDiscPath(portable) == external);
    Write(portable / "config/disc-path.txt", std::string(65538, 'x'));
    assert(app::portable::ReadDiscPath(portable).empty());
    Write(portable / "config/disc-path.txt", "first.iso\nsecond.iso\n");
    assert(app::portable::ReadDiscPath(portable).empty());
    Write(portable / "config/disc-path.txt", std::string("a\0b", 3));
    assert(app::portable::ReadDiscPath(portable).empty());

    std::vector<uint8_t> xbe;
    std::unique_ptr<kernel::Disc> disc;
    std::string error;
    constexpr auto abc_sha1 = "a9993e364706816aba3e25717850c26c9cd0d89d";
    assert(app::portable::OpenDisc(disc_path, abc_sha1, xbe, disc, error));
    assert(disc && std::string(xbe.begin(), xbe.end()) == "abc");
    assert(!app::portable::OpenDisc(disc_path, "wrong-hash", xbe, disc, error));
    assert(!disc && xbe.empty() && !error.empty());
    const auto wrong = root / "wrong.iso";
    SyntheticDisc(wrong, "missing.xbe");
    assert(!app::portable::OpenDisc(wrong, abc_sha1, xbe, disc, error));
    Write(wrong, "not an Xbox disc");
    assert(!app::portable::OpenDisc(wrong, abc_sha1, xbe, disc, error));
    assert(!app::portable::OpenDisc(external, abc_sha1, xbe, disc, error));

    // Exercise the actual shell-link writer without touching the user's real desktop.
    const auto exe = portable / "mc3.exe", shortcut = root / "test.lnk";
    assert(app::portable::WriteShortcut(exe, shortcut));
    assert(SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)));
    IShellLinkW* link = nullptr;
    assert(SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link))));
    IPersistFile* saved = nullptr;
    assert(SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(&saved))));
    assert(SUCCEEDED(saved->Load(shortcut.c_str(), STGM_READ)));
    wchar_t value[32768];
    assert(SUCCEEDED(link->GetPath(value, DWORD(std::size(value)), nullptr, SLGP_RAWPATH)) && fs::path(value) == exe);
    assert(SUCCEEDED(link->GetWorkingDirectory(value, DWORD(std::size(value)))) && fs::path(value) == portable);
    assert(SUCCEEDED(link->GetArguments(value, DWORD(std::size(value)))) && !value[0]);
    saved->Release();
    link->Release();
    CoUninitialize();

    // Only known files and empty directories created by this test are removed.
    assert(fs::equivalent(root.parent_path(), temp));
    for (const auto& path : {shortcut, wrong, disc_path, portable / "not-a-disc.txt",
                            portable / "config/disc-path.txt", relocated / "config/disc-path.txt"}) assert(fs::remove(path));
    for (const auto& path : {portable / "directory.iso", portable / "config", relocated / "config", portable, relocated, root}) assert(fs::remove(path));
}
