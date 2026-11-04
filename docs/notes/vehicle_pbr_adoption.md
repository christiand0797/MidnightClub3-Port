# Vehicle PBR: where adoption stands (2026-11-04)

The renderer side exists (committed in fa21373): `runtime/gpu/vehicle_pbr.h` bridges the two recognised car vertex
programs, `D3D12Backend::PrepareVehiclePbr` binds a vehicle material's normal/ORM maps into the enhanced car paint
pixel shader (`VehicleMappedLighting`). Vehicle materials live in the same `materials-v1.toml` as the city ones,
with `surface = "vehicle"` and exact `vertex_shader` / `pixel_shader` / `uv_set` keys (never "any":
`pbr_device.cpp` rejects them), taken from the `vehicle-keys vs … ps … bridge <stage mask>` line every capture
writes per draw (d3d12.cpp `CaptureShaders`).

## Pieces

| Piece | State |
|---|---|
| Generated maps | `tools/pbr/sets/vehicles_missing.json` (541 textures that had none; `generate.py --set`), on top of the earlier car/traffic/rider maps. |
| Maps inventory | `tools/pbr/vehicle_map_inputs.py` writes `textures/vehicle_map_inputs.json` (schema `mc3-vehicle-map-inputs-v1`) for every generated texture whose disc location is a vehicle folder, with each map's SHA-256, and prints the digest for `--maps-sha256`. Generator normals are `opengl_y_up` (`--normal-convention opengl`). |
| Captures | Needed: F4 captures from a current build with cars in view (garage, Arcade cruise, traffic). None existed on 2026-11-04. |
| Pack | `tools/pbr/vehicle_pack.py` builds the vehicle material proposal from captures + inventory. It also requires a **gate receipt** (`mc3-offline-pbr-gate-v1`: strict movie audit 106/106 and a verified full project backup) whose SHA the caller supplies after reviewing it. No such receipt exists; the tool cannot and must not be fed an invented one. |
| Install | The proposal's materials join the city pack's manifest; its new digest goes in settings.toml `[pbr] approved_manifest_sha256`. |

| Gate receipt | `tools/pbr/gate_receipt.py audit` → `backup` → `receipt` builds it only from the real `upscale_movies.py --audit-only --model remacri-4x --require-original-provenance` report and an all-file SHA-256 verified backup marker (`G:\MC3_Backups`, helpers from `overnight_pipeline.py`); nothing is typed in by hand. State in `.local/pbr_gate/`. Do not use `overnight_pipeline.py --run` for this: it still hardcodes x4plus and runs `generate.py --force` on starter50, overwriting `textures/pbr/manifest.json`. |

## Movie pass (started 2026-11-04)

Strict Remacri status at start: 42/106, 76,829 frames left (~3 s/frame on the RTX 3050, so 2–3 days). It runs as
`upscale_movies.py --model remacri-4x --require-original-provenance` with report `.local/movie_pass/progress.json`
and a pause file that a wrapper sets while MC3 (or another GPU game) runs. The 37 old x4plus movies are redone too
(x4plus looked plastic and pipeline v1 had BT.601 colours).

## Next

1. Captures with cars (needs the game, so not while another agent is driving it).
2. When the movie pass finishes: `gate_receipt.py audit`, then `backup` while no other agent is writing to the
   project (the copy refuses a source that changes mid-copy), then `receipt`; the user reviews it before its SHA
   goes to `vehicle_pack.py --gate-sha256`.
3. `vehicle_pack.py`, merge into the installed pack, install, check `VehicleMappedLighting` in the log and a capture.
