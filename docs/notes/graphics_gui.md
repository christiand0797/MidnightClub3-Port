# Native graphics settings — 2026-10-23

Implemented in the isolated `graphics-tuning` checkout while the primary project was frozen for backup.
Native build integration and game-window validation belong to the parent agent; the checks below do not substitute
for those observations.

## User controls

- F1 or controller Back + Y opens the native settings overlay. Mouse, Tab / Shift+Tab, keyboard arrows and
  controller D-pad navigate it; Enter / A activates controls. F2 opens Vehicle directly (garage agent's controls).
- The settings window fits the visible client area at 640x480 through 4K. Its contents scroll independently of
  the Close / Save now footer. Tabs can scroll horizontally on small windows.
- Native GUI text now uses Windows Segoe UI rasterized at 22 px with horizontal/vertical oversampling, rather than
  enlarging ImGui's 13 px default. A 20 px built-in font remains available if the Windows font is missing. Label
  columns reserve more room for the larger typeface. At 1080p the existing scale gives approximately 26 px text;
  at 480p contents scroll while the footer stays on screen.
- Display mode and monitor/window size are separate from **Native scene resolution**. Automatic follows the
  existing output settings. Explicit presets are 640x480, 854x480, 1280x720, 1920x1080, 2560x1440, 3840x2160,
  5120x2880, 7680x4320 and 15360x8640. Custom dimensions accept 320..16384 by 240..16384. Both zeros mean Automatic.
- Scene render scale accepts 25..400%. Both dimensions are limited together to D3D12's 16384 texel limit, retaining
  aspect ratio: 16K at 400% becomes 16384x9216. The UI shows the resulting requested dimensions and the renderer's
  observed front-buffer dimensions separately.
- Changing scene resolution never creates a 16K desktop window or 16K swap chain. A window fits the monitor's work
  area; its requested scene can still be larger. The physical client-sized back buffer contains the final scaled
  scene and the settings UI. Explicit 4:3/16:9 scene shape controls the Xbox anamorphic projection independently of
  monitor shape, with the existing 4:3 or 16:9 presentation aspect.
- Display edits are staged. Apply starts a 20-second Keep / Revert confirmation. Keep becomes available only once
  the renderer reports the requested scene dimensions. Timeout, B / Escape, or F1 closing the menu restores the
  previous display selection. Unconfirmed values are never written by the normal menu save actions.
- The monitor mode is tested before fullscreen changes. An unsupported mode restores the previous selection;
  an unavailable mode from a copied startup settings file falls back to a visible borderless window.
- Quality, Picture, Input and Vehicle changes remain live. Quality presets preserve the selected scene resolution
  and render scale. Selecting a scene preset clears the legacy `force_1080p` override.

## Renderer changes and recovery

The previous scalar scale used only output height. A 1920x1080 request made a 1440x1080 intermediate from the
Xbox's 640x480 frame. `app/resolution.h` now calculates exact independent axes for the main frame and proportional
effect/depth targets. Other shapes, such as square shadow/reflection targets, retain uniform scaling and bounded
dimensions. An extension that explicitly overrides target scale still takes precedence.

Viewport scissors, clear rectangles, drawn-copy destinations and region-copy Y coordinates use the target's own
height/guest-height ratio. Copies between different pixel scales use a drawn resample rather than an invalid direct
region copy. Render-target half-pixel sampling and point readback account for independent X/Y scales using the
previously unused `spread[i].w` component; `DrawConstants::ext` and all C++/HLSL buffer offsets remain unchanged.

One full 15360x8640 colour/depth pair alone consumes about 1013 MiB; feedback copies, effects, textures, transient
uploads and the driver need additional memory. The UI states this baseline estimate. There is no claim that every
GPU can allocate or sustain 16K. A failed target allocation latches for that selection instead of retrying on every
draw. The menu restores the previous selection, or a copied startup configuration falls back to 1280x720 and then
640x480. Renderer target caches rebuild at the next presentation boundary. The small desktop-sized UI remains
available. Swap-buffer resize failure reacquires the existing buffers and reports recovery. Picture-target
allocation failure is also latched and triggers the same recovery instead of retrying each frame.

Controller polling formerly called `ToggleMenu` while holding the input mutex. Display rollback now serializes
menu state under `UiLock`; the toggle was moved outside the input mutex to avoid inversion with UI `RawPad` reads.

## Persistence

`[graphics] scene_width` / `scene_height` are new; 0/0 is backward compatible Automatic. Existing top-level width /
height, render_percent, display_mode and the enhancements file continue loading. Invalid partial scene pairs reset
to Automatic. Dimensions and render percentage are bounded before the renderer reads them. Non-finite graphics
float values retain their previous/default values.

## Checked here

- Clang syntax checks: settings menu, settings persistence, main window handling, D3D12 backend and shader source
  generator. Third-party ImGui/toml++ warnings only.
- `resolution_test`: exact native sizes and half-size effects across the presets; independent Y rectangle mapping;
  signed/out-of-bounds rectangle clamps; uniform square targets; 16K supersampling bounds; copy scale matching;
  oversized window fitting; non-finite/huge extent arithmetic.
- `settings_test`: real settings.cpp save/load of 16K, render-scale bounds, invalid persisted dimension pairs,
  backwards-compatible output dimensions, and scene-driven widescreen changes, alongside existing tuning checks.
- `graphics_menu_test`: actual settings_menu.cpp and ImGui run headlessly at 640x480, 720p and 4K; the window fits
  each screen with the actual larger UI font; the 480p contents are scrollable. Exercises unpersisted display preview, allocation rejection restoring only display fields, automatic
  timeout, F1 close rollback, startup 1280x720 then 640x480 recovery, and saving/loading the working fallback.

Still required after parent native build: mouse/controller interaction in the actual game; 480p / 1080p / 4K actual
front-target captures; unsupported fullscreen handling on the current monitor; bounded high-resolution allocation
and recovery; effect/HUD alignment under the new independent-axis path. These checks must be recorded as observed
before calling the graphics menu fully verified.
