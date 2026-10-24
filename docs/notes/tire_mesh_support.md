# Tire mesh support audit — 2026-10-23

The original tire meshes explain why a scalar tire radius alone cannot prove that an extreme-width, cambered
wheel stays above the road. This audit adds a read-only decoder and an affine vertex-support calculator. It does
not change assets, wheel transforms, collision geometry, tuning settings or the renderer.

## Source and format evidence

`tools/garage/tire_mesh_support.py` reads the user's loose `resources/vehicle/tire.xbpf` and matching `.xbpmap`,
cross-checking the existing `tools/texture_pack/xbpf.py` page/address parser. Verified source SHA-256 values:

| Input | SHA-256 |
|---|---|
| `tire.xbpf` | `8082778ad5d4474ae9545537817d7f500ef68419fc0f291072dfb4fda5c5213b` |
| `tire.xbpmap` | `7180f197772f22d51901e79d98b2d63585a0d60598d91c8a3c6ea0569a424ec1` |
| `default.xbe` | `0b3d81a02834e57864a601d473d5296cc0a2463e4b837373d91030f751fa51a3` |

The pool contains **98 `drwShaderModel` records and 196 decoded geometries**. Each model has two separate draw
slots at record +0x30/+0x34. Names repeat across pages, so reports retain the page number and record offset.
The 88 pages whose first texture name starts with `whl_tir_` contain eight position variants across eleven tire
brands. Their slots contain 180/60 vertices; index counts are 245 or 246, and 79 respectively. The other ten
pages use special tire artwork, including street-bike meshes, with 270/180 vertices and 415–416/241–243 indices.
All decoded vertex streams have a 28-byte stride, and every index is within its stream. All vertices in these
particular assets are referenced. The tool does not assume this for other input.

Decoding follows bounded pointers rather than searching for plausible coordinates:

- The `pf05` page table and every map/header size must agree; overlapping pages or records are rejected.
- Each record's baked address denotes its payload at +0x20. Model pointers must stay inside that payload.
- Slot headers lead to geometry tables. Geometry byte +7 gives the verified stride of 28; position XYZ is the
  first twelve bytes of each vertex. Geometry +0x18 points to the vertex resource, whose physical address maps
  back into the same record.
- Geometry +0x10 points to the index descriptor. Its pointer identifies the count word; the resource header
  begins four bytes before that word. Geometry +0x1C independently identifies the index data. **The index
  resource is not assumed to be index-data-minus-16**: original bike layouts have additional alignment.
- Nonfinite positions, unsupported resource headers/strides, cross-record pointers, incomplete arrays and
  out-of-range references fail with an explicit format error.

## Original transform evidence and the stated scenario

Generated original `templates/game/generated/game/x86_recomp.27.cpp`, function `0xF7170`, reads front/rear
width/diameter fields and stores scales **`{2 × width, full diameter, full diameter}`** at `0xF7206..0xF7270`.
The XBE float at `0x449004` is exactly 2.0. Thus a normalized X span of -0.25..+0.25 becomes the requested width;
the Y/Z scale uses full diameter, not radius.

The first side reads the axle camber at render +0x12E8/+0x12EC and converts degrees with the original float at
`0x44B090` (`0.01744999922811985`), then adds the wheel's live angle at +0x158 (`0xF75A8..0xF7642`). The opposite
side negates its angle before `0x869D0` (`0xF7D1A..0xF7D39`) and applies the float32 pi yaw `0x40490FDB`
(`0xF7DCA..0xF7DD3`). Hub offset changes along the live wheel basis; it is not necessarily a world-horizontal
translation (`0xF76C7..0xF76F9`, `0xF7CEC..0xF7D14`).

The generated report uses an **explicit illustrative column-vector convention**:
`bodyRz × steerRy × signedCamberRz × spinRx × sideRy × diag(2w,D,D)`. It uses stored float32 positions/constants
promoted to Python binary64. This is not a captured final game matrix or bit-exact execution of every guest
matrix operation. Width/diameter/camber scenarios are applied only to the 88 ordinary-artwork pages; the other
ten retain metadata and bounds without an ordinary-car scaling claim. Both draw slots remain separate; this
audit has not proved their LOD selection, simultaneous visibility or primitive assembly.

For supplied matrix M, hub translation t, normalized road normal n and plane distance d, the exact minimum over
the supplied referenced vertex cloud is `min(n · (M v + t) - d)`. The tool records this clearance, support from
the hub, and the witness's original vertex-stream index. A wheel whose hub is one scalar radius above a flat
plane can still have negative clearance when its cambered shoulder extends farther down.

## Measured example

For page 0, `tire_11`, width **0.755 m**, full diameter **0.650 m**, upright body, no steering/spin, flat vertical
normal and the stated left-side convention:

| Camber | Slot 0 support from hub | Slot 1 support from hub | Scalar radius |
|---|---:|---:|---:|
| 0° | 0.325138468 m | 0.325000000 m | 0.325 m |
| +15° | 0.385413939 m | 0.411616225 m | 0.325 m |
| +20° | 0.404726632 m | 0.434496661 m | 0.325 m |
| -30° | 0.470311932 m | 0.470192009 m | 0.325 m |

At +15°, the slot difference is **26.202 mm**; treating both slots as one ideal cylinder obscures the rounded
shoulder's geometry. At zero camber the first slot is only about 0.138 mm beyond the nominal radius. These are
mesh/scenario measurements, not observed road penetration. The right-side convention gives matching support
within binary64 roundoff here because the mesh, mirrored camber and opposite yaw transform together.

## Reproduction and remaining validation

Run with the existing Python 3.12 / NumPy environment used by the texture tools:

```powershell
py -3.12 -m unittest discover -s tools/garage -p test_tire_mesh_support.py -v
py -3.12 tools/garage/tire_mesh_support.py --width .105 .195 .355 .755 --camber -30 -15 0 15 20 --output .local/tire-mesh-support-new.json
```

**28 tests pass**, including original-asset decoding, independently known affine/plane examples, malformed
pointer/count/stride/index cases, nonfinite/overflow rejection, very large/subnormal finite normals, preservation
of separate slots and page identity, original witness indices, and refusal to overwrite an existing report.
The original-asset tests explicitly skip if the supplied assets or existing NumPy dependency are unavailable.
The successful local full report `.local/tire-mesh-support-20261023-final.json` has **7,040 support samples** and
SHA-256 `bf2358574f4d676bc773dd7f711fb12a6421463849d5e1fdc66947ab3a0c612e`. It is a local diagnostic, not shipped
game content. Source assets remained unchanged throughout the audit.

Native QA must still capture the actual selected mesh/draw slot, final wheel matrix, hub position and nearby
road normal for each axle and side, including steering, suspension motion, body roll and road slopes. It must
verify added-profile selection/purchase, garage-to-street reconstruction and representative bike/special paths.
Any future correction needs these actual inputs and ownership checks before changing hub or physics behavior.
The [source-backed capture and fixture plan](tire_contact_support_plan.md) identifies those fields and the
remaining virtual-draw/matrix gaps; no production tire-envelope correction is implemented by that plan.
The separate private chassis-floor fix and real contact/scrape/spark gates require their own native validation;
this offline analysis establishes no all-car ground-contact guarantee.
