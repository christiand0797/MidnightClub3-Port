# Tire-width growth around the wheel centre

2026-10-24. The user reported that wider tires grow into the car while their outside edge stays fixed.
The original renderer deliberately produces that behavior. This correction removes its automatic fitting
translation for verified player/selected-preview cars; tire scaling and explicit PC offsets remain separate.

## Original source and reproduced cause

`0xF32A0` refreshes the rendered wheel dimensions, then calls `0xFB4E0` for each axle. The latter reads the
visual model's tune pointer at model +0x300 and `TireExtentFront/Rear` at tune +0x1C/+0x20. Its first argument
is the selected width; the second argument, nominally a tire offset, is unused in this implementation.
Its exact finite-input result is:

```text
half = width × 0.5
fit = extent - half  when extent != 0 and half > extent
fit = 0              otherwise
```

F32A0 writes those results to render +0x12D8/+0x12DC. `0xF7170` scales the tire's X basis by twice its selected
width, then subtracts the fit translation along the first-side wheel X basis and adds it on the opposite side.
With an upright four-wheel fixture, increasing width from 355 to 755 mm moves each centre **200 mm inward**
while the nominal outside edge stays fixed. This reproduces the reported behavior using original generated
FB4E0, F7170 and its original affine/rotation routines, rather than a substitute renderer.

## Production correction and ownership

`tuning.cpp` keeps two distinct baselines when a render/model is bound or refreshed:

- `offset_original` is the untouched retail hub offset, retained for ownership restoration.
- `offset_centered` is zero only when the incoming value exactly matches the checked model/width result of
  FB4E0. Missing model metadata, nonfinite dimensions and unrelated/authored offsets retain their input baseline.

Player rendering uses the centred baseline and applies the explicit PC offset once. Consequently width changes
keep the wheel centre fixed and extend both shoulders in opposite directions. Front/rear values remain
independent; both side transforms retain their native signs, camber, wheel spin and suspension position.
This does not change selected widths, profile codes, tire/rim diameters, physical radii or traction calculations.

The existing concrete car/sim/cfg/render associations and player-sim set gate application. When a car loses
player ownership, the original retail baseline is restored with zero additional PC tuning before ownership is
removed. AI/unowned renders keep their original offsets. Repeated tuning and paint-only refreshes do not adopt
an already adjusted hub as a new baseline. The signed mapping of the existing explicit PC offset is preserved;
this investigation does not rename its direction from an assumed wheel basis.

## Focused checks

Two isolated CPU checks pass from `.local/width-center-check/out`:

1. `wheel_width_pivot` executes real FB4E0/F7170 and original matrix operations. It reproduces the old anchor,
   then checks fixed centres and equal/opposite shoulder growth for all four wheels with independent front/rear
   widths, camber -30/0/+20 degrees and explicit offsets -0.2/0/+0.2 m. It also verifies original ABI/x87 balance.
2. `tuning_width_integration` runs the production tuning wrapper and actual generated FB4E0 reader. It sweeps
   105/195/355/755 mm front widths with different rear widths, independent PC offsets/camber, repeated refresh,
   paint-only refresh, ownership removal, unowned-car writes, unknown authored offsets and missing tune metadata.
   Raw retail offsets are restored exactly on demotion. Existing tuning/airbag/wheel-solver assertions also run
   in that executable.

The shared test suite can include `tests/wheel_width_pivot.cmake`. That adds the original matrix test and the
original reader oracle to `tuning_test`; no generated source is modified. The focused checks are CPU evidence,
not a native visual certification. Native QA still needs a changed-width garage preview and street car on both
sides, with explicit offset/camber controls. The separate tire-profile bead deformation and road contact/sparks
require their own checks; symmetric width growth alone does not establish those outcomes.
