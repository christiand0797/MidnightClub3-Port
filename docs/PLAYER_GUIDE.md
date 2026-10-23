# Player guide

This guide describes the PC settings in the current source. The native Golf R32
check confirmed immediate ride-height changes and garage-to-street retention of
ride, camber, wheel offset and engine-power values. All **19 regression tests**
pass, including execution of the original game's refresh instructions. Release
notes identify the included build; other cars and extreme physical combinations
still need individual validation.

## Download and start

Use the Windows x64 portable asset from the
[v0.3.0 development release](https://github.com/christiand0797/MidnightClub3-Port/releases/tag/v0.3.0-dev.20261023).
At this documentation update it is a draft awaiting final validation; the link
becomes public after publication. Check its notes before downloading.
Extract the entire `MidnightClub3-Native-…-win64.zip`, then double-click
`mc3.exe`. The supplied DLLs must stay beside it. The automatic GitHub source ZIP
does not contain a playable installation.

A complete-content release downloads and verifies its pinned game, texture and
movie parts on first launch. Keep about 30 GB free for an installation currently
around 14 GB. A smaller own-disc release instead asks for the supported original
Xbox USA **DUB Edition Remix** ISO. Setup creates the desktop shortcut and save
folders. See [installation, offline setup and updates](INSTALL.md) for the full
procedure. An emulator, Python and a compiler are not needed to play a packaged
release.

## Open and use the PC menu

| Action | Keyboard / mouse | Controller |
| --- | --- | --- |
| Open or close PC settings | **F1** | **Back + Y** together |
| Open the Vehicle tab directly | **F2**, or the garage's **Vehicle tuning (F2)** button | Open settings, select **Vehicle** |
| Move between controls | Click, or **Tab / Shift+Tab** | D-pad or left stick |
| Activate a button / checkbox | Click or **Enter** | **A** |
| Adjust a focused control | Arrow keys, or drag / click the control | D-pad left / right |
| Change tabs | Click the tab; use the tab bar's arrows if necessary | **LB / RB** |
| Back / close | **Escape**, **Close**, or **F1** | **B**, or **Back + Y** |

Scroll inside the menu to reach controls below the visible area. **Close** and
**Save now** remain at the bottom of the window. Settings operate on the active
game window; click it if keyboard input is going elsewhere. A controller can be
connected after launch. Physical controller and keyboard input remain available
when developer input scripts are present.

Display changes are staged. Change the fields, select **Apply display changes**,
then select **Keep settings** within **20 seconds**. Keep becomes available when
the requested scene has been created. **Revert**, a failed allocation, closing
the menu, or the timeout restores the earlier selection. **Discard pending
changes** drops unapplied edits. Other menu changes apply live and are saved
with **Save now** or when the menu closes. Saved settings apply on the next run.

## Display

The **output** is the physical monitor or window image, including the PC menu.
The **native scene** is the resolution used to render the 3D game before it is
scaled into that output. A 1080p monitor can therefore show a 4K scene downscaled
to 1080p. Selecting a larger scene does not make the PC menu tiny or create a
16K desktop window.

| Option | Choices / built-in default | Behavior |
| --- | --- | --- |
| Display mode | Fullscreen, Borderless window, Window; default **Borderless** | Chooses how the game occupies the desktop. Unsupported exclusive display modes can fall back to a usable window. |
| Monitor / window size | Desktop or reported monitor modes; default **Desktop** | Selects physical output. Borderless covers the desktop; an ordinary window is fitted into the available desktop. |
| Native scene resolution | Automatic; 640×480, 854×480, 1280×720, 1920×1080, 2560×1440, 3840×2160, 5120×2880, 7680×4320, 15360×8640 | Automatic follows output. Presets select the actual scene dimensions before render scale. Default **Automatic**. |
| Custom scene width / height | Width 320–16384, height 240–16384 pixels | Enter both dimensions. Set both to **0** for Automatic. A half-filled pair cannot be applied. |
| Scene render scale | **25–400%**, default **100%**; quick buttons 100 / 150 / 200 | Multiplies both chosen scene dimensions, or output dimensions in Automatic. 200% uses four times as many pixels. The largest axis is capped at 16384 while preserving aspect ratio. |
| Force widescreen | Default **Off** | Makes the original game use its widescreen presentation even on a narrower output. |
| Requested scene / Current scene | Read-only pixel dimensions | Requested includes render scale. Current reports the renderer's actual scene; check this after applying. |
| Colour + depth memory estimate | Read-only MiB | A minimum estimate for one target pair, excluding extra effects, textures and other GPU resources. |
| V-sync | Default **On** | Synchronizes presentation to the display to reduce tearing. This change is live. |
| Frame rate | **60 fps** default; 30 fps original-style timing | Selects the game's matching time step and frame cap. This change is live. |

For an exact 4K scene, choose **4K** with **100%** scale. For an exact 16K scene,
choose **16K** with **100%**; 16K requires substantial GPU memory and processing
time. The settings range does not guarantee that every GPU can allocate or run
every resolution. Start at 720p or 1080p, then increase gradually. Keep the game
visible during the confirmation countdown.

The old `force_1080p` setting in `config/mc3_enhancements.toml` can override
output size in older configurations. Deliberately selecting a native scene
preset clears that legacy override. The scene aspect ratio and the game's
widescreen state are separate choices; avoid an unusual custom ratio when
troubleshooting stretched presentation.

## Vehicle

These are additional **PC tuning settings** layered over the game's purchased
parts and saved garage setup. They form one PC profile that follows the player's
cars; they are not individual per-car save slots. Ordinary traffic and opponents
are intended to keep their original physics. The garage's wheel fit limit is
model data and applies when the relevant model loads.

Live changes now request the game's original wheel/suspension refresh immediately;
changing another garage part is unnecessary. The Golf R32 check confirmed that
ride, camber, offset and engine-power values also survive the return to the street.
See the [tested values and scope](notes/live_vehicle_tuning.md). Spring, damping,
brake and grip destinations are covered by code regressions; their driving feel
and every car's physical limits remain separate playtests.

| Option | Range / built-in default | Behavior |
| --- | --- | --- |
| Extended garage choices | Default **On** | Adds steps in the original game's wheel, tire and suspension lists. Reopen the relevant garage list after changing it. Turning this off does not erase saved extended values. |
| Ride height front / rear | **−2.000 to +2.000 m**, default **0** each | Additional per-axle suspension offset on top of the saved garage height. Negative lowers; positive raises. −0.100 m means 10 cm lower. The range and settings save/load are tested; physical fit at ±2 m is not validated for every car. Earlier builds have a ±0.500 m limit. |
| Allow wheels to tuck into the body | Default **On** | Permits tire/body intersection when deeply lowered. The intended contact behavior keeps tires on the road rather than forcing them down through the floor. |
| Camber front / rear | **−30 to +20 degrees**, default **0** each | Additional visual wheel angle relative to the saved setup. Negative tilts the wheel tops inward. |
| Wheel offset front / rear | **−0.300 to +0.300 m**, default **0** each | Additional distance per wheel side: positive outward, negative inward. +0.020 m moves each side outward by 2 cm. |
| Airbags follow selected ride height | Default **On** | Uses the installed airbag controls. With selected per-axle height H, deflated is H; inflated is H + 2×abs(H). Example: H = −0.20 m travels from −0.20 to +0.20 m. |
| Front / rear spring stiffness | **×0.25–3.00**, default **×1** | Multiplies the car's spring coefficient. Higher is stiffer. |
| Front / rear damping | **×0.25–3.00**, default **×1** | Multiplies suspension damping. Higher resists suspension movement more. |
| Front / rear brake force | **×0.25–3.00**, default **×1** | Multiplies each axle's braking coefficient. |
| Front / rear tire grip | **×0.50–2.00**, default **×1** | Multiplies static and sliding tire friction. |
| Engine horsepower | **×0.50–3.00**, default **×1** | Multiplies engine power after purchased upgrades. |
| Wheel and tire size limit | **×1.00–3.00**, default **×1.60** | Expands the stock maximum used by the garage's fit check. Takes effect when the car/model loads; it does not itself select or buy a larger wheel. |
| Reset extra vehicle tuning | Button | Restores this page's additional tuning defaults while preserving Extended garage choices and the game's purchased parts. |

Very low ride heights are intended to let the chassis contact the road and use
the game's existing scrape/spark effects. Body contact, wheel tuck and airbag
travel have not been visually verified for every car, both axles and every road
surface. A custom ride height is not a guarantee of continuous sparks. Extreme
camber, wide tires and the full ±2 m travel still require physical fit checks.

To distinguish the two menus: buy/select rims, tires, suspension and airbags in
the **original garage**, then use **F2** for the additional PC controls. A larger
fit limit merely makes larger original-garage choices possible. Change one value,
close the PC overlay, and inspect the car. If an effect fails in the fixed build,
record the setting, vehicle, values and garage/street state for a reproducible report.

The extended original garage currently offers 38 tire-width choices from
**105–755 mm**, 29 ride-height selections, and 17 rim labels from **12–28**.
The width steps are not uniform and a car's fit limits can restrict the usable
combinations. Choose front and rear independently where the original menu
provides those pages.

The tire-profile expansion adds 26 entries for **34 total profiles**;
original save codes 0–7 retain their meaning. New labels run from **Profile ratio
30%** to **Profile ratio 97.5%** in 2.5 percentage-point steps, reusing the retail
60% and 72.5% entries. This is the engine's calibrated diameter ratio: **a larger
ratio gives a thinner sidewall**. It is not a conventional width-based 35 / 40 /
45 tire aspect label. Find it in **Garage → Wheels/Tires → Tire Profile**, not in
F1. Newly saved profiles remain readable with Extended garage choices turned
off. Confirm the release notes include the expansion before relying on its new
entries. The generated game-code regression checks pass; each vehicle's fit in
driving still requires a playtest.

## Input

| Option | Range / built-in default | Behavior |
| --- | --- | --- |
| Inner stick deadzone | **0.00–0.90**, default **0.15** | Fraction of stick travel around the center that is ignored. Increase slightly for stick drift. Applies radially to both sticks of controllers. |
| Full stick threshold | From inner + **0.05** through **1.00**, default **0.95** | Travel at which a stick reaches full input. The region between inner and outer thresholds is smoothly rescaled. |
| Reset controller response | Button | Restores 0.15 inner and 0.95 full threshold. |

Keyboard game controls are separate from PC menu navigation:

| Keyboard | Game input |
| --- | --- |
| Enter | Start |
| Space | A / confirm |
| Escape | B / back |
| Backspace | Back |
| Arrow keys | D-pad |
| W / S | Accelerate / brake |
| A / D | Steer left / right |
| X / Y | Corresponding face buttons |
| Q / E | Original black / white buttons |

## Quality

The three preset buttons change only anisotropic filtering, anti-aliasing,
sharpening, texture detail and level of detail. They preserve the chosen scene
resolution, picture adjustments, effects, HD pack and camera settings.

| Preset | Filtering | FXAA strength | Sharpening | Texture detail | LOD |
| --- | --- | --- | --- | --- | --- |
| Performance | 4× | 0.75 | 0.20 | Full | ×0.85 |
| Balanced | 16× | 1.00 | 0.35 | Full | ×1.00 |
| High detail | 16× | 1.00 | 0.25 | Full | ×1.50 |

| Option | Range / built-in default | Behavior |
| --- | --- | --- |
| Anisotropic filtering | As the game asks, 2×, 4×, 8×, 16×; default **As the game asks** | Improves texture clarity at shallow viewing angles. Full texture detail also requests high-quality filtering. |
| Texture detail | As the console, **Full** default, Max | Full permits the sharpest mip with filtering. Max uses only the sharpest mip and can shimmer at a distance. |
| Anti-aliasing | Off / **FXAA** default | Smooths edges in the post-processing pass. |
| Anti-aliasing strength | **0–1**, default **1** | FXAA intensity; shown when FXAA is selected. |
| Sharpening (CAS) | **0–1**, default **0.35** | Contrast adaptive sharpening. High values can emphasize noise or halos. |
| HD texture pack | Default **On** | Uses installed `textures/replace/<hash>.png` replacements where available. |
| HD movies | Default **On** | Uses matching `video_hd` MP4s with original movie audio/timing where available. |
| Dump textures | Default **Off** | Writes encountered textures to `textures/dump` for mod work. This can consume disk space. |
| Motion blur | **0–2**, default **1** | Original speed-blur multiplier: 0 off, 1 original, 2 doubled. |
| Light glow | **0–2**, default **1** | Original streetlight/neon glow multiplier. |
| Headlight pools | **0–2**, default **1** | Headlight light on the road. |
| Neon brightness | **0–4**, default **2** | Vehicle underglow multiplier; 1 is original brightness. |
| Level of detail | **×0.50–4.00**, default **×1** | Extends model-detail and prop-draw distances. Values above 1 also increase far clipping, capped at ×2, from the next race/free-roam load. |
| Field of view | **×0.75–1.50**, default **×1** | Scales the driving camera's vertical FOV. Menus and mirror retain theirs. |

The pack toggle cannot create a missing replacement. Turning HD movies off is a
useful comparison when a movie seems softer or shows an artifact. The additional
texture/movie upscale pass remains separate work; this guide does not claim it
has completed. PBR conversion is planned separately and is not an active renderer
mode in this menu.

## Picture

These adjustments affect the final image. Disabling **Post-processing** disables
the picture chain, including its anti-aliasing and sharpening effects; this does
not unload the HD texture/movie packs or reset the separate game-effect sliders.

| Option | Range / built-in default | Behavior |
| --- | --- | --- |
| Post-processing | Default **On** | Enables the final post-processing chain. Picture sliders are disabled when it is off. |
| Brightness | **−0.50–+0.50**, default **0** | Adds brightness. |
| Contrast | **0.50–1.50**, default **1** | Adjusts contrast around middle gray. |
| Saturation | **0–2**, default **1** | 0 removes color; larger values intensify color. |
| Vibrance | **−1–+1**, default **0** | Changes subdued colors more than already vivid colors. |
| Gamma | **0.50–2.00**, default **1** | Above 1 darkens midtones; below 1 lightens them. |
| Reset picture | Button | Resets the five numeric picture adjustments. It does not reset other tabs. |

## Info and saving

**Info → Show frame rate** enables a small FPS counter in the game. It is off by
default. The tab also displays the current measured frame rate and menu controls.

Paths are relative to the folder containing `mc3.exe`:

| Path | Contents |
| --- | --- |
| `settings.toml` | Display, `[graphics]`, `[tuning]` and other runtime settings. |
| `config/mc3_enhancements.toml` | Legacy output/widescreen flags under `[render]`; controller deadzones under `[input]`. |
| `config/disc-path.txt` | Remembered own-disc ISO location. |
| `hdd` | Original game saves. Back this up before changing PCs. |
| `logs` | Run logs and diagnostics. |
| `CrashDumps` | Crash reports when generated. |
| `textures/replace`, `video_hd` | Installed replacements; do not rename their files. |

The defaults in this guide are compiled defaults. An existing settings file or
a release's supplied configuration can override them. Updating the executable
preserves the files above; uninstalling or replacing the entire folder can lose
your local saves. **Save now** saves PC options; the game's own save process still
handles purchased parts, career progress and per-car garage setups.

## Troubleshooting

| Symptom | First steps |
| --- | --- |
| No F1 / keyboard response | Focus the game window. Try F2, then F1 to close the menu. Enter is Start in the game; Space is A. Check that another window or dialog is not in front. |
| Controller drift or weak steering | Input → Reset controller response, then raise inner deadzone only as needed. Reconnect the pad. Use D-pad for menu navigation. |
| Menu has more controls than fit | Scroll within the main panel. Tabs can scroll horizontally. Keep output at desktop size; change native scene resolution separately. |
| Black screen after changing resolution | Wait the full 20 seconds for automatic revert. Avoid confirming an unusable mode. Next try Borderless, Automatic or 1280×720 scene, 100% scale. |
| Game cannot start with saved display settings | Close it. Back up and rename `settings.toml` to `settings.toml.backup`; also back up/rename `config/mc3_enhancements.toml` if a legacy forced-output setting persists. Launch to regenerate defaults. Keep `hdd` unchanged. |
| 4K / 8K / 16K is slow or reverts | Reduce native scene resolution and keep scale at 100%. A 200% scale on a preset doubles each axis. GPU allocation recovery restores earlier settings or a small safe scene. |
| Blur, shimmering or oversharpening | Compare Balanced, Full texture detail, FXAA, CAS 0.2–0.35 and 100% scale. Avoid Max texture detail while checking distant shimmer. |
| Vehicle slider saves but does not affect car | Use a build containing the [driving ownership/refresh repair](notes/live_vehicle_tuning.md). The Golf R32 passed immediate ride updates and garage-to-street retention. Reset extra tuning to compare, then report the car, option/value and garage/street state if another case fails. |
| Tire goes through ground / incorrect airbag height | Record the vehicle, front/rear, garage ride step, PC ride values, tire size/profile and road/garage location. All-car contact behavior is still being validated. |
| Movie is soft, fails or black | Compare Quality → HD movies Off. Keep installed movie names unchanged. On Windows N, install the Media Feature Pack linked in INSTALL. |
| Setup interrupted / checksum error | Start the same extracted `mc3.exe` again. Keep parts from the same content release. Setup can resume/repair; do not mix manifests or manually edit its marker. |
| ISO drive moved | Use `mc3.exe --choose-disc`, or see INSTALL for `--disc`. |
| Crash | Preserve `logs` and any `CrashDumps`, note the last menu/action/car, and include the release version when reporting it. |

For updates, extract the new portable release into the existing installation
with the game closed, preserving `hdd`, `settings.toml` and `config`. For a move
to another PC, copy the complete installation or follow [INSTALL](INSTALL.md).
Launch once at the new location to refresh the desktop shortcut.
