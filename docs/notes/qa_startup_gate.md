# Opt-in QA startup gate — 2026-10-26

The gate lets an owned `mc3.exe` become visible to the existing game-pause scheduler **before game
initialization**. It does not stop/restart a movie, change coordinator flags, create a window, initialize input
or open a GPU device. With neither environment variable present, it is inert.

`wWinMain` invokes it immediately after log initialization and before `app::Load`, the game init callback,
disc discovery/shortcut handling, splash/handover, window creation, input initialization and kernel boot.
Waiting, release and refusal/timeout logs are flushed before blocking, continuing or returning respectively.
Refusal returns2 normally. Existing entrypoint timer/DPI/crash-handler setup and private logging precede it.

Both variables must be configured together:

- `XBE_QA_STARTUP_RELEASE_FILE`: a fresh **absolute** release-file path, absent when the gate first probes it.
- `XBE_QA_STARTUP_TOKEN`: exactly32 ASCII hex characters. File content must match its exact case-sensitive
  bytes, with no newline, BOM, spaces or other payload. Explicitly empty variables are invalid, not defaults.

The gate uses a120-second steady-clock deadline, checks before and after each probe, and sleeps at most250ms
between probes. A delayed probe cannot grant release at/after the deadline. Configuration, clock, filesystem
or sleep errors fail closed. A pre-existing path is rejected even if it already contains the correct token.
The Windows reader opens the final entry with `OPEN_REPARSE_POINT`, rejects directory/reparse/non-disk entries
and size other than32, and shares only for readers while checking the token. It never writes, deletes,
truncates or replaces the release or any scheduler/input marker. The file remains evidence after success,
failure or timeout. A complete valid read proves observed bytes/type; it cannot prove the publisher used a
rename rather than a closed direct write. The controlled QA publisher must enforce atomic publication.

## Later controlled handoff

1. Use a new isolated build **containing this gate**, with its EXE/source/settings receipt. r9/r10 do not
   implement it; setting these variables on those old executables cannot hold their initialization.
2. Root prepares a unique owned local directory and missing release path plus a fresh32hex token. Launch
   only the owned gate-capable private `mc3.exe` with both variables. Do not use shared stop flags.
3. Observe its flushed `qa startup gate: waiting` log. The coordinator can now recognize the actual MC3
   process and pause its existing video child at an inference boundary. A requested pause alone is not proof:
   root checks actual child pause logging/state and absence of NCNN inference, retaining the same video PID.
4. Only after actual GPU quiescence and appropriate human-session availability, write the exact token to a
   new temporary file, close it, then atomically move it to the absent release path **without replacement**
   (for example Windows `MoveFileW`, which refuses an existing destination). If foreign content appears,
   preserve it; never use a replacing rename or clear it to force launch. The successful gate release log
   precedes all normal game initialization. Root then owns normal native UI QA.
5. If readiness is not established, do not publish. After120 seconds the not-yet-interactive process exits2;
   the existing coordinator can remove only its own pause marker and resume the same movie worker/progress.
   A successful game close uses that same scheduler path. Keep all release/log/receipt evidence.

The user's native-QA authorization already covers this handoff; no additional permission prompt is required.
Scheduling/quiescence and human-session availability remain necessary. Root subsequently validated the
controlled r11 protocol, three HD-enabled starts and same-PID movie/encoder retention and resume; see
[native evidence](native_photo_r11_20261026.md). The pre-release source/log ordering is persisted. A direct
historical HWND inventory was observed during root control but was not separately persisted as a receipt.

## CPU coverage

`qa_startup_gate` is registered in CTest. Eight filesystem/injected-clock groups pass in
`.local/qa-startup-gate-cpu-20261026/build`, using the project's Clang GNU-style driver and MSVC/UCRT. They cover
inert defaults with no clock/filesystem callbacks, incomplete/empty/bad configuration, Unicode release paths,
new atomic release, matching/foreign/BOM/newline/empty pre-existing and new payloads, preserved directory and
dangling symlink entries, read/sleep errors, clock reversal, timeout, last-boundary publication, release just
before deadline and delayed probes ending at/beyond deadline. The shared main/helper seam also rejects a
260-character relative path before long-path normalization can make it absolute. These tests create no game,
window or GPU device.

Photo-capture hooks and frozen r10 build/staging evidence remain unchanged. The later r11 build/overlay
and actual native receipts are documented in the linked follow-up. Canonical r9, public release assets
and original textures were unchanged by the gate implementation and its private validation.
