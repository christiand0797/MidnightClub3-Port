# Live vehicle tuning refresh regression

The concrete car must be registered as a human player's car before `sub_000FFD90` runs. The tuning hook then
sets CarCfg dirty bits `0x81` when an F1 Vehicle setting changes. The original update already performs the
required refresh; an extra time-manager rebuild is unnecessary.

The generated instructions at `0x1006CA`, `0x1006DB`, and `0x10073C` call `121970`, `FF220`, and `105450`.
`121970` overwrites the saved ride-height baseline in each wheel tune at `+0x48`; the existing garage post-hook
calls `TuningCarCfgApplied` immediately afterward. `105450` copies the tuned offsets into all four live wheels
through `A6560`. That routine writes the cached offset at `+0x10C` and adjusts compression at `+0x144` by the
offset delta, preserving the current contact displacement. `FF220` applies the renderer's hub offsets to the
live axle coefficients. None of this requires the full `FFC50` constructor/reset path.

`tests/tuning_refresh_fixture.cpp` executes the original generated bodies of `FFD90`, `121970`, `F32A0`,
`FF220`, `105450`, `A6560`, the axle setters, and the saved-config table readers. It links the production
`tuning.cpp` hooks. The narrow `121970` adapter calls `TuningCarCfgApplied` at the same boundary as the garage
hook. Geometry lookup and performance interpolation are explicit fixture boundaries. Unexpected body resets,
profile systems, and unrelated virtual calls abort the test. `GarageRefreshPlayerCar` is a no-op here because
`garage_test` separately covers concrete human-controller recovery; this fixture explicitly binds ownership.

Assertions cover:

- The unregistered preview symptom: its tune baseline is read, but no dirty cache refresh is requested.
- A registered preview's changed ride height reaching all four live wheel caches on the next update.
- Compression changing by exactly the offset delta, without a body reset.
- F1 camber and offset surviving the original renderer's wheel rebuild.
- Ride-only changes updating native axle coefficients without another garage selection.
- Ten unchanged frames preserving the caches and multipliers without repeated dirty rebuilds.
- Discarding and rebinding a sim at reused guest addresses without stale baselines.
- The original `1288C0` airbag update supplying independent axle baselines through the production matching hook,
  followed by `128620` writing the correct four wheel caches with all bags in the deflated state.
- Saved ride-height, wheel, and camber bytes remaining unchanged.

Include `tuning_refresh.cmake` from `tests/CMakeLists.txt` to enable the target when generated game sources
exist. Run from a configured native checkout:

```powershell
cmake --build out/tests --target tuning_refresh_test --config Release
ctest --test-dir out/tests -C Release -R '^tuning_refresh$' --output-on-failure
```

The focused test also passed on 2026-10-23 from the managed checkout, using the primary checkout's generated
sources. From an x64 Visual Studio developer shell:

```powershell
clang++ -std=c++20 -O2 -UNDEBUG -Wno-parentheses-equality -Iruntime `
  -I'C:/Users/purpl/Desktop/MidnightClub3-Port/templates/game/generated/game' `
  tests/tuning_refresh_fixture.cpp templates/game/src/game/tuning.cpp `
  -o out/refresh-test/tuning_refresh_test.exe
& out/refresh-test/tuning_refresh_test.exe
```

Expected output: `actual FFD90 / 121970 / F32A0 / FF220 / 105450 / A6560 refresh: PASS`.
This verifies the native refresh sequence and host hooks. Live garage-to-street ownership and rendering still
require the game QA pass; this fixture does not emulate a complete player factory or scene.
