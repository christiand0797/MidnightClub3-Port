# Settings, input, game hook and tuning audit

2026-10-21. Source audit of the working tree after the previous session's partial changes. No game was launched or
driven. Parent owns the final integration build and checks. Line references below describe the audit snapshot;
the parent is removing or fixing several of these files concurrently.

## Scope read

Read all of runtime/app/{app.h,main.cpp,settings.cpp,settings_menu.cpp,game_files.cpp,handover.cpp,splash.cpp,
system_info.cpp}, runtime/core/{input.cpp,input.h,input_mapping.h}, runtime/gpu/mc3_extension.{h,cpp},
templates/game/src/common/game_info.cpp, templates/game/src/game/{hooks.cpp,tuning.cpp,graphics_page.cpp,movies.cpp},
both template configs, installed settings/config, docs/HANDOFF.md, and the relevant generated parser/timing bodies.
Also followed settings consumers in gpu/d3d12.cpp, texture_pack.cpp, video_hd.cpp, kernel memory helpers, and CMake.
This is a source audit; the ROM's address descriptions were cross-checked against existing generated source rather
than a new binary-analysis session.

## Build blockers found at audit time

The prior cleanup removed placebo members from app::Settings but left their callers compiled:

- mc3_extension.h:67-83 reads removed fov_horizontal, aspect_ratio, fov_multiplier and ui_scale_x.
- graphics_page.cpp:218-283 reads removed FOV, color grading toggle, bloom, SSAO, UI/HUD scale members. Its hooks
  are commented out, but the Settings() function containing those references still compiles. The CMake game source
  glob includes this file. The remaining Options hook merely forwards to the original.
- The MC3 extension's ext values have no generated HLSL consumer; its shader overrides repeat defaults, while its
  small-texture Sampled hook merely sets kind to kTex2D. Widescreen, output resolution, scaling and post-processing
  are already implemented in the host/backend. Removing this dead extension must retain the default
  ShaderHooks::Constants() float4 ext declaration and matching DrawConstants layout.

The parent was notified immediately and owns the cleanup.

## Actual settings and gaps

| Control | Actual consumer / state |
|---|---|
| Output/display/render scale/vsync/aniso | app settings/main and D3D12 targets/samplers/present; implemented |
| FXAA strength, CAS, brightness/contrast/saturation/vibrance/gamma | D3D12 PostProcess around line3260; implemented, requires graphics.post |
| Speed blur/glow/headlight strength | ScaleScreenEffect around2834; implemented using the title's existing passes, classification still needs scene verification |
| Texture detail | StageSampler around2014; Full lifts mip limits and enables16x unless explicit anisotropy selected |
| Texture pack/dump | texture_pack.cpp real consumers |
| HD movies | video_hd.cpp Enabled reads graphics.hd_movies; real, but no F1 checkbox at audit time |
| Controller inner/outer deadzones | input.cpp191-205; real radial remap of both hardware sticks, config-only |
| Force1080p/widescreen | Output/Widescreen and Xbox video flags; real |
| graphics.lod | Stored but no active consumer; F1 displays disabled Pending slider |
| FOV, SSAO, separate bloom, UI/HUD scaling legacy config | No implementation; previous config still advertises these despite loader ignoring them |
| Native game's Graphics page | Disabled unfinished prototype, use F1; no native ride-height menu integration exists |

Legacy enhancement config now only reads render.force_1080p / force_widescreen, input deadzones (fallback gameplay
names), plus legacy force_60fps/show_fps only if settings.toml omitted their new graphics names. Save writes the
small supported config and drops the stale keys. The template and installed config had not yet been cleaned.

Input mapping still merges keyboard and controller, foreground-only keyboard, port0 fallback connected even with
no controller. Hardware stays enabled in background by explicit SDL hint. Deterministic XBE_PAD_SCRIPT replaces
port0 even when the script path is invalid; this is deliberate harness behavior, but a stale environment variable
can appear to disable live input. XBE_PAD_LIVE adds input and CAPTURE commands. Neither is used in this audit.

## Timing correctness

At audit time, switching F1 from60 to30 midrace left the existing1/60 fixed step while lowering the host cap to30:
settings_menu.cpp113-116 sets cap; hooks.cpp76 only updated kStep when sixty was true. This yields half speed until
the game requests fixed timing again. Parent owns the fix and an actual-hook test.

Generated source x86_recomp.59.cpp6355-6397 confirms sub20BB00 is cdecl (ret only), stores requested float rate at
4B2D54, unsigned substeps at4B2D64, sets534770 and computes4B2D58=1/(rate*substeps); copies step to4B2D48 and inverse
to4B2D50. sub20B6F0's loc20B942 directly copies the fixed step into4B2D70 each frame. Preserve original requested
rate/substeps before changing them so disabling60 can restore the original step without a guest-call ABI guess.

## Tuning correctness: discovered and corrected

The old tuning hooks incorrectly assumed visitor return meant values were loaded. The generated vehModel visitor
subFB760 only registers fields through sub32F3D0. The latter creates0x58-byte descriptors, stores field addresses
at descriptor+4C and links them into parser+4C; it does not load a value. Thus the old code snapshotted constructor
defaults, then dropped them as reused memory as soon as the actual parser wrote loaded values.

Concrete parse boundary from generated x86_recomp.96.cpp:

- sub32F490(this=parser,arg=stream,ret4) reads a first token; its nonempty path tail-calls the split body sub32F4B7.
- At32F5A7 the field type comes from descriptor+4 and destination from+4C.
- Float type6 uses32F681..32F694, stream vfunc14 followed by fstp[destination]. Type8 reads vec3. Type10 at32F6E0
  recursively calls32F490 with the nested parser.
- sub32F910 and32F940 wrap this reader;32F9A0 opens the file then invokes32F940.
- sub32F840 destroys a parser and recursively frees type10 descriptors/parsers.
- vehSim subA3CA0 atA3E15..A3E3E creates two child parsers with32F8B0 and passes wheel pointers self+54/+58 to
  subA6DA0. Completing the parent parser therefore completes both wheel blocks.

Parent authorized scoped implementation in tuning.cpp:

1. Visitor wrappers capture their actual parser argument before calling the guest and queue field addresses by
   that parser; no constructor defaults become live.
2. Strong sub32F490 wrapper publishes loaded originals after the original returns for that exact parser.
3. Strong sub32F840 wrapper removes unused pending fields on parser destruction, including write/save visitors.
4. Validate every target/check field's committed pages before post-guest reads and each frame's reads/writes;
   unavailable entries are retired before dereference. Check vehSim owner member range before reading wheel pointers.
5. Reject nonfinite loaded floats/targets; preserve the original across re-visits of already tuned values; allow
   actual reloads to establish new originals.

tests/tuning_test.cpp exercises the actual tuning.cpp hooks with real runtime headers and mocked guest bodies,
app settings, memory/commit map. It covers before/after parsing, wrong parser, loaded values replacing defaults,
no accumulation, reloading, decommit/recommit retirement, neighbour mismatch retirement, nonfinite input, parser
destruction before reuse and unavailable object pages. Parent integrates target/CMake and runs it; no test/build
was run in this worker.

## Tuning limitations still unresolved

- Applies to all parsed car data, including traffic. There is no verified player pointer/owner path in these
  visitors. Do not claim player-only tuning or manufacture one from field offsets.
- Two neighbour values are only a reuse heuristic. Identical reused blocks can pass it. Committed-page checks
  prevent the common stale/decommitted read, but do not establish allocation generations or make read/write atomic
  against a concurrent decommit. A real owner/destructor path is the durable solution.
- It remains unverified whether changing these config fields after parse affects live physics/model copies, and
  whether TireDiameterLimit expands the garage rim selection. A callback after load may copy data elsewhere.
- Garage's own Ride Height / Ride Height Rear arrays and CarCfg bytes are documented in HANDOFF; a native slider
  and player-only save behavior remain separate unfinished work.

## Other follow-up risks

ToggleMenu's dirty flag and shared Settings are touched from window, renderer and controller threads without a
common lock. Do not fix this by taking UiLock inside Poll while g_lock is held: RenderUi already holds UiLock then
FeedController/RawPad takes g_lock, so that would invert locks. Queue controller toggles for the UI/render thread.
Settings files are saved directly rather than atomically. Force1080p output affects backend output, while exclusive
monitor mode switching still follows width/height; the UI wording says output resolution, so this is not currently
a promise to force the monitor mode. User playtest is still needed for NPC skinning, HD video rendering, Vehicle
tab behavior, minimap alpha and racer notification placement.
