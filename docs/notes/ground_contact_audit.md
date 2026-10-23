# Ground contact audit: native chassis floor and wheel tuck

2026-10-23. Read-only investigation of the current native port; no production source, game asset, runtime,
save, or published release was changed. No game was launched. This note proposes work and identifies missing
evidence; it does not certify chassis contact, extreme tire geometry, or sparks for every vehicle.

Related evidence: [road_scrape.md](road_scrape.md), [vehicle_ownership.md](vehicle_ownership.md),
[garage_expansion.md](garage_expansion.md), and [garage_profiles.md](garage_profiles.md). Older statements in
`customization_system.md` and historical sections of `garage_tuning.md` that describe an entirely fixed bound,
or attribute ground penetration solely to wheel travel, are superseded by the instruction flow below.

## Finding

The current wheel travel extension and visual arch-fit bypass address real wheel-position clamps. They do not
stop another original mechanism from retracting the chassis collision floor upward as ride height is lowered.
That retraction can let the visible body pass below the road before the chassis bound makes contact. Changing
the suspension solver again is not the smallest first correction for this mechanism.

The smallest candidate is a concretely owned-player wrapper at `0x121440`, restricted to the original refresh
caller `0x10570C`, that removes the negative-ride contribution from its argument, then runs the original bound
writer/rebuild. Keep tire-radius compensation and the original bound's stored shape/taper behavior. This is a
candidate, not an installed fix. Bound ownership/aliasing and bundled floor geometry must be established before
it is safe to claim that same-model AI and the visible underside are unaffected.

## Exact native dataflow

All addresses below are guest addresses from the original `default.xbe`. Its SHA-256 is
`0b3d81a02834e57864a601d473d5296cc0a2463e4b837373d91030f751fa51a3`.
The generated original instructions are in `templates/game/generated/game/x86_recomp.28.cpp` and `.32.cpp`.

`0x105450` is the original wheel/ride cache refresh. It calls `0xA6560` for all four runtime wheels and copies
their tuned radius and width to `wheel+0x100/+0x104`. Front wheels are body `+0xD8/+0xE0`; rear wheels are
`+0xDC/+0xE4`. The floor calculation reads one cached suspension offset from each axle, at runtime wheel
`+0x10C`, after that refresh.

`0xF11E0(render, axle)` reads model resources through `render+0x1800 -> +0x18`, selects the node at
`resources+0x3818+4*axle`, and returns `2*node[+0x14]`. Its missing-node fallback at `0x454868` is **-0.3**,
a sentinel, not a usable positive tire diameter. A proposed wrapper must not treat missing nodes as valid radii.

Define:

```
Rs = (stock front node radius + stock rear node radius) / 2
Rc = (render front tire diameter + render rear tire diameter) / 4
H  = (front cached SuspensionOffset + rear cached SuspensionOffset) / 2
A  = max(0, Rs - Rc - H)
```

The render diameters are `render+0x12D4/+0x12CC`. At `0x1056C5..0x1056F9`, the code forms `A`, then pushes
it at `0x105706` and calls `0x121440` at `0x105707` (return address `0x10570C`). It skips the call when
`0xFF200` identifies resource type 2. Thus this is a single average-height floor adjustment, not independent
front/rear floor planes; the rigid body itself can still pitch/roll.

The earlier portion of `0x105450` separately changes the simulation's size/centre term at `sim+0x18`. This
candidate does not alter it. Changes to that term would need separate inertia/rigid-body evidence.

`0x121150` prepares the floor information once during human/AI physics construction (callers `0x2CB430` and
`0x1FAA5F`). It obtains the bound through `0xA2450`:

```
bound = [[[[body + 0x04] + 0x1C] + 0x04] + 0x0C]
vertex count = bound[+0x4C]
float3 vertex array = bound[+0x68], stride 12 bytes
```

It scans for the minimum vertex Y, then classifies vertices strictly inside `(minimumY-0.05, minimumY+0.05)`
as the bottom layer. It stores their indices at body `+0x190...`; the writer later consumes exactly four slots
`+0x190/+0x194/+0x198/+0x19C`. Unused slots start at `-1` in the body constructor `0x120930`.
The classifier has no visible four-slot count guard: a fixture should verify that every applicable bundled
shape has the expected layer, rather than assuming arbitrary meshes are supported.

It caches:

- body `+0x1A0`: original minimum Y, `Y0`;
- body `+0x1A4`: the lowest Y above that bottom layer, minus 0.05, `Ycap`;
- body `+0x1A8`: maximum absolute X of vertices above the bottom layer, `Xhalf`.

`0x121440` skips body type 2. For each non-sentinel bottom index it computes:

```
Yfloor = min(Y0 + sim[+0xD8] + A, Ycap)
taper  = tan(sim[+0xDC]) * (Ycap - Yfloor)
Xfloor = -Xhalf + taper  if the old vertex X is negative
         Xhalf - taper  otherwise
Zfloor = the old vertex Z
```

`sim` here is body `+0xB4`. Constructor `0x121550` stores `+0xD8 = 0.05` at `0x121671` and `+0xDC = 0`
at `0x121679`. Constants read directly from the XBE include `0x447C18=0.5`, `0x44AAEC=0.05`, and
`0x4499F0=0.01`. The floor writer ends by rebuilding polygon data with `0x2395F0` and the bound's virtual
`+0xB0` method at `0x121530/0x121539`.

The generated body for `0x121440` explicitly tail-calls `sub_001214F0` immediately after instruction
`0x1214EC`. `0x1214F0` continues the same loop and stack frame; it is not an independent public function
that may safely be invoked with only a body pointer and float. A native fixture must execute both generated
bodies and preserve the original stack/x87/flag behavior.

For equal front/rear stock and current radii, the unmodified result before the cap is:

| Mean cached ride H | Argument A | Floor above bundled Y0 |
|---:|---:|---:|
| 0 | 0 | 0.05 m |
| -0.10 m | 0.10 m | 0.15 m |
| -0.15 m | 0.15 m | 0.20 m |
| -0.35 m | 0.35 m | 0.40 m |
| front -0.35 m, rear 0 | 0.175 m | 0.225 m |

The actual rise is limited by `Ycap`. These are instruction-derived values, not measurements of a specific
vehicle's visible floorpan.

## Candidate correction and scope

For an owned player with finite valid geometry and negative `H`, replace the argument with:

```
Acorrected = max(0, Rs - Rc)
```

For `H >= 0`, pass the original argument unchanged. Equivalently, the owned path could use
`max(0, Rs - Rc - max(H,0))`. This removes only the floor retraction caused by lowering. It preserves the
original response to tire size and positive ride height, the 0.05 m native skin, taper, cap, indices, polygon
rebuild, and real contact/spark handlers. It is continuous at stock `H=0`.

Do not activate that replacement only on crossing `H=-0.15`: for equal radii that would abruptly extend the
collider downward by roughly 0.15 m at the threshold. If retaining every original retail lowering step is a
requirement, a continuous transition needs a separate design and validation; a hard deep-only switch is unsafe.

The wrapper should verify the original return address, the concrete owned car's current body at `car+8`,
the current simulation at `car+0x50` and body `+0xB4`, current cfg binding, committed ranges, finite offsets,
positive stock radii and current diameters, valid bound indices, and a finite ordered `Y0/Ycap`. Reuse existing
ownership/lifetime state; a shared CarCfg is not ownership evidence. Snapshot the relevant ownership data under
the tuning lock, release it, then call the original function. Do not hold a nonrecursive lock across guest calls.

Even the corrected equal-radius floor is `min(Y0+0.05,Ycap)`, not the unmodified bundled minimum `Y0`.
That 5 cm can matter when comparing a visible underside with collision contact. Do not globally zero `sim+D8`,
translate the whole collision shape, or invent a per-car downward offset without measuring the relevant body
mesh and bound in the same coordinate frame. Tire-size compensation can also raise the floor when tires shrink;
this proposal intentionally retains it and therefore does not by itself promise zero visible body penetration
for arbitrarily tiny tires.

## Bound ownership is a required check

`0x121DE0` obtains a resource-supplied shape and creates/attaches the physics archetype. Its call to `0x2A03A0`
uses the `<car>_bound` name; `0x2A5A80` first checks a `bound/<name>.phys` resource. The later setter
`0x2A4BB0` stores the supplied shape pointer at archetype `+0x0C`, increments its reference count when enabled,
and does not clone it there. This does not prove that car construction shares the shape: its input may already
be an instance-specific clone. It also does not prove uniqueness.

Before a player-only bound modification, construct a player and AI of the same model with different cfgs,
then with the same cfg; compare bound pointers and `bound+0x68` vertex-array pointers. Refresh the player and
confirm AI vertices and contact geometry stay byte-for-byte unchanged. Repeat a preview/street reconstruction
and delayed destruction. If shapes/arrays alias, an ownership-gated write is still a shared mutation; isolate
the shape through an established native clone/lifetime path first. Do not mutate the shared resource in place.

## Original asset evidence and remaining measurement

Read-only Dave archive inventory found no bound-named paths in player resource archives `assets_x.dat_2.._5`.
AI `.bnd` files occur in `_1`, and AI `_bound.phys` files in the primary archive. The selected original 350Z
bundle is `resources/vehicle/vp_350z_04/vp_350z_04.xbck`, 353872 bytes, SHA-256
`fb52ab184fa39abef5c92702dd940c0362bf7ecb0b4042fae1281ff76edb9c8b`; it contains `@boundbone` at file
offset `0x4C2FF`. A name and plausible float values are not sufficient to identify the serialized bound vertices.

Required per-model measurements are the actual loaded bound vertices/indices and its coordinate transform,
the visible underside of the stock body and installed bumpers/skirts, and wheel node centres/radii. Record
`Y0`, `Ycap`, bottom indices, floor after refresh, and minimum body-mesh Y in the same chassis frame. The
catalogue includes bikes/type-2 behavior and tune files with runtime-derived model fields; neither the 350Z
bundle nor a scalar tune-file survey proves the whole catalogue.

## Wide/cambered tires are a separate support problem

Physics radius `wheel+0x100` is a scalar. Eliminating the arch renderer's downward translation and widening
the native solver's travel can preserve its solved contact position while a wide, tilted rendered tire still
intersects the road. For an ideal cylindrical tire on a flat road, the half-height is
`R*abs(cos(camber)) + (width/2)*abs(sin(camber))`; it can exceed R. Real meshes, both side transforms,
steering, suspension axis, body roll, road normal, and the rim must be measured before using that expression.

A general rendered support test is `min(dot(n, M*p + t) - planeDistance)` over the actual tire/rim vertices,
where M/t are the final wheel transform and n is the actual contact normal. A world-Y radius correction is
not sufficient on ramps. A visual-only support lift would also separate rendered hubs from physical contact,
so it should not be mistaken for a physical tire-envelope solution. Keep this work independent of chassis-floor
correction; it cannot be certified by the existing radius-based solver fixture.

## Required offline and bounded native validation

1. Add a new generated-original floor fixture executing `0x121150`, `0x121440`, its `0x1214F0` continuation,
   and `0x2395F0` with real polygon inputs; use a narrow explicit boundary for virtual `+0xB0`. Check changed
   XYZ, untouched nonbottom vertices, valid/-1 indices, next-layer cap, taper signs, x87 balance, stack return,
   repeat refresh idempotence, type 2 skip, and same-model aliasing. The existing
   `tests/tuning_refresh_fixture.cpp` **stubs `0x121440`** and only records its argument.
2. Feed the original `0x105450` refresh into that fixture through the proposed wrapper, with stock and changed
   tire radii, front-only/rear-only/equal/mixed ride settings, positive and negative H, threshold neighbours,
   missing/invalid geometry, respawn, ownership changes, and AI. Keep stock H=0 and all AI outputs identical.
   Include a positive-H/negative-H mixed axle case so mean-height cancellation is explicit.
3. Extend the existing generated `0xA7250` solver and both-side renderer fixtures to combine the corrected
   body floor with four separate solved wheel centres. Do not replace the solver with a repeated formula.
   The current suspension sweep does not simulate a rigid chassis settling on its polygon bound.
4. Add a generated-original `0x1027D0` contact regression that records actual `0x36F60` spark-emitter and
   `0x10B130` sound calls. Preserve the native response-magnitude >30 gate, relative speed >5 gate, sliding
   dot interval, material-6 exclusion, per-car Spark properties, integer budget, roof branch, and damage-out
   early return. Zero contacts/stationary conditions must not produce invented sparks. A real contact alone
   does not guarantee a spark every frame.
5. Only after offline geometry/alias tests pass, run a bounded native playtest: low coupe, sedan, SUV/pickup,
   and motorcycle; original/corrected captures on the same flat road and shallow ramp; front/rear/equal lowering,
   stock and changed diameters, widest/narrowest tires, both camber signs, steering/roll, airbags fully down/up.
   Inspect all four tire bottoms, visible underside/contact normal, genuine scrape sound, and motion-dependent
   sparks. Record stock, AI, stationary, airborne and no-contact controls. Stop after reproducing a failure;
   diagnose the specific layer rather than extending an open-ended playtest.

This audit performed source/instruction and archive reads only. These required fixtures and live observations
have not been implemented or run by this investigation.
