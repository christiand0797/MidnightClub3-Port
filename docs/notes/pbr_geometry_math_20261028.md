# PBR geometry foundation

The reviewed module is integrated as `runtime/gpu/pbr_geometry.h/.cpp`, with a focused
`pbr_geometry` CPU test. It does not activate a renderer, change legacy shaders/default
rendering, adopt materials, change saves or affect media/GPU scheduling.
All fixtures are synthetic vectors, matrices, UV derivatives and shader text. No original-derived
asset generation or GPU execution occurs.

## What is implemented

- `runtime/gpu/pbr_geometry.h/.cpp` reuses the existing `gpu::pbr::Vec3` type.
- `BuildAffineBridge` composes **untransposed row-vector model × view** transforms and computes
  inverse-transpose normals. It exposes a negative linear determinant without arbitrarily flipping
  authored normals or raster winding. Translation affects positions, not normals.
- Input matrices must have the exact affine last column `(0,0,0,1)`. Perspective/homogeneous-scale
  matrices are rejected rather than divided or guessed into model/view space. Singular and nearly
  collinear linear rows are rejected by a relative angular determinant test. Nonfinite values and
  transforms whose position/normal matrix cannot be represented as floats are rejected.
- Safe normal normalization and derivative rescaling avoid arbitrary metre/UV-unit cutoffs.
  Normal transformation uses the inverse transpose under nonuniform scale/shear and preserves
  orthogonality to transformed surface tangents.
- `FaceNormal` gives the position-derivative cross product with an **explicit caller-selected**
  orientation flip. This does not establish the title's screen/culling convention.
- `BuildCotangentFrame` constructs U/V **gradients**, with a signed projected-area correction so
  tangent/bitangent still follow increasing U/V when the supplied normal or geometry reverses.
  It preserves skew UVs and a common normalization rather than silently inventing an orthonormal
  basis. It reports UV-derivative orientation and final basis handedness separately. Degenerate,
  near-collinear and nonfinite position/UV inputs return failure with an empty result; no fallback
  axes or different UV set are substituted.
- `GeometryHlsl()` supplies corresponding guarded transform, face-normal and cotangent helpers.
  Synthetic VS/PS wrappers compile through the existing system D3DCompile library. VS reflection
  verifies two **row-major 4×4 matrices**, each 64 bytes, at offsets 0 and 64 in a 128-byte cbuffer.

## Packing and consumption

`RowMatrix4` is exactly 16 floats in row-major order. It is deliberately different from the
**already-transposed** legacy `DrawConstants::wv/wvn` bytes. A later separate PBR cbuffer can
upload `model_to_view.values` and `normal_to_view.values` directly to explicitly `row_major`
HLSL matrices. Do not upload the whole C++ `AffineBridge` (it also contains host booleans/padding),
and do not reinterpret a legacy transposed matrix as this type.

The cotangent T/B vectors need not be perpendicular to one another or individually unit length.
They have a common scale and are perpendicular to the supplied unit N. Independently normalizing
T/B discards their relative UV metric. The existing material `TangentNormal` helper currently
normalizes supplied basis vectors individually; choosing how it consumes these cotangents needs
an explicit later integration decision/tests, not an implicit change here.

CPU composition/inversion uses double intermediates and returns floats. The HLSL helpers operate
on floats. Their formulas and guards correspond, but tests do **not** claim GPU numerical parity
or identical behavior at floating-point/denormal/rejection boundaries. Shader code returns false
on invalid data; a future integrated material shader must retain the original shading/fallback
when its geometry evidence is unavailable or a helper rejects a pixel.

## Validation

Build the `pbr_geometry_test` target from the project's `tests` CMake directory and run
`ctest --test-dir BUILD -R '^pbr_geometry$' --output-on-failure`. The integrated 17-check
run passed in `.local/f1-controls-cpu-20261027/ctest-r8.log`. Independent staged evidence
remains under `.local/pbr-geometry-stage-20261028-r1`; its `stage-result.json` digest is
`9792b53619c888777f643d339cf417b8638ceddf5f0c2c9f2cfb1f00ebf4f8d6`.

The test exercises:

- Explicit noncommuting WORLD/VIEW expectations, translation and matrix order/packing.
- Nonuniform scale, shear, rotation, mirrored and double-mirrored transforms; transformed-normal
  orthogonality to independent transformed tangents.
- Small/large well-conditioned scales, relative near-singularity, unrepresentable inverse,
  nonfinite inputs, projective matrices, overflow and zero-normal rejection.
- Normal orientation flips; UV-U/UV-V mirroring, geometric mirroring and combined cancellation;
  skew/nonorthogonal UV gradients; derivative-scale invariance; no stale outputs after rejection.
- CPU D3DCompile of the separate synthetic VS/PS helpers with strictness and warnings-as-errors,
  plus reflected row-major constant layout. No D3D device, native game or GPU worker is started.

## Integration boundaries

1. The four reviewed source/test files are integrated, with the main test CMake include.
2. Geometry and affected material/hook tests pass. The geometry module does not require linking
   the material loader implementation; it only shares its public `Vec3` declaration.
3. Keep all renderer/legacy shader paths unchanged. A later explicitly opt-in fixed-function
   geometry bridge must export the **proved actual draw-local** view position/normal while
   retaining the existing raster position; record post-extension constants and complete assembled
   input geometry as described in the source-backed bridge note.

The exact car morph/skinning register intervention points are not implemented or inferred here.
No actual normal decode, layout/program allowlist, camera/light proof, root signature, PSO,
material upload, HDR/IBL or native raster/animation acceptance is supplied. Nothing sets
`SurfaceEvidence.linear_lighting=true`, selects a material or creates a PBR enable toggle.
This module alone cannot activate a PBR renderer.
