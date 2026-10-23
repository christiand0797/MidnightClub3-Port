#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "app/app.h"
#include "core/host_path.h"
#include "core/log.h"
#include "kernel/kernel.h"
#include <cassert>
#include <filesystem>
#include <fstream>
#include <type_traits>

namespace fixture { std::filesystem::path folder; }
namespace app {
const GameInfo kGame{.title = L"Unicode portable path component test", .settings = "settings.toml"};
std::filesystem::path ExeDir() { return fixture::folder; }
void* Window() { return nullptr; }
}

int main() {
    namespace fs = std::filesystem;
    static_assert(std::is_same_v<decltype(kernel::g_launch_file), fs::path>);
    const auto temp = fs::absolute(fs::temp_directory_path());
    const auto root = temp / ("mc3_host_path_test_" + std::to_string(GetCurrentProcessId()) + "_" + std::to_string(GetTickCount64()));
    assert(fs::equivalent(root.parent_path(), temp) && fs::create_directory(root));
    std::vector<fs::path> folders;
    auto current = root / fs::path(u8"玩家_Игра_🚗");
    assert(fs::create_directory(current)); folders.push_back(current);
    for (int i = 0; i < 4; i++) {
        current = hostpath::Native(current / std::wstring(72, wchar_t(L'a' + i)));
        assert(fs::create_directory(current)); folders.push_back(current);
    }
    fixture::folder = current;
    assert(current.native().size() > MAX_PATH && current.native().starts_with(L"\\\\?\\"));
    const auto bytes = current / fs::path(u8"保存_данные.bin");
    const char payload[] = "Native Unicode path bytes";
    FILE* output = hostpath::Open(bytes, "wb");
    assert(output && std::fwrite(payload, 1, sizeof(payload), output) == sizeof(payload));
    assert(std::fclose(output) == 0);
    FILE* input = hostpath::Open(bytes, "rb");
    char received[sizeof(payload)];
    assert(input && std::fread(received, 1, sizeof(received), input) == sizeof(received));
    assert(std::fclose(input) == 0 && std::memcmp(received, payload, sizeof(payload)) == 0);
    assert(hostpath::FromUtf8(hostpath::Utf8(bytes)) == bytes);

    // Exercise the actual settings and log implementations under a Unicode path beyond MAX_PATH, with no
    // process UTF-8 code-page manifest or LongPathsEnabled registry prerequisite.
    app::Get().graphics.scene_width = 3840; app::Get().graphics.scene_height = 2160;
    app::Get().tuning.ride_front = -.125f;
    app::Save(); app::Get() = app::Settings{}; app::Load();
    assert(app::Get().graphics.scene_width == 3840 && app::Get().graphics.scene_height == 2160);
    assert(app::Get().tuning.ride_front == -.125f);
    logx::Init(current / "logs", "unicode");
    const fs::path log_file = logx::Path();
    LOGR("portable path: %s", hostpath::Utf8(bytes).c_str());
    logx::Flush();
    std::ifstream log(log_file, std::ios::binary);
    const std::string text(std::istreambuf_iterator<char>(log), {});
    assert(text.find(hostpath::Utf8(bytes)) != std::string::npos);
    log.close();
    // Reinitializing onto a file-as-directory closes the previous log and fails without damaging that file.
    logx::Init(bytes, "not-a-directory");
    assert(fs::file_size(bytes) == sizeof(payload));

    std::wstring module = L"C:\\玩家\\";
    for (int i = 0; i < 8; i++) module += std::wstring(70, L'文') + L"\\";
    module += L"mc3.exe";
    int requests = 0;
    auto found = hostpath::ReadExecutablePath([&](wchar_t* buffer, DWORD capacity) {
        requests++;
        DWORD count = std::min(capacity, DWORD(module.size()));
        std::copy_n(module.data(), count, buffer);
        if (count < capacity) buffer[count] = 0;
        return count;
    });
    assert(requests > 1 && found == hostpath::Native(fs::path(module)));
    assert(fs::is_regular_file(hostpath::ExecutablePath()));
    bool failed = false;
    try { (void)hostpath::ReadExecutablePath([](wchar_t*, DWORD) { return DWORD(0); }); }
    catch (const std::runtime_error&) { failed = true; }
    assert(failed);

    assert(fs::equivalent(root.parent_path(), temp) && root.filename().wstring().starts_with(L"mc3_host_path_test_"));
    assert(fs::remove(log_file)); assert(fs::remove(current / "logs"));
    assert(fs::remove(current / "settings.toml"));
    assert(fs::remove(current / "config" / "mc3_enhancements.toml")); assert(fs::remove(current / "config"));
    assert(fs::remove(bytes));
    for (auto at = folders.rbegin(); at != folders.rend(); ++at) assert(fs::remove(*at));
    assert(fs::remove(root));
}
