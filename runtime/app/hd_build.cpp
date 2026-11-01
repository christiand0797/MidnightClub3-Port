// "Make HD" (tools/enhance/build_hd.py, shipped by the release in enhance\ with its own Python): HD textures, PBR
// materials and HD movies built on this PC from the player's own disc. Until it has finished, each start offers to
// run it in its own window; the PBR pack it builds is adopted by settings.cpp (AdoptHdPack).
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "app/app.h"
#include "core/log.h"

namespace app {

void OfferHdBuild() {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path dir = ExeDir(), enhance = dir / "enhance", script = dir / "Make HD.cmd";
    if (Instance() > 0 || !fs::exists(enhance / "python" / "python.exe", ec) || !fs::exists(script, ec)) return;
    if (fs::exists(enhance / "done.txt", ec)) return;
    if (HANDLE running = OpenMutexW(SYNCHRONIZE, FALSE, L"Local\\MC3MakeHD")) {  // the builder holds it
        CloseHandle(running);
        return;
    }
    const fs::path declined = dir / "config" / "hd-build-declined.txt";
    if (fs::exists(declined, ec)) return;
    const int answer = MessageBoxW(nullptr,
        L"Make the game HD?\n\n"
        L"This makes HD textures, realistic materials and HD movies from your own game disc, so the game looks "
        L"like the pictures on the GitHub page.\n\n"
        L"It takes a few hours and works in its own window. It pauses while you play and carries on by itself "
        L"when you close the game, so leaving it on overnight is best. New textures show up the next time you "
        L"start the game.\n\n"
        L"Yes = start now\n"
        L"No = not now (ask me next time)\n"
        L"Cancel = don't ask again (you can double-click \"Make HD\" in the game folder any time)",
        kGame.short_title, MB_YESNOCANCEL | MB_ICONQUESTION | MB_SETFOREGROUND);
    if (answer == IDCANCEL) {
        fs::create_directories(declined.parent_path(), ec);
        std::ofstream(declined) << "The game no longer asks to run Make HD. Delete this file to be asked again.\n";
        return;
    }
    if (answer != IDYES) return;
    const auto result = reinterpret_cast<INT_PTR>(
        ShellExecuteW(nullptr, L"open", script.c_str(), nullptr, dir.c_str(), SW_SHOWNORMAL));
    LOGR("Make HD: %s", result > 32 ? "started" : "could not start");
}

}  // namespace app
