# Additional tire profiles — 2026-10-23

The original garage now offers **34 tire profiles** per supported axle, alongside the existing 38 widths,
29 ride selections and 17 rim sizes. Enable **F1 > Vehicle > Extended garage choices**, then use the game's
own tire-profile customization page. Both axles use the same appended profile catalogue; the original per-car
front/rear selection flow and purchase/refresh path remain in use.
The later [driving ownership repair](vehicle_ownership.md) addresses PC tuning being lost between garage and street.

New entries read **Profile ratio 30%** through **Profile ratio 97.5%**. A higher value means a thinner sidewall;
a lower value gives a taller tire. These are the game's internal profile ratios, **not** the conventional
width-based aspect-ratio numbers printed on real tires. Width and rim selection remain separate garage choices.
Extreme combinations can intersect the body, as requested; live road contact at extreme camber/width remains
the separate investigation in [road_scrape.md](road_scrape.md).

## Save and diameter mapping

The original `CarCfg` saves front/rear profile as signed bytes at `+0x1C/+0x1D`. Codes 0–7 retain their exact
original values; `0xFF` retains the automatic-profile sentinel and original eight-profile auto selection.
New codes **8–33** append the following ratios, in this permanent save order:

```
30, 32.5, 35, 37.5, 40, 42.5, 45, 47.5, 50, 52.5, 55, 57.5,
62.5, 65, 67.5, 70, 75, 77.5, 80, 82.5, 85, 87.5, 90, 92.5, 95, 97.5 percent
```

The original eight ratios are approximately 41.5532, 47.585, 53.8852, 60, 66, 72.5, 78.2 and 83 percent.
The visible list merges and sorts both groups from thickest to thinnest; display ordering never changes save
codes. Disabling extended choices does not disable decoding saved extended profiles.

Original diameter is `T[rim] / P[profile]`: `T` at `0x4B3910`, `P` at `0x471958`.
`T` is calibrated to nominal rim inches plus three (20 uses 0.5842 m), so calling `P` a conventional physical
sidewall aspect ratio would be misleading. The extension reads this original calibrated table directly.

The five original numeric table readers were audited:

| Function | Extended behavior |
|---|---|
| `0x270BC0` | Returns the added ratio for model/tire builders |
| `0x270B40` | Returns the added tire diameter for wheel visuals |
| `0x272FE0` | Returns the same diameter from either axle's saved profile for physics |
| `0x270B60` | Evaluates actual radius/ride constraints without indexing beyond either retail table |
| `0x270AF0` | Unchanged original eight-profile automatic selection |

Original getters still run for stock codes. Unknown signed/positive profile codes use the last stock ratio
instead of indexing unrelated memory. Added codes stay below 127, preserving the signed-byte contract.
The existing `CarCfg::Set` branch at `0x2723A6` accepts these codes unchanged and requests the original
wheel/suspension refresh with dirty flags `0x81`.

## Mesh resource lookup correction

A later source/asset audit found a separate eight-entry mesh table that the diameter-only tests did not cover.
Both original `resources/vehicle/decal.xbck` and `decal_g.xbck` have rendering-root `+0x44 = 8` and the eight
profile resource-array pointers at `+0x2C` (file offset `0x5DB0`). Profile codes 8 onward previously read
adjacent descriptors/data in the original `0xF6A20`, `0xFBA90` and `0xFCF20` mesh lookups. This is a correction
after the frozen `v0.3.0-dev.20261023` package; the published package does not acquire source changes automatically.

After original resource loading (`0xF5E70`), a separate **136-byte system-owned pointer table** retains the eight
stock entries and adds aliases for codes 8–33. Each new code selects the original sidewall mesh whose ratio is
nearest its requested ratio. Every alias points at an existing descriptor; none takes ownership of its meshes
or textures. The original rendering count remains **8**, preserving relocation and descriptor teardown semantics.
Before original resource unloading (`0xF5270`), the original pointer is restored. The separately allocated alias
table is released after original teardown, including exception unwinding. Reload/reused-root checks compare
the current root and table identity before restoration, preventing writes into a replacement resource image.

The saved profile bytes, render profile codes and diameter calculations keep their full appended values.
`0xF11C0` is a **numeric ratio getter** used by drawing matrices, so reducing all render codes to a stock mesh
index would incorrectly change those matrices. It remains unchanged on the normal alias path.

If the small alias allocation fails, the post-`0xF32A0` helper maps mesh slots to stock indices before any direct
resource read. A checked fallback retains the exact extended ratio in `0xF11C0`, requiring matching saved profile
bytes, current mesh slot and calibrated rim/diameter. Independent render objects retain independent ratios;
stale cfg/diameter bindings are rejected. Allocation recovery restores the full mesh codes at the next refresh.
Malformed signed/unknown render codes normalize to stock profile 7. Saved bytes are never rewritten by this helper,
and decoding remains active when extended garage choices are hidden.

## Garage allocation and labels

The custom database's profile list at `+0xF0` is extended through original append method `0x274980` after its
eight stock names. Reinitialization checks the actual name sequence and cannot duplicate indices.
`TiresProfileNames`, `TiresProfileStatus` and `TiresProfilePrice` grow together. The current movie must expose
the expected names/status arrays and have sufficient free Flash symbols plus a 16-symbol reserve before any
array mutation. New prices copy the existing first price. A movie without a profile page is left alone.

New label sources share a retained 32 KiB guest arena. Every string-table entry receives its own ordinary
guest allocation, including descriptions. No table entry owns an arena interior pointer. The existing
teardown hook invalidates the label cache before table destruction and handles reused table addresses.

## Verification

The garage fixture checks all **26 new profiles × 17 original rims × both axles = 884 physical diameter reads**
against corresponding visual diameters, extended-option-off compatibility, malformed-code fallback, fit-query
accept/reject cases and stable database indices. With generated sources present it executes the **actual original**
stock ratio/diameter/automatic readers and original `CarCfg::Set`, checking all 34 codes on both axles.

Flash fixtures verify atomic name/status/price growth, rejection one symbol below the required pool reserve,
sorted labels, no duplicate members on refresh and restoration of VM globals. The real generated string-table
destruction path runs across repeated purchase/save exits and reused table addresses, including all new labels
and a failed allocation/retry. These tests do not replace the parent's native UI purchase/drive checks.

The separate `profile_mesh` fixture executes the **actual original `0xF6A20` resource loads and `0xF11C0` ratio
getter** with the production hooks: all 34 profiles on both axles across 11 tire brands (**748 resource reads**,
plus 68 exact ratio checks). It covers original pointer/count preservation, unload-before-free ownership,
repeated loads and reused resource addresses, allocation failure across two independent renders, recovery without
another purchase, malformed codes, stale cfg and mismatched-diameter rejection. These prove safe lookup and
numeric preservation; native visual purchase/drive QA of the corrected executable remains a separate check.
