# Midnight Club 3 native PC build

This project statically recompiles the USA Xbox executable into a Windows x64 program using D3D12, XAudio2 and SDL3. It reads your own original disc data. No emulator process or Xbox BIOS is used.

The current executable is `templates/game/out/native/mc3.exe`. The old executable copied to the repository root is stale; use the launcher so the current build, settings and HD media are selected.

## Build and launch

From this folder in PowerShell:

```powershell
.\Build-MC3.ps1 -Jobs 8
.\Launch-MC3.ps1
```

The launcher reads `.local/source-disc.txt`. To select a different original image for this run:

```powershell
.\Launch-MC3.ps1 -DiscImage 'E:\ORIGINAL XBOX GAMES\unzipped\Midnight Club 3 - DUB Edition Remix (USA).xiso.iso'
```

Passing `-DiscImage` to **Build-MC3.ps1** verifies the disc/XBE and remembers the path. `-ConfigureOnly` configures CMake without compiling; it is not a clean build. Close the game before linking a replacement executable.

Build requirements: Windows, Visual Studio C++ Build Tools and Windows SDK, LLVM/Clang, CMake 3.25+, Ninja, Git and Python. The existing build scripts locate the installed toolchain. Dependencies and a first full compilation take longer than an incremental build.

## Controls and settings

Enter is Start, Space is A/select, Escape is B/back, Backspace is Back, arrows navigate menus, W/S are throttle/brake and A/D steer. An SDL-compatible controller also works; Start and A keep their console roles.

F1 or controller Back + Y opens the settings overlay. It includes display/resolution/render scale, 60/30 fps, FXAA, sharpening, picture controls, game blur/glow/headlight strength, texture detail, HD textures/movies and experimental vehicle tuning. Changes save beside the executable in `settings.toml` when the overlay closes. Radial controller deadzones and output overrides are in `config/mc3_enhancements.toml` beside the executable.

The game's original Options/Tuning menus are not extended yet. Ride-height controls currently affect loaded car data including traffic; player-only ownership and garage selection limits still need playtesting/research. FOV, LOD, SSAO and separate bloom/UI scaling controls are not implemented.

## HD media

The active runtime replacement pipeline is:

- `tools/texture_pack/extract_all.py`: verified supported Xbox texture records to hash-named source PNGs.
- `tools/texture_pack/upscale.py`: AI upscales into `templates/game/out/native/textures/replace/<hash>.png`.
- `tools/video/upscale_movies.py`: publishes completed MP4s under `templates/game/out/native/video_hd/`.

The renderer selects replacement textures by their original data/format/palette hash. Original Bink audio and timing remain in use when an HD movie supplies the picture. Files without an HD replacement keep their original picture. Restart the game after adding texture replacements because their directory is indexed once per run.

The supported texture batch completed 4,774 replacements on 2026-10-21. This is not all game textures: P8, linear, `.tex`, `.xbpf` and loose XPR coverage is incomplete. Movie generation is still running. Do not start duplicate GPU jobs.

The root `extract_*`, `repack_xpr.py`, `pipeline.py` and `complete_pipeline.py` are historical experiments. Their repacked assets are not the active runtime path. Do not copy their outputs over the original assets.

## Verification and current work

See `docs/HANDOFF.md` and `docs/REANALYSIS.md` for the current build, tests, remaining rendering/audio/menu work and research notes. Native logs are in `templates/game/out/native/logs/mc3.log`; crash dumps are beside that executable under `CrashDumps/`.

The original disc remains read-only. Game data, generated translated code, downloaded tools and HD output are local artifacts, not source to commit.
