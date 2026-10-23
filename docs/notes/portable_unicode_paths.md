# Portable Unicode and long Windows paths — 2026-10-23

The portable startup originally narrowed its executable directory before logging and before kernel Boot. On a
Windows machine whose active ANSI code page cannot represent a username or extracted folder, `.string()` could
throw before first-launch setup or substitute a broken path. `ExeDir` also silently truncated at MAX_PATH.

## Implemented

- `core/host_path.h/.cpp` provides explicit UTF-8 formatting for path text, native wide file opening on Windows,
  extended paths for long names, and a growing executable-location buffer bounded by Windows' Unicode path limit.
  The header intentionally does not include windows.h, avoiding Win32 macro changes in guest kernel sources.
- `ExeDir` and relaunch use the full executable location. Logs receive `std::filesystem::path`, open through the
  wide CRT API, and retain a native path. Reinitializing logging closes its previous file.
- Boot, default device mounts, launch/display persistence, and guest-file resolution retain native filesystem paths.
  Host save reads/writes use wide file opens. Guest names, ordinals and game behavior are unchanged.
- Settings use toml++'s Windows wide-path overload. Game discovery, splash, HD texture/movie, input-file and capture
  diagnostics format host paths explicitly as UTF-8, so logging cannot accidentally narrow a Unicode location.
- Long paths normalize separators before adding `\\?\` / `\\?\UNC\`. Extended Windows paths require backslashes;
  portable helpers that join slash-delimited relative constants must normalize those resulting paths too.

## Verification

`host_path_test` creates a uniquely owned temporary tree containing CJK, Cyrillic and emoji characters, extends its
path beyond 400 characters, then exercises real file writes/reads, real settings save/load and real logging. It
checks UTF-8 round trips and grows an executable-path reader through multiple truncated buffers without launching
or moving a game executable. Cleanup removes only the exact files and empty directories the fixture created.

The fixture passed without a UTF-8 process manifest or a LongPathsEnabled registry prerequisite. Syntax checks
also passed for main, game discovery, settings, system info, splash, logging, input, boot, guest files, Xbox kernel
exports, disc I/O, texture packs, HD video and D3D12. Parent integration must include the new host_path.cpp in the
runtime source glob and run the native link/regression suite. A complete portable executable launch from a Unicode
folder remains a separate parent validation; these component tests do not claim that game-window observation.
