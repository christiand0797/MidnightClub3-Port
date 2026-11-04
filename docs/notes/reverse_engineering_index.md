# Original-game knowledge index

Last researched: **2026-11-04**. This is a continuing subsystem index, not a claim
that the whole game has been reverse engineered. It separates original-image
facts, port implementation behavior, bounded observations and open questions.

The original reference is USA DUB Edition Remix `default.xbe`, title ID
`0x54540079`, XDK `5849`, SHA-256
`0b3d81a02834e57864a601d473d5296cc0a2463e4b837373d91030f751fa51a3`.
The recompiler input is `templates/game/config/game.toml`; instruction-address
comments in `templates/game/generated/game/x86_recomp.N.cpp` retain original
XBE addresses. Generated filenames/line numbers can change after recompilation;
function plus original instruction address is the stable citation.

## Evidence vocabulary

- **Original source fact:** an instruction/data relationship in the pinned XBE,
  reproduced by its generated translation. This does not establish live state.
- **Port behavior:** what current handwritten runtime/game hooks do. It may
  deliberately differ from the original.
- **Bounded native observation:** a named build/context and preserved capture or
  receipt. It does not automatically apply to a later build or every vehicle/city.
- **Open:** a candidate interpretation or missing proof; it is not an API contract.

## Subsystem map

| System | Source anchors and durable notes | What remains open |
| --- | --- | --- |
| Boot/recompilation | `templates/game/config/game.toml`, `runtime/app/game_code.cpp`, `runtime/kernel/boot.cpp`; [runtime audit](runtime_audit.md), [portable setup](portable_setup.md) | Exact acceptance for each newly published runtime/compiler pair. |
| Kernel memory/handles/files | `runtime/kernel/memory.*`, `kernel.*`, `files.cpp`; [current save trace](career_save_network_trace_20261104.md) | Full original I/O failure/recovery and cross-file transaction behavior. |
| Original career/profile serialization | `0x193F0`, `0x1A550`, `0x1A680`, `0x26BD10`, `0x277250`, `0x27F740`, `0x2829E0`; [save/network trace](career_save_network_trace_20261104.md), [save routing](save_profile_recovery_20261027.md) | Whole-career schema/tail/signature; a complete safe import/load acknowledgment. |
| Career money/garage wallet | `templates/game/src/game/economy.cpp`, `0x557F0C`, data `+0xACC`; [World/Career design](f1_world_career_development.md), [exact native evidence](feature_native_20261028.md) | Current-build native proof after later renderer/network changes; all result/reward paths. |
| Race completion and reward | `0x1C2B00 -> 0x1CFC50 -> 0x285B70`, `0x2840E0`; [new trace](career_save_network_trace_20261104.md#race-completion-and-money-observation) | Every race type, loss/retry/tournament/hookman cases; persistent event identity. |
| Progression/unlocks | `0x28C3E0`, `0x292CD0`, `0x289CC0`, `tune/progress/unlockingrules.csv`; current [HANDOFF](../HANDOFF.md) Detroit entry | Full dependency graph and event ordering; no blanket unlock behavior inferred. |
| Original menu/Flash | `0x139520`, `0x1395E0`, `0x13C5D0`, `0x154E40`; [garage navigation](garage_navigation.md), [garage save transition](garage_save_crash.md) | Complete screen lifecycle/map and all native controller paths. |
| PC F1 settings/UI | `runtime/app/settings*`, `pbr_menu.*`, title `gameplay_menu.*`, `graphics_menu.*`; [graphics GUI](graphics_gui.md) | Broader physical-controller/native visual coverage. |
| Pad/input/timing | `runtime/input/*`, `templates/game/src/game/arrow_driving.cpp`, `hooks.cpp`; [navigation](garage_navigation.md), [replays](replays.md) | Remote input lease/ownership and all pause/reconnect cases. |
| Replay | `0x20EA20`, `0x20EAC0`, `0x2DAC0`, host `replay.cpp`; [replay evidence](replays.md) | Port's transform correction is not original lockstep network simulation. |
| Owned-car identity/configuration | `0x53F308`, CarCfg stride `0x108`, `0x271E20`, `0x26BB80`; [per-car tuning](per_car_tuning.md) | No proven original persistent unique ID; external roster edits remain ambiguous. |
| Car/controller construction | `0x2CBC40`, human `0x2CBDE0`, AI `0x1FAE10`; [ownership](vehicle_ownership.md), [current trace](career_save_network_trace_20261104.md#car-construction-and-remote-actor-scope) | Exact remote-player factory/controller and teardown/streaming ownership. |
| Garage part database | `garage.cpp`; [customization system](customization_system.md), [expansion](garage_expansion.md), [profiles](garage_profiles.md) | All models/parts/body compatibility. |
| Tire geometry and width/profile | `tire_vertex_fit.*`, wheel hooks; [mesh support](tire_mesh_support.md), [bead fit](tire_profile_bead_fit.md), [width centering](tire_width_centering.md) | Full native combinations, extreme camber and transient contact. |
| Ride/suspension/body floor | `tuning.cpp`, `wheel_camber.*`; [ride bounds](vehicle_ride_bounds_audit.md), [collision ownership](collision_bound_ownership.md) | All-car and road-normal contact/scrape behavior. |
| Road contacts/sparks/sounds | `scrape_diag.cpp`; [road scrape](road_scrape.md), [ground contact](ground_contact_audit.md) | Open-road stock/slammed controlled comparison, rather than wall collisions. |
| Traffic/pedestrian population | roads `0x2B9670/0x2B9690`, pedestrians `0x222770`, `world_tuning.cpp`; [World notes](f1_world_career_development.md), [late cap](world_traffic_native_cap_20261028.md) | Exact visible populations and every road/region/city. |
| NPC animation/skinning | `runtime/gpu/skinning_capture.h`; [skinning](npc_skinning.md), [source weights](npc_source_weights.md), [capture gap](npc_capture_evidence_gap.md) | Every actual post-program pose/footplant; remote actor insertion is separate. |
| Cameras/FOV/LOD | `fov.cpp`, `lod.cpp`; [LOD/FOV](lod_fov.md), [PBR geometry bridge](pbr_geometry_bridge_20261028.md) | Program-specific camera/light units and full-scene behavior. |
| XDK D3D state/shader slots | `runtime/xdk/d3d8.cpp`, `runtime/gpu/state.h`, `shaders.cpp`; [slot proof](d3d8_shader_slots.md), [neon/vinyl](neon_vinyl_20261028.md) | All state-block and multipass combinations. |
| Dedicated city PBR | `pbr_material/scene/fixed_mesh/pipeline/device.*`; [system](pbr_system.md), [reachability](pbr_reachability_20261029.md) | Full physically calibrated lighting/IBL/HDR and true geometry displacement proofs. |
| Vehicle material shading | `car_paint.h`, new `vehicle_pbr.h`; [car lights](car_paint_lighting.md), [program bridge](pbr_geometry_bridge_20261028.md) | Current exact program/material native A/B, unknown programs and all materials. |
| D3D12 queues/resources | `runtime/gpu/d3d12.cpp`, `pbr_device.*`; [HD startup fence](hd_startup_fence.md) | Intermittent fence stall cause; diagnostics do not establish a fence lifetime fix. |
| DLSS/velocity/output | `dlss.*`, velocity helpers in `shaders.cpp`; [DLSS plan](dlss_plan.md) | Every motion/replay/alpha/effect configuration and later build acceptance. |
| Texture extraction/replacement | `tools/texture_pack/*`, `texture_pack.*`; [extraction](texture_extraction.md), [coverage review](texture_visual_review.md), [second pass](texture_second_pass.md) | Per-pixel/all-UV native validation and new generated-map review. |
| Loading photos/art/layout | [loading audit](loading_art_audit.md), [capture plan](loading_photo_capture_plan.md), [R11 photo proof](native_photo_r11_20261026.md) | Remaining native city/pose comparisons; accepted images are not regenerated merely for coverage. |
| Bink/HD movies | `runtime/gpu/video*`, `tools/video/*`; [timing](video_timing_repair.md), [provenance](video_original_provenance_gate.md), [detail](video_detail.md) | Live pass completion/final audits, temporal quality and precise newer model/color claims. |
| Audio/XACT/DSound | `runtime/xdk/dsound.cpp`, `runtime/audio/*`; [envelopes/LFO](audio_envelope_lfo.md), [HANDOFF](../HANDOFF.md) song entries | Original frame-thread refill stalls versus genuine stream lifecycle bugs. |
| Original System Link/XNet | `runtime/xdk/xnet.cpp`, original `0x1AFBB0` message table; [new trace](career_save_network_trace_20261104.md#original-network-message-name-table) | Payload schemas, original session sequencing and proven native/browser bidirectional peers. |
| Browser relay/cloud accounts | `tools/multiplayer/*`; [integration snapshot](network_integration_review_20261104.md) | UI/HTTP records alone do not establish original career synchronization or roaming actors. |
| Content/release/backup | `tools/release/*`; [frozen content](content_snapshot_20261026.md), [validation](frozen_content_validation_20261026.md), [public release](public_release.md) | New exact-build native/release acceptance and a completed post-media all-file backup gate. |

## Current deep trace

[Career/save/network trace — 2026-11-04](career_save_network_trace_20261104.md)
records original save-buffer versus disk boundaries, a bounded race reward path,
original network message IDs, Network menu constructors and car factory scope.
It identifies useful observation points without claiming a finished shared-career
or remote-actor implementation.

The index links existing detailed audits rather than repeating their expensive
asset/media scans. Subsequent research can extend a specific row with function
addresses, caller chains, structure evidence and remaining uncertainties. A later
patch/build observation remains distinct from the original image behavior.
