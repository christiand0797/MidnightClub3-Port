# Midnight Club 3: DUB Edition Remix — native Windows port

Play the native Windows x64 port with Direct3D 12 and XAudio2. The original x86
game is translated to C++ ahead of time; no emulator is needed.

## Download and play

Use the **complete-content Windows portable ZIP** on the
[v0.3.2 development release page](https://github.com/christiand0797/MidnightClub3-Port/releases/tag/v0.3.2-dev.20261025).
Read its release notes for the available assets, tested features and known
limitations. Choose `MidnightClub3-Native-…-win64.zip`.

1. Extract the entire ZIP into a writable folder.
2. Double-click **mc3.exe**. First launch downloads and verifies the matching
   content, creates a desktop shortcut and starts the game. Allow about 30 GB
   free for the current content and setup's temporary files.
3. Press **F1** or controller **Back + Y** for PC settings; **F2** opens Vehicle
   tuning directly.

[Read the complete player guide](docs/PLAYER_GUIDE.md) for every Display, Vehicle,
Input, Quality, Picture and Info option, units, defaults, saving and
troubleshooting. [Installation instructions](docs/INSTALL.md) cover offline
content, own-disc builds, updates and moving the game to another PC.

The current native build passed a **Golf R32 garage-to-street check**: ride-height
changes applied immediately, and ride, camber, wheel offset and engine-power
values survived leaving the garage. The latest hotfix also passed native **'57 Bel Air** checks of an added
tire profile on both axles and the private street chassis floor. **21 regression tests pass**, including original
refresh, mesh-reader and floor-writer instructions. See the [hotfix validation](docs/notes/native_hotfix_20261023.md)
and [earlier tuning record](docs/notes/live_vehicle_tuning.md) for the tested values and remaining limits.
The ±2 m ride range is implemented; physical fit at every extreme on every car remains unverified.

A clean offline first launch also installed all **7,188 content files**, created
the desktop shortcut and automatically started the HD intro. See the
[release validation record](docs/notes/release_validation.md). Windows may show
its own network-permission prompt, which the user handles.

GitHub's automatic **Source code** ZIP is for development. Use the named portable
release asset to play. The additional media pass and all-car tuning validation
are ongoing. [PBR materials and geometry foundations](docs/notes/pbr_system.md)
are implemented in development; rendering integration remains unfinished.

## Screenshots

Native captures (Xbox Game Bar, 1920×1080) from the current development builds.

| | |
| --- | --- |
| ![Lexus GS with neon underglow](docs/screenshots/2026-10-28-lexus-gs-neon-underglow.jpg)<br>Lexus GS with neon underglow | ![Tokyo cruise](docs/screenshots/2026-10-31-tokyo-cruise-jetta.jpg)<br>Tokyo cruise |
| ![Impala on hydraulics](docs/screenshots/2026-10-29-impala-hydraulics-neon.jpg)<br>Impala on hydraulics | ![Headlights at night](docs/screenshots/2026-10-24-lexus-headlights-night.jpg)<br>Headlights at night |
| ![Golf in Tokyo](docs/screenshots/2026-10-21-tokyo-golf-street.jpg)<br>Golf in Tokyo | !['57 Bel Air in a parking garage](docs/screenshots/2026-10-27-bel-air-parking-garage.jpg)<br>'57 Bel Air in a parking garage |
| ![DUB Shop tire width](docs/screenshots/2026-10-23-dub-shop-tire-width.jpg)<br>DUB Shop tire width | ![DUB Shop vehicle menu](docs/screenshots/2026-10-24-dub-shop-golf.jpg)<br>DUB Shop vehicle menu |
| ![Bel Air on oversized wheels](docs/screenshots/2026-10-24-bel-air-oversized-wheels.jpg)<br>Bel Air on oversized wheels | ![PBR material settings (F1)](docs/screenshots/2026-10-29-pbr-material-settings.jpg)<br>PBR material settings (F1) |

## Development

This source repository includes the compatibility runtime, build and media tools,
regression checks and reverse-engineering notes. Start with
[the port instructions](PORTABLE_README.md), [the current handoff](docs/HANDOFF.md)
and [the continuation ledger](docs/AUTONOMOUS_RUN.md). Build with `Build-MC3.ps1`
and launch locally with `Launch-MC3.ps1`; the development executable is
`templates/game/out/native/mc3.exe`.

The tested local development build adds **F1 → World** traffic/pedestrian targets,
**F1 → Career** one-shot money editing and **F1 → Vehicle** per-owned-car setups
with global fallback. These additions are not in the older downloadable release.
See [the development controls guide](docs/DEVELOPMENT_CONTROLS.md) for saving and
[native validation](docs/notes/feature_native_20261028.md) for tested behavior.

The Git source tree excludes original game data, generated code, model weights
and build outputs. Complete private project snapshots are published separately.

The upstream framework documentation follows, with its original attribution and license retained.

# Xbox XBE Recomp

A tool for creating unofficial native PC ports of original Xbox games. Games are statically recompiled from their x86 XBE into native 64 bit Windows executables. The game's x86 code is translated to C++ ahead of time. The Xbox kernel and the XDK libraries (Direct3D 8, DirectSound, XAPI, XNet) are implemented at the API level on Windows. Everything is drawn with a native Direct3D 12 renderer and played through XAudio2.

This tool was originally created for porting [Tom Clancy's Splinter Cell: Double Agent](https://github.com/TGP482/SCDoubleAgent-XBERecomp) and was later used to port:
- [BLACK](https://github.com/TGP482/BLACK-XBERecomp)
- [007: From Russia With Love](https://github.com/TGP482/007FRWL-XBERecomp)
- [007: Nightfire](https://github.com/TGP482/007Nightfire-XBERecomp)

> [!NOTE]
> This project does not include any game assets. You must provide the files from your own legally acquired copy of the game to install or build.

## Installation

- Put this repository in your game repository as `sdk/`. This can be a copy or a submodule.
- In the game's `CMakeLists.txt`:
   ```cmake
   include(sdk/cmake/XbeRecomp.cmake)
   file(GLOB XBE_HOST_SOURCES CONFIGURE_DEPENDS src/*/*.cpp)
   list(FILTER XBE_HOST_SOURCES EXCLUDE REGEX "/src/game/")
   add_xbe_image(MyGame default.xbe game.toml 128 game)
   ```
- Build with Clang and Ninja (`clang++`, `-fuse-ld=lld`). Zydis, toml++, Dear ImGui and SDL3 are fetched by CMake.

## Game Setup

1. Copy `templates/game` to a new repository and put this SDK in it as `sdk/`. Set `GAME_TITLE`, `GAME_XBE` and `GAME_EXE` at the top of `build.bat` (`GAME_EXE` in `release.bat` too), and the target and xbe in `add_xbe_image` in `CMakeLists.txt`.
2. Put the game's XBE and its data folders in the `assets` folder. No game files, keys or generated code belong in any repository.
3. Run `xbe_symbols`, which `build.bat` does on the first build:
   ```
   xbe_symbols assets\default.xbe sdk\xdk --library MyGame.dll --out config\game.toml
   ```
   This prints the XBE's library versions, picks `xdk/<build>/signatures.toml` and finds the XDK functions and data the host replaces, then writes the config: `[xbe]` with a partition count and the code sections, the `layout.toml` include, `[symbols]` and `[data]`. Misses and ambiguous names are listed and left at 0, which become trapping stubs. It stops when no `signatures.toml` exists for the XBE's build.
4. Fill in `app::kGame` in `src/common/game_info.cpp`: title, window name, log and settings file names, splash, the xbe to exe table, game options, and the hooks for loading, the window, drawing and the pads.
5. Game hooks go in `src/game`. Define `X86_FUNC(sub_XXXXXXXX)` to replace a recompiled function, and call `__imp__sub_XXXXXXXX` to reach the original. `python sdk/tools/symbols/xbe2pe.py config/game.toml game.exe` writes a flat PE with the recompiled functions as exports for Ghidra.
6. Run `build.bat`. One game repo can build several images, one config and `add_xbe_image` each; `--name` sets the config's `generated/<name>`, which is the folder argument of `add_xbe_image`.

A new XDK build needs `xdk/<build>/layout.toml` and signatures made from titles whose configs are complete: `xbe_symbols --sign config/a.toml [config/b.toml] --out sdk/xdk/<build>/signatures.toml`. Signatures are lengths and hashes of the functions with addresses masked, never library code.

## Features

### Recompiler
- **Function Discovery** - Finds every function in the XBE by following direct calls, code pointers and the gaps between known functions.
- **Jump Tables** - Resolves switch tables of every form the Xbox compiler writes, including byte indexed and backwards tables, memcpy tables and copy sleds.
- **C++ Exceptions** - Supports C++ exceptions, with try and catch blocks working as on the Xbox.
- **x87, SSE and MMX** - Translates x87, SSE and MMX instructions, with the x87 register stack tracked at run time.
- **Overridable Functions** - Makes every recompiled function replaceable, allowing a game to override any of them without editing generated code.
- **Instruction Hooks** - Adds `[[hook]]`, which calls a game function before or after any instruction, and can return from or jump within the function.
- **Analysis Overrides** - Adds `[[function]]`, `[[switch]]` and `[xbe] data_ranges` to add functions, set switch targets and mark data the analysis gets wrong.
- **Incremental Builds** - Only rewrites generated files when their content changes, keeping rebuilds fast.
- **Game Code on the Player's PC** - Builds the recompiled code into `<exe>.dll` on the first start of a release, from the player's own XBE, with the bundled recompiler, Clang and LLD behind a progress window. A release ships no game code.
- **XDK Signatures** - Finds the XDK functions and data in a new game automatically and writes its config.
- **LTCG Libraries** - Supports games built with link time code generation, where library functions take their arguments in registers.

### CPU and Kernel
- **Guest Calls** - Adds indirect calls and host calls into game code, with the Xbox's 733 MHz timer and Pentium III CPUID.
- **x87 Precision** - Follows the game's x87 precision setting, giving the same results as on the Xbox.
- **CRT Math** - Computes the game's math functions on the host, fixing math that returned invalid results at random.
- **Kernel** - Implements threads, waits, files, memory, crypto and EEPROM settings, with game files read from host folders or a disc image.
- **Image Switching** - Relaunches into the matching executable when the game loads another XBE, keeping the window on screen without a gap.

### Graphics
- **Direct3D 8** - Implements Direct3D 8 as the XDK does it, including immediate mode draws, palettes, gamma ramps and Direct3D code built into the game.
- **Native Direct3D 12 Renderer** - Translates the Xbox's vertex programs, pixel combiners and fixed function pipeline to HLSL, with rendering on its own thread.
- **Renderer Extensions** - Allows a game to change render targets, add passes and add to the generated shaders through `gpu::Extension`, set with `gpu::SetExtension` from `GameInfo::init`.

### Audio
- **DirectSound** - Mixes sound in software through XAudio2, with Xbox ADPCM, streams, mix bins and the effects image.

### Input
- **Controller** - Supports Xbox, PlayStation and Switch controllers through SDL3, with hot plugging and rumble.

### General
- **Disc Images** - Runs the game from an ISO in the `assets` folder, either a whole disc dump or the game partition, or from the extracted data files.
- **System Link** - System Link can be played over LAN, or by directly connecting to a friend's IP address.
- **Bink Movies** - Fixes Bink movies playing too fast above 30 FPS and crashing on fast reads, for games that use Bink.
- **Crash Dumps** - Writes crash dumps to a `CrashDumps` folder beside the exe.

### Miscellaneous
- **Boot Splash** - Place a `splash.png` (or `.bmp`) beside the exe to display a splash screen at launch.
- **Diagnostics** - Logs frame timing with `XBE_PERF`, captures a frame with F3 when `XBE_CAPTURE_DIR` is set and enables the Direct3D 12 debug layer with `XBE_D3D12_DEBUG`.
- **Headless Harness** - Builds and runs a game on Linux without a window, GPU, audio or controller, for testing.
- **SDK Sync** - Keeps the `sdk/` copies in game repositories in step with this repository.

## Credits

- [XenonRecomp](https://github.com/hedge-dev/XenonRecomp) for the overridable function layout and instruction hooks this follows.
- [Zydis](https://github.com/zyantific/zydis) for decoding x86 instructions, [toml++](https://github.com/marzer/tomlplusplus) for the configs, [Dear ImGui](https://github.com/ocornut/imgui) for the settings overlay and [SDL](https://github.com/libsdl-org/SDL) for the window, input and controllers.
- [xemu](https://github.com/xemu-project/xemu) and [Cxbx-Reloaded](https://github.com/Cxbx-Reloaded/Cxbx-Reloaded) as references for the Xbox's hardware and API behaviour. This project does not use emulation or any emulator code.
