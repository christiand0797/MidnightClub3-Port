// The game in assets: its files, or a disc image (.iso) with this executable's xbe on it. The xbe must be the one
// this executable was recompiled from; another release's would run its own data under this code.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <cwctype>
#include <fstream>

#include "app/app.h"
#include "app/portable.h"
#include "app/portable_content.h"
#include "core/log.h"
#include "core/host_path.h"
#include "image_symbols.h"
#include "kernel/crypto.h"
#include "kernel/disc.h"

extern const char kImageSha1[];  // the xbe recompiled into this executable, written by CMake

namespace app {

namespace {

namespace fs = std::filesystem;

// assets beside the exe or in any folder above it (the build output)
fs::path FindAssets() {
    auto dir = ExeDir();
    std::error_code ec;
    for (int i = 0; i < 6; i++) {
        if (fs::is_directory(dir / "assets", ec)) return dir / "assets";
        if (!dir.has_parent_path() || dir.parent_path() == dir) break;
        dir = dir.parent_path();
    }
    return {};
}

std::string Sha1(const std::vector<uint8_t>& data) {
    alignas(8) uint8_t ctx[128];
    uint8_t digest[20];
    crypto::ShaInit(ctx);
    crypto::ShaUpdate(ctx, data.data(), data.size());
    crypto::ShaFinal(ctx, digest);
    std::string hex;
    for (uint8_t b : digest) {
        hex += "0123456789abcdef"[b >> 4];
        hex += "0123456789abcdef"[b & 15];
    }
    return hex;
}

bool ReadHostFile(const fs::path& path, std::vector<uint8_t>& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::error_code ec;
    if (std::filesystem::file_size(path, ec) > (64u << 20) || ec) return false;
    out.assign(std::istreambuf_iterator<char>(f), {});
    return true;
}

}  // namespace

bool FindGame(GameFiles& game) {
    // Explicit command-line paths are strict; ordinary double-clicks discover an image or show the picker.
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    fs::path requested_disc;
    bool explicit_disc = false, choose_disc = false;
    if (argv) {
        for (int i = 1; i < argc; i++) {
            if (std::wstring_view(argv[i]) == L"--choose-disc") choose_disc = true;
            if (std::wstring_view(argv[i]) != L"--disc") continue;
            explicit_disc = true;
            if (i + 1 < argc) requested_disc = argv[++i];
            break;
        }
        LocalFree(argv);
    }
    const fs::path exe_dir = ExeDir();
    auto mount = [&](const fs::path& path, std::string& error) {
        std::unique_ptr<kernel::Disc> disc;
        std::vector<uint8_t> xbe;
        if (!portable::OpenDisc(path, kImageSha1, xbe, disc, error)) return false;
        LOGR("Game: %s in %s, sha1 %s", IMAGE_XBE, hostpath::Utf8(path).c_str(), kImageSha1);
        game = {path.parent_path(), std::move(xbe)};
        kernel::MountDisc(std::move(disc));
        if (!portable::RememberDiscPath(exe_dir, path)) LOGW("setup: disc location could not be saved beside the executable");
        portable::EnsureDesktopShortcut();
        return true;
    };
    auto show_error = [&](const std::string& error) {
        std::wstring message(error.begin(), error.end());
        MessageBoxW(nullptr, message.c_str(), kGame.short_title, MB_ICONERROR);
    };
    if (explicit_disc) {
        std::string error;
        if (requested_disc.empty()) error = "--disc requires the path to an Xbox disc image.";
        else if (mount(requested_disc, error)) return true;
        LOGE("game: --disc: %s", error.c_str());
        show_error(error);
        return false;
    }

    if (!choose_disc) {
        std::string setup_error;
        if (!portable::InstallBundledContent(exe_dir, setup_error)) { show_error(setup_error); return false; }
        // A portable ISO beside mc3.exe takes precedence, then a previously selected external disc.
        auto candidates = portable::DiscCandidates(exe_dir);
        auto bundled = portable::DiscCandidates(exe_dir / "game");
        candidates.insert(candidates.end(), bundled.begin(), bundled.end());
        const fs::path remembered = portable::ReadDiscPath(exe_dir);
        if (!remembered.empty()) candidates.push_back(remembered);
        for (const auto& path : candidates) {
            std::string error;
            if (mount(path, error)) return true;
            LOGW("game: candidate disc skipped: %s", error.c_str());
        }

        // Developer builds and manually extracted disc folders retain their existing assets path.
        const fs::path assets = FindAssets();
        std::vector<uint8_t> xbe;
        if (!assets.empty() && ReadHostFile(assets / IMAGE_XBE, xbe)) {
            if (Sha1(xbe) == kImageSha1) {
                LOGR("Game: %s in %s, sha1 %s", IMAGE_XBE, hostpath::Utf8(assets).c_str(), kImageSha1);
                game = {assets, std::move(xbe)};
                kernel::MountDisc(nullptr);
                portable::EnsureDesktopShortcut();
                return true;
            }
            LOGW("game: extracted XBE is a different release; select the matching original disc");
        }
        for (const auto& path : portable::DiscCandidates(assets)) {
            std::string error;
            if (mount(path, error)) return true;
            LOGW("game: candidate disc skipped: %s", error.c_str());
        }
    }

    // No Python, compiler, extraction utility, administrator rights, or separate setup application is needed.
    for (;;) {
        const fs::path selected = portable::PickDisc(exe_dir, L"Select Midnight Club 3: DUB Edition Remix (USA) — original Xbox ISO");
        if (selected.empty()) return false;
        std::string error;
        if (mount(selected, error)) return true;
        LOGE("game: selected disc: %s", error.c_str());
        show_error(error);
    }
}

}  // namespace app
