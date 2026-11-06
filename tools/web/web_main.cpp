// Browser port entry point (docs/notes/browser_port_plan.md): <disc image> [--instance N] [--env KEY=VALUE]...
// Replaces harness/main_test.cpp in the WebAssembly build. The player supplies their own original disc image; this
// opens it with the runtime's disc reader, checks it is the USA release this port was recompiled from, mounts it as
// D:\ and boots default.xbe. Saves, logs and captures go to the current folder (hdd, logs, cap).
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "app/app.h"
#include "core/log.h"
#include "image_symbols.h"
#include "kernel/crypto.h"
#include "kernel/disc.h"
#include "kernel/kernel.h"

#if defined(__EMSCRIPTEN__) && defined(MC3_WEB_PAGE)
#include <emscripten/wasmfs.h>
#include <unistd.h>
#endif

extern const char kImageSha1[];  // Game_xbe.cpp

namespace {
std::string Sha1Hex(const std::vector<uint8_t>& data) {
    alignas(8) uint8_t context[128], digest[20];
    crypto::ShaInit(context);
    crypto::ShaUpdate(context, data.data(), data.size());
    crypto::ShaFinal(context, digest);
    std::string hex;
    for (uint8_t b : digest) {
        hex += "0123456789abcdef"[b >> 4];
        hex += "0123456789abcdef"[b & 15];
    }
    return hex;
}
}  // namespace

int main(int argc, char** argv) {
#if defined(__EMSCRIPTEN__) && defined(MC3_WEB_PAGE)
    // In a web page (tools/web/play.html) the game runs on a worker thread (PROXY_TO_PTHREAD) over WASMFS. The
    // browser's private storage (OPFS) is mounted at /opfs and becomes the working folder: the page copied the
    // player's disc there once (/opfs/disc.iso), and logs and saves (hdd) persist there between visits.
    if (backend_t opfs = wasmfs_create_opfs_backend()) {
        wasmfs_create_directory("/opfs", 0777, opfs);
        chdir("/opfs");
    }
#endif
    std::string disc_path;
    for (int i = 1; i < argc; i++) {
        if (!std::strcmp(argv[i], "--instance") && i + 1 < argc) setenv("INSTANCE", argv[++i], 1);
        else if (!std::strcmp(argv[i], "--env") && i + 1 < argc) {
            // a WebAssembly program does not see the host's environment: harness settings (REF, CAP...) come here
            std::string kv = argv[++i];
            size_t eq = kv.find('=');
            if (eq != std::string::npos) setenv(kv.substr(0, eq).c_str(), kv.substr(eq + 1).c_str(), 1);
        } else disc_path = argv[i];
    }
    if (disc_path.empty()) {
        std::fprintf(stderr, "usage: mc3 <original disc image> [--instance N]\n");
        return 2;
    }
    logx::Init(app::ExeDir() / "logs", app::kGame.log);
    app::Load();

    auto disc = std::make_unique<kernel::Disc>();
    std::string error;
    std::vector<uint8_t> xbe;
    if (!disc->Open(disc_path, error)) {
        LOGE("web: %s", error.c_str());
        logx::Flush();
        return 1;
    }
    const auto* entry = disc->Find("default.xbe");
    if (!entry || entry->dir || entry->size > (64u << 20) || !disc->Load("default.xbe", xbe)) {
        LOGE("web: the image has no readable default.xbe");
        logx::Flush();
        return 1;
    }
    if (Sha1Hex(xbe) != kImageSha1) {
        LOGE("web: this image is a different game or release (need the original Xbox USA Midnight Club 3: DUB Edition Remix)");
        logx::Flush();
        return 1;
    }
    LOGI("web: %s from %s", IMAGE_XBE, disc_path.c_str());
    kernel::MountDisc(std::move(disc));

    if (app::kGame.init) app::kGame.init();
    if (app::kGame.window) app::kGame.window(nullptr);
    std::string hdd = app::Instance() > 1 ? "hdd" + std::to_string(app::Instance()) : "hdd";
    std::filesystem::path game_dir = std::filesystem::path(disc_path).parent_path();
    if (!kernel::Boot(std::move(xbe), IMAGE_XBE, game_dir, app::ExeDir() / hdd)) return 1;
    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        logx::Flush();
    }
}
