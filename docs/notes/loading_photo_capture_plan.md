# Targeted native loading-photo capture — 2026-10-25

The opt-in diagnostic saves the **completed backbuffer of the same present that drew the photo**. It does
not generate an image, replay a CPU composite, alter the Flash transition, or require a global frame number.
Renderer/environment/output wiring and CPU tests are implemented. With no hash setting the diagnostic is inert.
No game, inference worker, input or scheduling marker was changed for this work; native validation remains open.

## Evidence for the timing and identity

Source locations below refer to `d125563` before the separate capture-directory fix:

- `runtime/gpu/d3d12.cpp:3630`, `Present`: existing F3/`g_capture` starts recording at the present boundary;
  it finishes at the next present. A transient photo drawn before the request can therefore be missed.
  `CaptureSnapTexture` at2995 is gated by an already active `cap_` and matches a volatile guest Data address.
- `CapSave(Target&)` at3935 passes `gw/gh`, deliberately reducing the capture to guest dimensions.
  `CapSave` at3914 writes a real top-down32-bit BMP after `Shrink`/`ReadBack`, not a PNG.
- `CreateSwapChain` at946 creates single-sample `DXGI_FORMAT_B8G8R8A8_UNORM` buffers. `DrawFrame` at3536
  composes the actual frame, grade/AA/sharpen, letterboxing and host UI, then leaves that backbuffer in
  `D3D12_RESOURCE_STATE_PRESENT`. `Present` calls it before `Submit(false)` and the swap-chain `Present`.
- `ReadBack` at3876 copies BGRA pixels and synchronously `Flush`es before mapping them. A diagnostic readback
  therefore stalls rendering; it is not free and cannot be described as preserving exact transition timing.
- `pack::ContentHash` in `texture_pack.cpp:213` hashes original guest format, dimensions, levels, cube flag,
  bytes and optional palette. Thus San Diego's identities remain `4D145117D9DD0F63` and `A21582D8BE650E3F`
  whether an original, active HD or staged candidate supplies the host pixels. These are the two layout-proven
 256-square DXT5 photo fills in `flash/loadscreen_garage_s.xbck`; file order is not visual left/right order.

The original generated code supplies a separate explanation for short, changing visibility:

- `x86_recomp.1.cpp`, original `sub_0001ADD0`, copies the string at0x4488D0 (`loadscreen_garage_`) on its
  garage branch at0x1AF22, appends the city suffix, and loads the selected Flash wrapper at0x1B797 through
  `sub_00139500`. It then initializes/advances the wrapper and supplies `showMenus`, `City`, `Mode` and other
  variables. San Diego's original garage description is `Six-One-Nine Customs`, at0x44889C.
- Original `sub_0001B9D0` reads the completion/request byte at loader+0x4004. When set, it writes
  `StartOfTransitionOut` (0x448698) through `sub_00139520` and polls `EndOfTransitionOut` (0x448684) through
  `sub_00139690` for1. Its render loop calls `sub_0013A0F0` with0.0333329998, renders via
  `sub_00139730`, and presents via `sub_002D2990`. `sub_0001AC60` sets that request byte.
  This proves a transition-driven loop, not a guaranteed clean portrait dwell time. No loading flag or
  ActionScript variable should be forced for this diagnostic.

## Implementation and opt-in settings

1. Read optional `XBE_CAPTURE_TEXTURE_HASHES` and `XBE_CAPTURE_TEXTURE_FRAMES` once. Require a nonempty
   `XBE_CAPTURE_DIR`, valid exact hashes, and the policy's bounded configuration; malformed opt-in values
   disable the diagnostic with a log. With no hash variable, leave the current capture/input paths alone.
   Suggested S values: `4D145117D9DD0F63,A21582D8BE650E3F` and `1,4,8,16`; `1` selects one frame.
2. Add a cached content identity to `Tex` and per-stage diagnostic identities to `DrawTextures`. The needed
   `name` is currently local to `BindGuestTexture` around1994 and exists only on content/selection changes.
   Retain it after upload/fallback, including after `CreateTexture` resets the cache entry. Expose it on cached
   binds as well. If packing/dumping are off, compute it only for opt-in eligible256-square DXT5 guest images;
   otherwise no new default hashing or readback is required. Keep original guest dimensions for this test,
   not the replacement resource's1024-square dimensions. Retain replacement-used and source/host-size metadata.
3. After successful pipeline setup and issued `DrawGeometry`, observe matching identities only for texture
   registers reflected from the **actual optimized compiled pixel shader**. `D3DReflect` results are cached by
   that blob's shader hash, only for relevant opt-in draws. An unused declaration/binding, preload/upload,
   failed/empty draw or disabled color write cannot trigger a match. Reflection failure yields no matches.
   This avoids duplicating the fixed-function/program combiner rules. Draw target/stage details are retained.
4. Call `Policy::FinishPresent()` exactly once for every present. It clears per-frame observations, requires
   all requested identities in the same frame, and returns whether this **current** frame is selected. The
   default sequence attempts four outputs, at matching-frame ordinals1/4/8/16. At most120 presents after the
   first target observation are examined, then the episode disarms; it cannot repeatedly capture later visits.
   Ordinary menus before the first target do not consume that budget. Readback failures consume their request.
5. For a selected frame, after `DrawFrame(t, bi)` and before existing `Submit(false)`, transition `bb_[bi]`
   from PRESENT to COPY_SOURCE, call existing `ReadBack(bb_[bi], bb_w_, bb_h_, pixels)`, and restore PRESENT.
   `ReadBack` flushes/resets the command list; record the restoration on that reset list before the normal
   submission. Copy the native BGRA buffer directly into a lossless top-down BMP; do not call `Shrink` or
   guest-size `CapSave(Target&)`. This includes the game wipe and host UI actually rendered for that frame,
   but excludes an OS-composited cursor or desktop occlusion. No draw occurs after the saved `DrawFrame`.
6. `CreateUniqueCaptureDirectory` creates each attempt under `XBE_CAPTURE_DIR/photo/`; `OpenCaptureFileNew`
   exclusively creates its `present.bmp` and separate `photo.json`. It never replaces `cap_dir_` or an active
   F3 session. Frame/present/time/PID names plus collision retries avoid the old HHMMSS mixing problem.
   At most four BMP attempts are allowed, with an explicit64MiB pixel-payload ceiling per image (256MiB total
   payload plus headers and small metadata). Invalid dimensions fail before readback/allocation; no evidence
   is resized to fit. Partial/error evidence is retained with failure status rather than silently accepted.

The sidecar records frame/present IDs and wall/monotonic time;
matching ordinal and exact observed original hashes; stages/draw targets, replacement-used and original/host
dimensions; backbuffer dimensions/format/index; readback/save status and explicit `post-DrawFrame/pre-Present`
capture stage, plus selected graphics settings. Source/build/invocation context, pre/post private texture-file
SHA-256 and EXE/settings SHA-256 belong in the external QA receipt. The actual swap-chain Present result is not
recorded: the saved bytes prove rendered backbuffer output, not OS presentation or foreground visibility.
Do not call bound-texture observation proof that the portrait contributed visible pixels: alpha, clipping,
offscreen draws or later overlays can hide it. Every saved image still requires actual visual inspection.

## Standalone policy and remaining validation

`runtime/gpu/photo_capture_policy.h` has no renderer, environment, filesystem, input, clock or GPU dependency.
`tests/photo_capture_policy_test.cpp` passed four CPU assertion groups with Clang/C++20. It covers inert defaults, strict bounded parsing,
wrong hashes, no cross-present panel accumulation, duplicate observations, exact same-present ordinals,
one-shot behavior, the episode deadline and its final allowed ordinal.

Three CTests pass in `.local/loading-photo-capture-policy-20261025/build`: `capture_directory_isolation`,
`photo_capture_policy` and `photo_capture_io`. The IO test compiles actual fixed-function/program HLSL using
the same optimization level as production and reflects it without creating a GPU device. It checks inactive
bound stages, unused SELECTARG arguments, passthrough coordinates, cached identity after whole-entry fallback,
entry erasure on failed allocation, overflow/byte limits, exclusive exact BGRA BMP payloads and
COPY_SOURCE/read/restore ordering on success, false and exception. The shared `ReadBack` also checks Map's
HRESULT/null pointer before memcpy. These tests do not prove actual GPU barriers, pixels or native visibility.

The user's existing native-QA authorization persists. A later bounded private run requires appropriate human
session availability and coordinated GPU quiescence; it does not require another permission prompt. No native
or inference process is started by this implementation or its CPU tests. Root owns the isolated candidate build,
keeps canonical r9/public release assets intact and defers release/adoption until native validation.

Future native A/B should collect fresh baseline and candidate sequences with the same diagnostic EXE/settings,
retain every raw BMP and metadata, and select comparable wipe phases by visual review. Preserve the existing
1536×864 raw-JPEG `s-before.png` and its evidence JSON unchanged; it has visible wipe overlays and is useful
historical context, not a lossless or phase-matched new baseline. No `s-after` exists yet. Staged candidates
remain unadopted, canonical images stay unchanged, and public r9 release assets are outside this diagnostic.
