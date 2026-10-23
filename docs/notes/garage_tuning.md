# Garage tuning: extended ride height / rims / tires, player-only tuning

2026-10-21. Code: `templates/game/src/game/garage.cpp` (new), `templates/game/src/game/tuning.cpp` (changed).
Setting: `[tuning] garage_extended` (bool, default true; lead wired app.h/settings.cpp/F1 "Extended garage choices").
All addresses are default.xbe guest VAs. Disassembly helpers used: scratchpad `garage/gd.py` (`<va> [n]`, `-f <va>`
function, `-c <va>` callers, `-r <va>` operand refs), `garage/ptrs.py <va> <n>` (dump dwords as ptr/string/float).

## 1. Data model

### CarCfg (the save; one per owned car)
Field registration 0x270F00.. via parser 0x32F3D0(type, name, ptr, 1, 0) (type 3 byte, 6 float):

| off | field | meaning |
|---|---|---|
| +0x08 | dirty flags | Set() ORs 0x80 ride, 0x81 rim/profile, 0x01 width |
| +0x0C/+0x10 | Camber0/1 | float |
| +0x14/+0x15 | TireOffset0/1 | byte |
| +0x16/+0x17 | RideHeight0/1 | byte = index in the ride height name list (0..14 stock) |
| +0x18/+0x19 | TireWidth0/1 | byte = index (0..8 = 195..355 mm) |
| +0x1A/+0x1B | RimSize0/1 | byte = inches (12..28); list index + 12 |
| +0x1C/+0x1D | TireProfile0/1 | signed byte, index 0..7, -1 = auto (0x270AF0) |
| +0x20.. | RimMdlIdx, TireMdlIdx, ... | |

Stock data (all 1087 tune/vehicle/customdata/*.carcfg): RideHeight 0..14 (raised 9..14 used by a few), TireWidth 0..8,
RimSize 12..28, TireProfile 1..7.

### CarCfg methods
- `Set(type, axle, index, flag)` 0x272020 (thiscall, ret 0x10; jump table 0x2725EC indexed type-1):
  0x18 rim: `[cfg+0x1A+axle] = index+12`; 0x19 profile: `[+0x1C+axle] = index`; 0x1A width: `[+0x18+axle] = index`;
  0x1F ride: `[+0x16+axle] = index`.
- `GetName(type, axle, flag)` 0x272A20 (jump table 0x272E9C): returns `db_list.names[byte]` (ride: 0x272CAF).
- `SetByName` 0x2730B0(type, name, axle, flag): index = db lookup 0x273EE0(type, name), then Set.
- `GetRideHeight(axle)` **0x270BE0**: `fld [0x471A90 + 4*cfg[0x16+axle]]` (thiscall, ret 4, x87 result).
- `GetTireWidth(axle)` **0x272FC0**: `fld [0x4B3918 + 4*cfg[0x18+axle]]`. Model visual 0x270C00(index) cdecl same table.
- `GetTireDiameter(axle, x)` 0x272FE0: `T[rim]/P[profile]` with T = 0x4B3910 indexed by rim inches
  (T[12..28] = 0.381..0.7874 m = rim+3 in), P = 0x471958 (8 ratios 0.4155..0.83). Rims >28 have no table entry.

### Tables (static, .rdata)
- 0x471A54 char*[15] ride names: rh_0, rh_100..rh_800, rh+600, rh+500 .. rh+100.
- 0x471A90 float[15] ride metres: 0, -0.025, -0.05, -0.075, -0.1, -0.1125, -0.125, -0.1375, -0.15, +0.125, +0.1125,
  +0.1, +0.075, +0.05, +0.025.
- 0x471ADC char*[9] tw_195..tw_355; 0x4B3918 float[9] 0.195..0.355.
- 0x4719C8 char*[17] rim_12..rim_28. 0x471938 char*[8] Profile_0..Profile_7 (labels "8".."1").
- Labels exist in fonts/mcstrings for rh_0..rh_800 ("Stock", 1"..8"), rim_12..28, tw_*, Profile_*; **not** for rh+*.

### Car custom database `[0x557F04]` (0x188 bytes, ctor 0x275030, files 0x275EF0, names 0x270C10)
Lookup by type 0x273EE0(type, name): per-type list {char** names; u16 count; u16 cap}. Offsets (type → list):
0x18 rim → +0xE8 (adder 0x274940), 0x19 profile → +0xF0 (0x274980), 0x1A width → +0xF8 (0x2749C0),
0x1F ride → +0x100 (0x274A80). Adders are thiscall(db, const char*) and strdup (0x20CFD0). 0x270C10 is called at
start from 0x29421 / 0x151097 (may run twice on the same db; the game then duplicates its names, lookup returns the
first match).

### Physics/visual application
- vehSim is a per-car instance: car component ctor 0x0FF160 allocates it (0xE0 bytes, 0x121550) at car+0x50.
- **vehSim::ApplyCarCfg(cfg, model) 0x121970** (thiscall, ret 8), called from car vtable 0x454F40 methods
  0x0FFC50/0x0FFD90 (spawn/reset) with `ecx=[car+0x50]`, and from 0x100592 with `ecx=[[car+8]+0xB4]`. For each wheel
  block ([sim+0x54] front, [sim+0x58] rear): +0x04 = TireDiameter*0.5, +0x08 = GetTireWidth, **+0x48 SuspensionOffset
  = GetRideHeight**. Right after, 0x0FF220(model) sets up the physics. So the garage step *overwrites*
  SuspensionOffset; tuning.cpp's old per-frame offset was retired at the first spawn (value mismatch) — fixed (§3).

### Garage UI (Flash + native)
- Garage object (this of 0x1465E0/0x145DC0/0x14A3D0): +0x18 Flash wrapper, +0x3E4 names-array variable name (char[]),
  +0x416 status-array variable name, +0x9CC current part type, +0x71C+4*[+0x72C] current CarCfg.
- Menu id → array names / type: 0x13E8D0(menu, &type, names, status, &flag), jump table 0x140140:
  0x28 RimSizeNames/Status type 0x18; 0x29 TiresProfile 0x19; 0x2A TiresWidth 0x1A; 0x2B Airbag 0x24;
  0x2C Hydraulics 0x25; 0x2D RideHeightNames 0x1F; 0x2E RideHeightRearNames 0x1F.
  Category names list 0x14F8A0 ("Ride Height" 0x45B63C, "Ride Height Rear" 0x45B628).
- The option **names are ActionScript arrays in the per-car movie** `flash/vs_<car>.xbck` (constant pool shows
  e.g. 350z: RimSizeNames rim_20..rim_15, TiresProfileNames Profile_2..7, TiresWidthNames tw_355..tw_195,
  RideHeightFrontNames rh_800..rh_0). Survey of all vs_*.xbck: every car ride rh_800..rh_0 (no raise), rims a 4..12
  slice of 12..28, profiles 2..7 (one car 0..7), widths all 9; 15 bikes have none.
- Status fill 0x1465E0 (thiscall, ret 4; called from garage update 0x1519D0): count = length of the *status* array,
  for each item sets status "1"/"2"/"5"/"6" via 0x139540; uses IsOptionAvailable.
- **IsOptionAvailable(item, axle) 0x145DC0** (ret 8, al): for 0x18/0x19/0x1F runs fit check model 0xEF590 →
  0x270B60(profile, rim, ride, clearance+ground, suspLimit, tireDiamLimit/2) on vehModel ([model+0x1800]+0x300);
  plus |rim front-rear| ≤ 2 sizes, |ride front-rear| ≤ 5 steps. Help texts WRN_RIMSIZE/WRN_TIREPROFILE/WRN_RIDEHEIGHT
  0x149C98...
- Selection apply 0x14A3D0 → 0x2730B0(type, name, axle, 0).
- Description key "%s_desc" 0x45B094 (0x14CFA5) → text via 0x13C5D0 → `0xEA750(table 0x507E08, key)`; entry+8 is the
  UTF-16 text. A missing key (retail mode [0x4B1158]=1) is inserted with its own name as text.

### Flash VM internals used
- Wrapper fns (thiscall on wrapper): length 0x1395F0(name) → int/-1 (requires value type 5 object); get element
  0x139650(name, idx, value*) → al; set string element 0x139540(name, idx, char*) (refuses idx ≥ length);
  set int element 0x139590. Value = {u32 payload, u32 type}: 0/1 string (1 = raw char*, never copied or freed),
  2 bool, 3 int, 4 float, 5 object, 7 undefined.
- VM = [wrapper+8]. Variable lookup 0x64060(name) → value*. Arrays are plain objects: members "0".."127" (names at
  0x44C4C0, 4 bytes each; member nodes store the name pointer, so names must be static) and "length" (type 3),
  see Array ctor 0x64F00. Object vtable +0x20 get_member(name, value*), +0x24 set_member(name, value*) → 0x65EB0 →
  0x65D70 (existing member: plain copy; new member: node from the **symbol pool** at `[0x5014D8]`).
- **VM context globals**: 0x641C0/0x64060 set `[0x5014D0]=vm+0x10, [0x5014D8]=vm+0x1C, [0x5014D4]=vm+0x28,
  [0x501410]=vm+0x34, [0x501420]=[vm+0x40], [0x50142C]=[vm+0x44]` and zero them afterwards. Outside those calls they
  are 0. Pool = vm+0x1C {u16 count +6, free list +8, nodes 0x10 linked through +0}.

## 2. First build crash (user: clicking Rim Size / Ride Height crashed)
Dumps mc3.2026.10.21_15.36.48 / 15.37.53: `sub_001465E0 hook → ExtendList → Lengthen → CallGuest → set_member
0x65EB0 → 0x65D70` AV. Registers: name ebx=0x44C4E4 ("9"), value "2" (0x44B5E4): creating status element 9 of a
9-element array. 0x65D70 needs the symbol pool `[0x5014D8]`, which is 0 outside the VM's own calls. ("length" had
worked because it already exists; the names rewrite had worked because 0x139540 → 0x641C0 builds the context itself.)

Fix (garage.cpp): `VmContext` sets the six globals from the VM exactly like 0x641C0 around every direct set_member,
and restores the previous values (not zero) in case it ever runs inside the player. Before touching anything,
ExtendList now checks: names list known (every element one of the expected names), status array exists, VM object
plausible ([vm+0x40]/[vm+0x44] valid), and the pool's free list has every node the extension will create + 16;
otherwise nothing is changed ("garage: ... symbol pool is nearly full" logged up to 4 times). All garage log lines are
LOGR so they appear in mc3.log.

## 3. What the code does now

### garage.cpp hooks
| hook | what |
|---|---|
| 0x270C10 (db names) | after the game: allocate a 4 KB guest arena (operator new 0x1B3B90) once; append 14 ride names (indices 15..28: rh_900..rh_1600, rh+700..rh+1200) and 7 width names (9..15: tw_375..tw_455, tw_175, tw_155) **only if** the list holds exactly the stock count, then verify. Re-runs detect the names already at 15/9. |
| 0x270BE0 GetRideHeight | index ≥15: -0.15-0.025k (to -0.35 m) / +0.125+0.025k (to +0.275 m); else original |
| 0x272FC0 GetTireWidth, 0x270C00 | index ≥9: 0.375..0.455, 0.175, 0.155 m; else original |
| 0x145DC0 IsOptionAvailable | garage_extended: rim/profile/width/ride items always available (no fit, no front/rear diff) |
| 0x1465E0 status fill | garage_extended: rewrite the current list to every step (ride 29: 16" low .. 12" high; rims 17: 28..12; profiles 8; widths 16), lengthen ...Status (status "2") and ...Price (copy element 0; owned strings → int 0); inject labels (rh_900.. "9\"", "Lower Nine inches", rh+N00 "+N\"", tw_NNN "NNN mm") into the current string table (only entries that fell back to their own key) |
| 0x121970 ApplyCarCfg | after: remember cfg→sim; if cfg is a player's, mark sim as player; `TuningCarCfgApplied(sim)` |
| 0x2CBDE0 mcPlayerFactory make (vtable 0x476940; car+0x5E=0, +0x5D player index) | car=[factory+0x44]: player sim car+0x50, cfg car+0x64 |
| 0x1FAE10 other factory (vtable 0x46C730; car+0x5E=1) | computer car: sim marked non-player |
| 0x0FFBA0 car dtor | forget the car's sims (TuningForgetSim) |

Saves: extended bytes are stored by the game's own CarCfg as usual. The getters stay active even with
garage_extended off, so such a save keeps its height/width. A save with extended bytes loaded by the *original* game
would read past the tables (garbage) — only relevant for exporting saves to an Xbox.

### tuning.cpp
- Tuned/Pending carry `owner` (vehSim for ride fields, 0 for vehModel limits).
- Ride offsets (F1 ride_height_front/rear) apply only to vehSims in the player set; others are written back to their
  original. Wheel size limit (TireDiameterLimit scale) still applies to every vehModel (it only feeds the garage fit
  check, which garage_extended bypasses anyway).
- New API (called by garage.cpp): `TuningSetPlayerSim(sim, bool)`, `TuningForgetSim(sim)`,
  `TuningCarCfgApplied(sim)` (re-track SuspensionOffset after ApplyCarCfg with the garage value as the new original and
  write the offset immediately, before 0x0FF220 sets up physics).
- tests/tuning_test.cpp (lead) registers kSim as a player and tests CarCfgApplied; my standalone run of the earlier
  scratch version passed (scratchpad `garage/tt/tuning_test.cpp`).

## 3b. Label fix (second in-game test: new ride items showed "!!RH_1300!!")
- Lists, selection and physics worked ("RimSizeNames now 17 steps (was 6)", "RideHeightNames now 29 steps (was 9)",
  Golf R32 visibly lower at rh_1300).
- Cause: the string table's missing-key mode is **2** at runtime (`[0x4B1158]` is 1 in the image but set to 2 at
  0x205DA8). In mode 2, 0xEA750 creates the missing entry with text `"!!" + key + "!!"` (0xEA7C0..0xEA867, "!!" at
  0x45428C), not the bare key. PutLabels only replaced entries whose text was exactly the key, so it left the
  placeholder. The upper case is only how the menu font shows it: the log has no upper-case "RH_" lookups, only
  PutLabels' own `rh_1300` / `rh_1300_desc` (caller FFFFFFF0 = CallGuest). The hash 0xEA710 is case-sensitive.
- Fix: PutLabels now treats an entry as a placeholder when its text, with leading/trailing '!' stripped, equals the
  key case-insensitively, and replaces only those. Tire width labels (tw_375 ...) go through the same path.
  No hooks.cpp change is needed: the Flash label reads the same table entry (by hash) that PutLabels rewrites.
- Rim labels like `23""`: original game data. The shipped fonts/mcstrings_x.strtbl stores the UTF-16 text `23""`
  (rim_*, rh_100..rh_800 `8""`; the CSV source `"23"""` was not un-escaped). The new ride labels now use the same
  style (`9""`, `+7""`) so the list looks uniform.

## 4. Unverified / to playtest
1. Garage → Rim Size / Tire Profile / Tire Width / Ride Height (front and rear): no crash; log shows
   `garage: ride heights extended (29 in the database)`, `garage: tire widths extended (16 ...)`,
   `garage: RideHeightNames now 29 steps (was 9)` etc., `garage: player car ...`.
2. Whether the Flash arc shows the added items: it reads `RideHeightNames.length` itself; if it cached the length
   before 0x1465E0 first ran, extra items may only show after re-entering the menu (or not at all). If they never
   show, the next step is to rewrite the arrays earlier (hook 0x13E8D0 return) or the movie's source arrays
   (RideHeightFrontNames etc.).
3. Labels of new items (expected "9\"", "+7\"", "375 mm"); fallback would show the key ("rh_900").
4. Price shown for new items = first item's price.
5. Physics with -0.35 m / +0.275 m offsets and 455 mm tires (body on ground, wheels in arches is the user's choice).
6. F1 ride sliders now only move the player's car (traffic/opponents unchanged).

## 5. Third round (2026-10-22): more tire widths, ground contact / wheel tuck

### Tire widths
Appended (save indices stay stable, append only): index 9..15 = 375, 395, 415, 435, 455, 175, 155 (first build),
16..22 = 135, 125, 475, 495, 515, 535, 555 mm. Table `kExtraWidthsMm` in garage.cpp drives the database names
(tw_NNN), the getters 0x272FC0 / 0x270C00, the labels ("NNN mm", "NNN mm Tire Width") and the garage list (23 widths,
sorted widest first like the movies). Log after start: `garage: tire widths extended (23 in the database)`.

### How the suspension really works (evidence)
- SuspensionOffset (wheel tune +0x48) is NOT read by the per-frame wheel. It only feeds the wheel mount positions:
  car component function 0x103C00 (called from 0x1057B0) computes, per axle,
  `mount_y = min(SuspensionLimit_vehModel - r, r - (GroundClearance + SuspensionClearance)) + SuspensionOffset`
  (r = model tire radius, [[car+0x68]+0x12D4]*0.5; vehModel = [[car+0xC]+0x300]) into [vehSim+0x88] (+4 front,
  +0xC rear). So lowering raises the wheel mounts relative to the body: the body sits lower, the wheels stay on the road.
- Runtime wheel class (methods 0xA6000..0xA8000, tune pointer at wheel+4): compression [wheel+0x144] is limited to
  [-TravelExtent(+0x44), SuspensionLimit(+0x4C)]. Suspension update 0xA7200: above SuspensionLimit the wheel bottoms
  out (0xA73A8): flag [0x5071CD] -> damping impulse from the point velocity (0x341B20), flag [0x5071CC] -> force
  (c - limit) * k through the rigid body vtable +0x2C, wheel+0x166 = 1 ("bottomed"), then c is clamped. Bottomed
  wheels are counted by 0xA34C0 / 0xA36B0 (aero and impulse redistribution) and sent in network state (0x1AE2C0).
- Chassis collision: the physics car (0x121DE0, vehSim at +0xB4) builds its phArchetype from "<car>_bound"
  (0x2A03A0, phBoundGeometry). The vehicle bundles only contain a "@boundbone" bone name, no .bnd text; the bound is
  evidently higher than the visible floor pan (otherwise the lowered body could not pass through the road).
  Body/roof sparks are the vehDamage emitter (tune vp_*.vehdamage: Spark*, RoofSparkVelVar, BottomOutCap; visitor
  0x101AE0) driven by bound contacts; ScrapeSound 0x21CA79, SparkSound 0x19D68D, particle_spark 0x19E76A.

### What changed (tuning.cpp)
New tracked kinds 3/4: each player wheel's SuspensionLimit (bump stop), tracked with the offset in
TuningCarCfgApplied (same TravelLimit/SuspensionExtent fingerprint). Target: when the wheel's effective
SuspensionOffset (garage step + F1 offset) lowers the car more than `kFreeLowering` = 0.15 m (rh_800, the game's own
deepest step), SuspensionLimit = original - excess (floor 0.01 m). Result: the lowest the body can go is where the
stock car bottoms out at rh_800; a slammed car rests on its bump stops (bottom-out force) instead of sinking through
the road, and the wheels tuck into the arches. Stock range (rh_0..rh_800) and raised heights are unchanged;
computer cars are never touched. Scratch test `garage/tt/stop_test.cpp` (scratchpad) checks the numbers.

### Not achieved yet: real body/road contact sparks
Sparks/scrape need the chassis bound to reach the road. The bump-stop floor keeps the body from passing through but
does not by itself make the bound touch, so sparks appear only if the bound bottom is at the stock bottom-out height.
Next step needs runtime data: the new log line
`garage: player vehSim XXXXXXXX ride F/R: front offset .. stop .. travel .. ext .., rear ..., size y .., model offset y ..`
(written whenever a player's ApplyCarCfg result changes) plus a frame capture at rh_1600; then either lower the
bound (archetype bound vertices / instance offset) by the excess lowering or tune kFreeLowering per car.

## 6. Fourth round: wheels through the garage floor (bump-stop build) -> widened travel

Lead's screenshot (Golf, 16" lower, garage): the body dropped and the wheels were drawn through the floor.

Cause: the previous build *lowered* SuspensionLimit by the extra lowering (0.24 -> 0.04 m at 16"). That is below the
wheel's static compression, and the wheel suspension 0xA7200 clamps compression at SuspensionLimit and draws the
wheel there. So the wheel was held "extended" below the road while the body settled lower. The bottom-out push did
not hold the body up (it is gated by [0x5071CC]/[0x5071CD]). The garage preview runs the same path: GetRideHeight
0x270BE0 has a single caller, ApplyCarCfg 0x121970, so the preview car is a physics car like in races.

Fix (tuning.cpp, kinds 3..6): beyond kFreeLowering (0.15 m, rh_800) the player's wheel travel is widened by the
extra lowering, both ways:
- SuspensionLimit (+0x4C) += excess: the wheel can compress further, up into the arch.
- TravelExtent (+0x44) += excess: the wheel can extend further, staying on the road while the body rests on its
  chassis bound.

Lowering then brings the chassis and its rigid bound down to the road. Contact goes through the damage handler
0x1027D0 (sliding contacts -> SparkEmitMod sparks + scrape sound 0x10B130; see customization_system.md section 9).
The wheels are never clamped below the road. Stock steps (rh_0..rh_800), raised steps and computer cars are
unchanged. Scratch test `garage/tt/stop_test.cpp` checks the numbers. tests/tuning_test.cpp still passes.

Remaining risk: if a car's bound bottom is higher than its visible floor pan, the pan can still dip a few cm into the
road at 13"-16" before the bound touches. The log line `garage: player vehSim ...` shows offset, stop and ext per
axle, so the lead can check.

## 7. Codex deep pass (2026-10-22, current work)

- Width choices now total **38, spanning 105–755 mm**. Existing database/save indices 0–22 keep their previous
  meanings. Appended indices 23–37 are 105, 115, 145, 165, 185, 575, 595, 615, 635, 655, 675, 695, 715, 735,
  755 mm; the Flash menu sorts them by physical width. Rims remain capped at the verified 12–28 inch table.
- The permanent guest string arena is 8 KiB: the expanded key/UTF-16 label/description set no longer fits the
  earlier 4 KiB budget. This is allocated once, rather than allocating symbols/strings on every menu visit.
- `garage_test` now models the VM context and consumes actual free-list nodes in its bounded fixture. It checks
  all six context globals during direct set-member and restoration afterward, successful width list growth,
  stable save indices and refusal to mutate any list when the pool lacks the full requested growth plus reserve.
  It executes the production hooks; the fixture replaces the original Flash VM, so this does not prove every
  vehicle movie has room. Runtime exhaustion still safely leaves that vehicle's original list in place.
- Fixed a rebase ambiguity in `TuningCarCfgApplied`: the original 0x121970 unconditionally writes both garage
  offsets (0x1219CE / 0x121A1C). A new garage step can equal the previous F1-adjusted value by coincidence. It must
  still become a fresh baseline. The ordinary parser unchanged-value shortcut no longer suppresses that rebase.
- `tuning_test` adds this coincident-value transition plus deep-lowering travel/extension, repeated-frame,
  raised-axle restoration and player-to-computer restoration checks. Ground intersection still requires a live
  physical/visual check; numerical travel coverage is not evidence that a collision bound matches the floor pan.

Corrections to older address notes, checked against the generated instruction bodies: the mount formula is at
0x105165..0x105208 inside **0x103C50**, a component blending/setup routine. **0x103C00 is disposal**, reached from
0x1057B0, not the mount calculation. Wheel compression is clamped by **0xA7250** (not 0xA7200); its upper clamp is
0xA74E4..0xA74ED and extension clamp is 0xA74B9..0xA74CE. 0xA7200 updates spring coefficients through 0xA6090.
The previous §5 statement that the mount formula runs in 0x103C00 was incorrect. This distinction matters before
adding new hooks: hooking disposal as though it were setup would edit freed objects.

### Direct-to-garage preview ownership and contact diagnosis

The garage owns two preview slots: car pointers at +0x714/+0x718, CarCfgs at +0x71C/+0x720 and selected slot at
+0x72C. Its creation path is 0x13C8E0, calling 0x2CBAE0 and storing the returned car at 0x13C972. A player factory
hook alone does not establish ownership of every direct-to-garage preview. The status-pass hook now identifies a
previously unowned selected garage car as a player preview, then applies tuning immediately. A concrete known
computer owner remains excluded. Destructor retirement uses the existing car-owner path. The regression tests
direct-to-garage registration, repeated refresh without duplicate rebasing, and refusal to promote a known AI car.

`tuning.cpp` also wraps the original **0xA7250** wheel solver without modifying its input/output. For extended-low
player wheels it samples `garage: wheel contact sim ... requested ... actual ... offset ... stop ... extension ...`
at most six times per wheel/setting, two seconds apart at 60 Hz. `contact` is the solver's original second argument.
Requested compression versus the result at wheel+0x144 establishes whether the wheel is being clamped; this does
not report the rendered tire's world-space bottom. Sampling records retire with the owning vehSim.

Validation: production `garage_test` and `tuning_test` targets built with Clang and passed (2/2). Native integration
and in-game floor/spark checks are owned by the lead and remain pending at this checkpoint.

Stronger solver regression: when generated guest sources are available, `tuning_solver_fixture.cpp` executes the
**actual generated original 0xA7250 body** under bounded memory. With player limits widened to 0.44 m compression /
0.38 m extension, ground requests +0.35 and -0.30 m survive unchanged. Once ownership is removed, restored stock
0.24/0.18 limits clamp those same requests to +0.24/-0.18. Airborne behavior still chooses full extension. The
fixture explicitly disables bottom-out impulse flags and fails if an unexpected rigid-body impulse path runs;
it does not fake successful chassis contact or sparks. Both targeted checks passed after adding this execution.

### Rendered wheels forced through the floor: separate arch-fit correction

Live evidence from the lead's 16-inch rear drop, captured at `.local/autonomous-captures/235154`, showed the rear
tire visibly cut by the garage floor while 0xA7250 reported contact=1, requested≈actual≈0.001 m. The player wheel's
limits were already expanded (rear offset -0.350, stop 0.430, extension 0.380). Further travel expansion cannot fix
an unclamped wheel.

The original wheel renderer has a **second, cosmetic fit correction** at 0xF7495..0xF7596. For normal tires it
queries 0xA65D0 at return address **0xF7525** and computes:

`correction = min(0, vehModel.SuspensionLimit[axle] - query - runtimeWheel.radius)`

`query = clamp(wheel.compression, -TravelExtent, TravelLimit) - cachedSuspensionOffset`

When negative, 0xF7543..0xF7572 adds that correction along the wheel's up vector to its render translation. At
extreme tuck this pushes the visible wheel **downward**, even though the ground contact solver placed it correctly.
A special tire branch at return address **0xF74D5** applies a symmetric ±SuspensionLimit envelope.

`tuning.cpp` now scopes a visual override to exactly those two call sites, owned player wheels and offsets below
-0.15 m. It executes original 0xA65D0 first, then supplies a query that makes only this cosmetic fit correction zero
(normal: limit-radius-0.00001; special: 0). Physical compression, tire radius, original shared vehModel data and all
other callers remain unchanged. This permits intentional fender intersection while preserving the grounded wheel
translation. First applications log `garage: tucked wheel ... keeps contact position` with actual radius/limit.

The regression executes original generated 0xA65D0: a player wheel with compression 0.001 and cached offset -0.350
returns the unmodified 0.351 to physical callers, 0.04999 to the normal visual fit caller with limit0.4/radius0.35,
and 0 to the special fit caller. Removing ownership restores the original 0.351 for the renderer too. Physical
compression is unchanged. Targeted tuning/garage tests passed. The lead is validating the rebuilt native executable
against the same garage view and an isolated QA save (`--instance 2`) before any claim of a complete ground fix.
