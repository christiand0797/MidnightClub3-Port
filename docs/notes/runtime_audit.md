# Runtime GPU, Direct3D and audio audit

Updated 2026-10-21. Source audit; no game launch, input automation, build, or gameplay verification performed by this worker. The lead owns integration/build validation.

## Scope and current integration

Reviewed GPU state/shader/vertex/texture interfaces and implementations, the renderer's current diff and relevant upload/draw/clear/post code, the threaded state transfer, Direct3D entry points, DirectSound mixer/entry points, Bink wrappers, `runtime/audio/*`, and `templates/game/config/game.toml`. Texture-pack and HD-video internals are a separate worker's scope; their D3D12 binding boundaries were checked here. Large unchanged D3D12 sections were inspected by relevant call paths, not exhaustively reproved.

The last handoff's shader-slot and audio-envelope agents did **not** finish in this working tree. Their notes were absent at audit start. Do not describe those features as integrated.

The C++ `DrawConstants` and HLSL common cbuffer currently agree on one `float4 ext`, inline attributes, signed-blend/effect scales, and the three four-matrix blend arrays. The earlier five-float extension corruption is absent from these two files. Fixed-function blend generation uses weights only when register 1 is supplied. Inline transform constants are now recognized by `Walk()`; whether this fixes pedestrian limbs remains a user playtest item.

## Concrete fix made

**WORLD1–WORLD3 were never delivered to the renderer thread.** `runtime/gpu/render_thread.cpp:69` describes state ranges sent to the default enabled renderer thread. It sent one matrix from `transforms[256]`, whereas `runtime/gpu/d3d12.cpp:2361` reads four matrices at WORLD0–WORLD3 and `runtime/gpu/shaders.cpp:741` combines them. The worker's renderer `State` starts zeroed; later world matrices consequently remained zero even when the title had set them. Affected fixed-function blended positions/normals lost those matrices' weighted contributions.

Changed the range at `runtime/gpu/render_thread.cpp:79` to four matrices, preserving the state serializer and all other ranges. This is the only source edit made by this worker, explicitly authorized by the lead after discovery.

No test that merely matches this source literal was added. A meaningful unit regression would run the actual state-delta transfer into a fake backend (requires factoring the serializer or kernel/memory test doubles). The native build plus user animation check remain necessary. This fix does not prove the separate programmable pedestrian shader is correct.

## Highest-priority remaining evidence

| Finding | Evidence | Next concrete work |
|---|---|---|
| Buffer envelopes are hit in the recorded run but discarded. | `dsound.cpp:747` logs success without reading the descriptor; the current native log at line 415 records `buffer envelope modulation is not implemented` at 31.660 s. Stream EG at `dsound.cpp:1031` returns success silently. | Recover the envelope descriptor and timing/retrigger/release semantics from the title/XDK entry points, implement shared buffer/stream envelope state in the host mixer, test envelope progression independently, then verify actual sound. Do not invent millisecond/attenuation units. |
| Shader loading/selection are still stubs. | `d3d8.cpp:905` and `:909` only log unsupported and return success. Config maps them to `0x4151C0` / `0x415220`. No corresponding call log appears in the current recorded run. | Implement instruction slot loading and selection, keeping declaration layout separate from program selection; confirm guest handle/slot API behavior first. This is a real gap but current logs do not prove it caused a visible defect. |
| Pretransformed immediate geometry still enters the transformed path. | `EndPrimitive()` at `d3d8.cpp:1077` logs unsupported for FVF XYZRHW, then forces `vs_handle = handle | 1`; `vertex.cpp` recognizes pretransformed state only when bit 0 is clear. | Carry pretransformed state explicitly for synthetic immediate declarations; include it in their cache identity. Avoid treating screen-space positions as world coordinates. Current log contains no observed hit. |
| LFO modulation is unimplemented. | Buffer `dsound.cpp:743` and stream `:957` ignore descriptor and return success. | Recover descriptor semantics and apply pitch/amplitude modulation in the mixer with explicit phase/retrigger behavior. Current log contains no observed hit. |

## Additional correctness concerns to reproduce or cover

1. **Inline fallback attributes are synchronized after a push-buffer End draw.** In `Walk()`, the raw NV097 END method invokes `EndPrimitive()` at `d3d8.cpp:1153`, but `g_im_attr` is copied to `g_state.vattr` only at `:1185`, after all commands have been parsed. If an attribute is set outside a BEGIN and the same Walk span then contains a primitive whose vertex mask omits it, that draw sees the previous fallback value. The source path is definite; no affected MC3 draw was demonstrated. Synchronize before submitting each immediate primitive, and test a constant colour outside BEGIN with position-only vertices.

2. **Direct HLE constant updates do not flush earlier inline constant commands.** The four `SetVertexShaderConstant*` bodies at `d3d8.cpp:881–901` write `g_state.vsc` immediately. `Walk()` applies queued raw constant writes only at a later draw/scratch flush. If a raw write and a later HLE write target the same register, the older raw write can win. Reproduce the mixed call ordering, then flush queued state before direct writes or unify ordering. Do not add arbitrary flushes without checking draw/state interactions.

3. **The texture pack toggle is checked only on byte upload.** `BindGuestTexture()` at `d3d12.cpp:1886–1939` checks `ReplaceEnabled()` inside `sig != tx.sig || !same`. Switching the setting does not itself invalidate a cached replaced texture; an unchanged texture can remain HD after disabling or remain original after enabling. This is relevant to the settings worker. Either invalidate textures on toggle or explicitly document restart-only behavior.

4. **Post-pass creation failures do not propagate.** `PostPass()` at `d3d12.cpp:3233` returns void and can return early when the PSO is missing. `PostProcess()` continues the chain and reports success, potentially showing an unrendered target after a shader/device failure. Consider a boolean result and fallback to the ordinary blit. No current failure was recorded.

5. **Audio capabilities remain narrower than “no stubs.”** Filtering, DSP effects, output bus routing, I3DL2/reverb, and several listener operations are discarded. `EffectsDesc()` allocates zeroed DSP memory so title code can resolve effect descriptors; it does not execute those effects. `GetOutputLevels()` returns silent meters. These are valid documented limitations, not complete implementations. PCM/ADPCM mixing, stream callbacks, voice pitch/3D attenuation are implemented.

## Symbol config status

TOML parses successfully: 253 symbols, 198 nonzero mappings, 55 zero mappings, no duplicate nonzero addresses. Eleven zero entries are Direct3D functions and 27 are audio-related. The header comment claiming all zero entries mean “image has none” is not established by the source audit. A full zero-symbol audit still requires XBE disassembly/signature/inline-call evidence; this worker did not claim those absent.

Mapped DSOUND wrappers for host-owned streams and the buffer/stream voice-property functions are present. The source-level export audit is useful for unmatched `.XTLID` DSOUND functions, but passing it does not prove the HLE functions perform their contract (EG/LFO are examples).

## Legitimate platform boundary no-ops versus missing behavior

`SetTile` (console memory-controller compression), analog flicker/soft display filter selection, and a console vertex-cache flush do not inherently require GPU emulation in this native renderer. Treat them separately from dropped shader programs, envelope modulation, DSP effects, and missing output measurements. A rule to replace every `return 0` is not a correctness strategy.

Bink's absent-summary guard and frame-hold wrappers are present; the HD-video worker owns picture integration. The current hold set does not visibly clear on Bink close here, so a close-before-next-frame and reused handle would be worth covering if movie sequencing misbehaves. No such failure was demonstrated.

## Validation limits and recommended sequence

1. Lead rebuilds current integrated sources including the WORLD range fix and runs existing tests.
2. User checks title/menu/world rendering and NPC limbs; retain fixed-step timing captures.
3. Finish descriptor-backed audio EG because it is actually logged in the run.
4. Implement/test shader slot semantics and pretransformed immediate draws before describing the renderer as complete.
5. Broader DSP and symbol-zero audits remain follow-up work; this audit is not evidence of full gameplay completion.
