# Download, extract, launch

Download the Windows x64 portable ZIP from [GitHub Releases](https://github.com/christiand0797/MidnightClub3-Port/releases). Use the named `MidnightClub3-Native-…-win64.zip` asset. GitHub's automatic “Source code” ZIP is for developers.

1. Extract the **entire** ZIP into a writable folder, such as `Games\MidnightClub3`.
2. Double-click `mc3.exe`.
3. For a complete-content release, setup downloads and verifies the matching game, texture and movie content automatically. For a small own-disc release, select your original Xbox **Midnight Club 3: DUB Edition Remix (USA)** ISO when the file picker opens.

The same executable completes setup and starts the game. It remembers the selected disc, creates a **Midnight Club 3 Remix (Native)** desktop shortcut, and creates save/settings folders automatically. Administrator rights, Python, a compiler, an emulator and a separate installer are unnecessary.

Keep the supplied DLLs next to `mc3.exe`. Run from the extracted folder, rather than inside the ZIP. Use Windows 10/11 x64 with a Direct3D 12 capable GPU. Windows N editions need Microsoft's [Media Feature Pack](https://support.microsoft.com/en-us/windows/experience/platform-variants/media-feature-pack-for-windows-n) for Media Foundation movie playback.

## Complete-content setup

A full-content portable ZIP includes `content-source.txt` and `content-manifest.tsv`. They pin a specific release and SHA-256 checksum. Setup downloads its `.mc3c` parts over HTTPS, checks every part, streams files into `game`, `textures/replace` and `video_hd`, checks every installed file, and starts the native game. It does not replace saves or personal settings.

Allow approximately **twice the content's installed size** in free disk space during initial setup. The current original ISO, active textures and movies need roughly 14 GB installed; 30 GB free allows room for temporary chunks. Setup removes its downloaded chunk cache after successful installation. Download size and exact checksums are listed in the release assets.

Canceling or losing the connection preserves partial downloads. Run `mc3.exe` again to resume. If setup reports a checksum failure, run it again to retry that download. An incomplete replacement never takes the place of a working file.

For offline installation, download **all `.mc3c` parts from the same content release** and place them beside `mc3.exe` before starting it. Setup recognizes them locally. The `.mc3c` files are native streaming content parts, not 7-Zip split archives. After a successful installation, manually downloaded parts can be removed.

Later launches verify installed content before starting. This can take time on slower drives. Missing or damaged media is repaired from local parts or downloaded again; keep an internet connection available if the original parts are no longer present.

## Bring your own ISO

Place the matching `.iso` beside `mc3.exe` to skip the picker, or select it once from any drive. The executable reads the ISO directly without extracting or modifying it. The supported disc's `default.xbe` must have SHA-1:

`5bca1cf092edaadf876e8f80129900407a4a07b8`

The saved location is `config/disc-path.txt`. A disc inside the portable folder uses a relative path so the whole folder can move to another PC. An external disc retains its absolute path. If that drive is missing, select its new location. To change the disc deliberately, run `mc3.exe --choose-disc`; `mc3.exe --disc "D:\Games\MC3.xiso.iso"` selects a strict command-line override.

## Controls and settings

Press **F1**, or controller **Back + Y**, for the PC graphics and tuning menu. The game pauses controller input while that menu is open. Press Enter for Start, Space for A/confirm, Escape for B/back, and arrow keys for the directional pad.

Settings are kept beside the executable in `settings.toml` and `config/mc3_enhancements.toml`. Save data is under `hdd`. Session reports are under `logs`; crash reports are under `CrashDumps`.

## Updating or moving to a laptop

Close the game before updating. Extract the newer portable ZIP into the existing folder, replacing the shipped executable/DLL/documentation files. Keep `hdd`, `settings.toml` and `config`. Runtime-only updates can reference the same pinned content release and reuse the installed content without another download.

To move an existing installation, copy the extracted folder with its `game`, `textures`, `video_hd`, `hdd` and `config` folders. Launch `mc3.exe` once at the new location to update the desktop shortcut. If the ISO lives elsewhere, select its new path. A fresh download creates a fresh save folder; it does not retrieve saves from another computer automatically.

This project is in development. Each release's notes describe its actual checks and known limitations; successful setup does not establish that every car, race or graphics mode has been playtested.

## Player settings and troubleshooting

Read the [complete player guide](PLAYER_GUIDE.md) for F1 / F2 / controller menu
access, every Display, Vehicle, Input, Quality, Picture and Info option, units,
defaults, Apply / Keep / Revert, save locations and troubleshooting. Vehicle
controls and all-car ground contact are still being validated; the guide marks
current limitations instead of claiming every available slider is playtested.
