# MC3 tuning and customization system: full map

2026-10-21. This is research only: no code was changed and the game was not launched. Addresses are guest VAs in
`default.xbe`. "Evidence" means the address of the instruction or function that proves the claim. Anything not
proven is marked **(guess)**. Function names such as `CarCfg::Set` are ours.

Sections:
- §0 garage list-extension crash and the Flash VM.
- §1 data files.
- §2 custom DB and part type ids.
- §3 CarCfg.
- §4 Garage class.
- §5 selection to physics and visuals.
- §6 upgrades.
- §7 saves, network and AI.
- **Priority sections:** §8 tire width, §9 collision bound and scrape sparks, §10 tuck vs ride height, §11 neon,
  §12 vinyls/decals/tint, §13 paint shading.
- §14 extension checklist.
- §15 open leads.

How this was produced:
- Ghidra 12.1.4 through the REA MCP tools. `default.xbe` was rebuilt as a flat PE with image base 0x10000 and one
  RWX section covering 0x11000..0x58A270. That image is `scratchpad/cust/mc3flat.exe` (built from the XBE section
  table). Raw decompiles are in `scratchpad/cust/dec/<va>.c`. **Pitfall:** when Ghidra has no function at an
  address, REA silently decompiles the function at *file offset* = that address, which is VA+0x10000. The
  pseudocode's `FUN_` name will not match the address you asked for (e.g. asking for 0xFB760 returns
  FUN_0010b730). Always check the name. This happened for 0x101AE0, 0x1297F0, 0x129D10, 0x1B7250, 0x2CE650,
  0x327670, 0xF1950, 0xF20F0, 0xFA800, 0xFB760 and 0xFFC50. Those were read with capstone instead.
- Capstone helpers in `scratchpad/cust/` (copies, so other agents' tools are untouched): `d.py` (disassembly,
  callers `-c`, operand refs `-r`, function `-f`), `s.py` (string at VA), `p.py` (dword/float/string table dump),
  `r.py` (data refs to a VA), `fields.py <fn>` (lists every tune-file field a visitor registers through 0x32F3D0:
  type, name and destination offset).
- Tune files were extracted with `tools/texture_pack/dave.py` to `scratchpad/cust/x/tune/...`. One car's resource
  bundles are in `scratchpad/cust/car/resources/vehicle/vp_350z_04/`.

---

## 0. The garage list-extension crash (sub_001465E0 -> ExtendList -> Lengthen)

**Status (coordinator, 2026-10-21):** fixed. The cause was the Flash VM context globals at 0x5014D0.. being valid
only inside the player's own calls. The analysis below confirms that cause from the dumps and records the related
hazards that remain.

### 0.1 What the dumps show
`%LOCALAPPDATA%\CrashDumps\mc3.exe.5992.dmp` (15:36) and `mc3.exe.16148.dmp` (15:37) both fault at host
`mc3.exe+0x256794`, inside the code lldb labels `__imp__sub_00065BD0`. That is the generated body of guest
0x65D70, inlined by the compiler. The host instructions at the fault:
```
mov eax,[rdi+0x5014d8]   ; guest: mov ecx,[0x5014D8]   (current symbol pool)
lea edx,[rax+8]
mov ecx,[rdi+rdx]        ; guest: mov eax,[ecx+8]       <- faults: [0x5014D8] == 0
```
Guest registers at the fault: eax=ecx=esi=0; ebx=edx=0x44C4E4, which is the string "9" in the index-name table;
edi=0x8358E164, the member-list head of the object at 0x8358E160. So the crash happened while creating array
element "9" outside any VM context. Element 9 is the first element past the stock 9-entry ride-height and
tire-width lists. Both dumps predate the VmContext build (garage.cpp 16:35, exe 16:36). No dump exists after 16:36.

### 0.2 How the Flash VM really works (needed by anyone touching Flash arrays)
The player is Rockstar's own SWF runtime (strings "swfOBJECT::Fixup", "swfCMD::Fixup" at 0x44CBD4/0x44CD88).
The per-car garage movies `flash/vs_<car>.xbck`, `ds_`, `ps_` and `ms_` hold baked SWF data. The C++ side loads
`vs_%s.swf` (0x45B248, ref 0x14EC37) and the others.

VM object. The constructor is 0x64550(movie, nInstances, nSymbols, nObjects, stringBytes), thiscall, ret 0x14.

| VM offset | What | Global set during a call | Node size | Pool init |
|---|---|---|---|---|
| +0x08 | root instance ("_root"), vtable 0x44C700 | — | — | — |
| +0x10 | **instance** pool {base, u16 cap +4, u16 free +6, freelist +8} | 0x5014D0 | 0x80 | 0x643F0 |
| +0x1C | **symbol** pool (member nodes) | 0x5014D8 | 0x10 | 0x64440 |
| +0x28 | **object** pool (objects and arrays) | 0x5014D4 | 0x08 | 0x64490 |
| +0x34 | **string** pool (GC'd chunks of 12 chars plus next) | 0x501410 | 0x10 | 0x64440 (bytes/12 nodes) |
| +0x40 | locals/registers member list head (4-byte new) | 0x501420 | — | — |
| +0x44 | `_global` object (vtable 0x44C464, members at +4) | 0x50142C | — | — |

Pool sizes are fixed by the caller at 0x139EE0/0x139FF6:

| Condition | Instances | Symbols | Objects | String bytes (chunks) |
|---|---|---|---|---|
| game state `[0x53F2D4] == 5` (evidence 0x139EED `cmp edx,5; sete bl`) | 400 | **4000** | 250 | 5000 (416) |
| otherwise | 500 | **410** | 205 | 5000 (416) |

The same sizes appear at 0x13A03F/0x13A080. Which mode the garage VM uses was not proven. The garage.cpp
`PoolFree` check is the right guard either way.

Value = {u32 payload, u32 type}. Types come from the to-number switch at 0x63C80 and the jump table at 0x63CB8:

| Type | Meaning |
|---|---|
| 0 | GC string (payload = first 16-byte chunk in the string pool) |
| 1 | constant `char*`, not GC'd |
| 2 | bool (byte) |
| 3 | int |
| 4 | float |
| 5 | object/instance pointer |
| 7 | undefined (the game's initialiser) |

Symbol node = {+0 name `char*` (stored, **not copied**), +4 payload, +8 type, +0xC next}. 0x64C90 searches a list
by name. 0x65D70 inserts: it finds the name, otherwise pops a node from `[0x5014D8]`. When the freelist is empty
it calls 0x1A28F0 = **Quitf** ("FATAL: %s", "%s pool is full", "symbol"). Quitf spins forever (0x1A2924 `jmp $`),
so the game **hangs** rather than crashing. With `[0x5014D8]==0` it **crashes** (the dumps above).

Object vtable 0x44C464: +0x18 mark=0x65240, +0x20 GetMember=0x64DC0 (falls back to `[0x501420]` and then
`_global`), +0x24 SetMember=0x65EB0 (-> 0x65D70 on obj+4). Instance vtable 0x44C700: +0x18 mark=0x65260,
+0x24 SetMember=0x66250 (handles `_x`/`_y`/`_xscale`/...), +0x2C CallMethod=0x65BD0 (gotoAndPlay/Stop, play,
stop, nextFrame, prevFrame).

Every C++ entry point that touches the VM sets **and then clears** all six globals itself:

| Function | What it does |
|---|---|
| 0x64060 | find variable (calls 0x65160) |
| 0x64150 | get element |
| 0x641C0 | set element, **only if index < length** (0x64235 `cmp; jl`) |
| 0x64300 | set variable (inferred) |

The Flash wrapper (garage+0x18; wrapper+8 = VM) has these methods:

| Function | Method |
|---|---|
| 0x1395F0 | Length(name), -1 if not an object |
| 0x139650 | Get(name, i, value*) |
| 0x139540 | SetString(name, i, `char*`) = type 1 |
| 0x139590 | SetInt = type 3 |
| 0x139520 | set var |
| 0x139690 | get var as int |

**Any other code that calls a vtable SetMember directly must establish the six globals first and restore them
afterwards.** That is the fix that landed (VmContext).

Index names: element names are taken from the static table 0x44C4C0 ("0".."127", 4 bytes each; 0x44C6C0 =
"this"). Hard limit: **an array touched from C++ can have at most 128 elements.** Index 128 and up would use
"this", "dName" and so on as member names.

GC: 0x64BC0 (called from Flash::Draw 0x139730 when its arg is true, at 0x1397C1) is mark-and-sweep over the
**string pool only**:
1. 0x644E0 allocates a mark byte per chunk and marks the free chunks.
2. Marking starts at the `[vm+0x40]` list (0x65220 -> 0x63BF0 per value), `_global` (vtbl+0x18 = 0x65240) and
   the root instance tree (0x65260: sibling +0x78, child +0x74, +0x64 object, name chain +0x48 via 0x63BD0).
3. 0x64B40 sweeps: unmarked chunks go back on the freelist, marked chunks have bit 0 of next (+0xC) cleared.

0x63BF0 marks only **type-0** values. **Objects/arrays referenced through type-5 values are not traversed.** A
type-0 (GC) string stored only inside an array is freed by the next GC and leaves a dangling chunk pointer. The
stock garage only ever writes type 1 into arrays (0x139540), and garage.cpp also writes only type 1/3, so this is
safe today. **Never store type-0 strings in arrays.** A GC crash can only come from a type-0 value with a bad
payload or from a freelist node outside its pool (0x644E0 would write marks out of bounds).

### 0.3 Remaining hazards for the list extension (current garage.cpp)
1. **Front ride-height list name.** The movie defines `RideHeightFrontNames` (rh_800..rh_0, 9 entries),
   `RideHeightRearNames` and `RideHeightNames`. The C++ category switch 0x13E8D0 uses `RideHeightNames` for
   category 0x2D (front) and `RideHeightRearNames` for 0x2E. `RideHeightFrontNames` is never referenced by the
   XBE, so the movie probably copies it into `RideHeightNames` **(guess)**. `ListFor()` correctly matches the
   names the C++ uses.
2. **Unlocked fraction.** The status pass 0x1465E0 computes `unlocked = ftol(fraction * statusLength)`. The
   fraction comes from 0x146040 (progress rewards Rims0..8, vinyls, riders, plates...). Items at index >= unlocked
   get status "5" (locked) unless `[0x53F2F2]` is set. Lengthening a list therefore moves the lock boundary. Ride
   height, width, profile and rim size return 1.0 (all unlocked) and are then forced to "1"/"6", so they are not
   affected. Rims (type 8) **are** affected if rim lists are ever extended.
3. **Status codes** (strings): "1"=0x44B600 (neutral/selectable), "2"=0x44B5E4 (buyable), "3"=0x459E98 (owned,
   0x273360 bitfield), "4"=0x459EA4 (installed), "5"=0x4582F8 (locked), "6"=0x459FD8 (unavailable/doesn't fit).
   For types 0x18/0x19/0x1A/0x1F every item is first set to "1", then to "6" if IsOptionAvailable fails. For
   0x1F it is also set to "6" when `CarCfg::State(2,0)` (SuspensionState0, +0x5F) is 0: **ride height needs the
   first suspension upgrade** (0x146827). The same check at 0x151A54 greys entry 3 of the parent list.
4. **Fit check uses tables directly.** 0x270B60 reads ride metres from 0x471A90[idx] (not the hooked getter
   0x270BE0), rim diameter from 0x4B3910[byte] and profile from 0x471958[idx]. Extended ride indices 15..18 read
   0.0, and 19+ read the `tw_*` name pointers as floats (~6.5e-39 = 0). The check is harmless while
   IsOptionAvailable is forced true, but wrong if that override is ever removed.
5. **Rear-dimension coupling** (garage+0x1B4 == 0x27, "Rear Dimensions"): stock 0x145DC0 rejects a rim more than
   2 **indices** from the other axle and a ride height more than 5 **indices** from the other axle. Ride-height
   index order is not monotonic (0..8 lowered, 9..14 raised descending). "Within 5" in index space is physically
   meaningless and even rejects rh_0 vs rh+100 (indices 0 and 14). New indices (15..28) make it worse. Keep the
   override, or compare metres.
6. **Labels.** List labels are UI string-table keys (0x0EA750 lookup). garage.cpp's PutLabels writes its own
   UTF-16 pointer into the table entry +8. That is fine while the table lives, but it must be redone when
   `[0x507E08+0x28]` (language/table index) changes. It already compares the table pointer.
7. **Price arrays** (`...Price`): the C++ never reads them (no `RimSizePrice` etc. string in the XBE). Only the
   movie reads them. Copying element 0 is safe. Leaving them shorter is probably also safe **(guess)**: the movie
   may show "undefined".

---

## 1. Data files

All of these are in the base archive `assets_x.dat` (Dave), path prefix `tune/`.

| File | Parsed by | Purpose |
|---|---|---|
| `vehicle/<car>_base.vehsim` / `<car>_mods.vehsim` | vehSim visitor 0xA3CA0 | stock physics and fully-upgraded physics. Upgrades **lerp** between the two (§6) |
| `vehicle/<car>.vehmodel` | 0xFB760 | garage fit limits (§5.2) |
| `vehicle/<car>.vehmods` | 0x129330/0x1294A0/0x1295F0/0x129770/0x129D10 (vehMods, name fn 0x1297F0) | how much each upgrade part contributes (§6) |
| `vehicle/<car>.vehdamage` | 0x101AE0 region | damage thresholds plus **spark** look and `BottomOutCap` (§9.2) |
| `vehicle/customdata/<car>[_N\|_dub].carcfg` | CarCfg visitor 0x270E80 | stock/AI/rival configurations (§3). 1,086 files |
| `vehicle/customdata/<car>/default.mccarcustom` (+`<car>.damage`) | 0xF4430 (`mcCarCustom` 0x454BC4) | brake rotors, xenon headlights |
| `vehicle/customdata/customwheels.txt` | DB loader 0x275EF0 via 0x2742E0 | lists `tires`, `biketires`, `rims` (model names), `brakes` |
| `vehicle/customdata/customexhausts.txt` | 0x275EF0 | `exhausts` -> db+0x48 |
| `vehicle/customdata/customplates.txt` | 0xF1260 (`plates`, `customPlates`) | 81 plate styles {background, font, textoffset, numletters} |
| `vehicle/customdata/customriders.txt` | 0x274420 | `riders` name count -> "name_d" entries in db+0x50 |
| `vehicle/customdata/decaltextures.txt`, `logotextures.txt`, `windowtextures.txt`, `shared_vehicle_textures.txt` | 0x275EF0 | decal/vinyl texture name lists (7 categories in decaltextures: table at 0x4B39BC; category 6 is not stored) |
| `vehicle/customdata/paintjobs/paintjoblist.txt` + `*.mccarpaintjob` | 0x275EF0 + visitor 0xF2110 | paint presets per paint type (§10) |
| `vehicle/customdata/vinylcolors/*.mcvinylpreset` | `mcVinylPreset` visitor 0xF1950 | default vinyl colours per vinyl |
| `vehicle/customdata/colorlibs/*.clib` (calipercolors, chromecolors, neoncolors, opaquecolors, wheelinserts, windowcolors) | colour DB `[0x509E1C]` | 16-byte RGBA entries, indexed by the CarCfg colour bytes (§3.3) |
| `garage/<car>.mcgarage` | `mcGarage` 0x45B880 (0x151691) | garage camera look/eye points per part |
| `garage/garage.garageshared` | — | garage position per city, light, FOV |

The `.vehmodel` format is in HANDOFF §4.3: vehModel +0x0C/+0x10 SuspensionLimitF/R, +0x14/+0x18
TireDiameterLimitF/R, +0x1C/+0x20 TireExtentF/R, +0x24/+0x28 SuspensionClearanceF/R, +0x2C TireSteerLimit,
+0x30 GroundClearance. The garage reaches it as `[[carRender+0x1800]+0x300]` (0xEF590).

---

## 2. Car custom database `[0x557F04]`

Filled at boot. 0x270C10 is called from 0x29421 and from 0x151097 (garage rebuild). It loads the static name
tables, and the text files are added by 0x275EF0/0x274420 (`$/tune/vehicle/customdata`). Each list is
`{char** names; u16 count; u16 cap}` (8 bytes) and **grows on demand** (0x14B910 adds 8 to the capacity). Names
are copied by 0x20CFD0. So **database lists have no fixed limit**; the limits are the CarCfg byte/short indices
and the per-value tables in the XBE.

Lookups:
- 0x273EE0 `db.Index(type, name)` returns -1 for name==0 or type>0x33. It builds a type->list table on the stack
  and calls 0x145230 (strcmp scan). Fallback forms exist for types 10 (side exhaust: index + count(db+0x48)),
  0x2C and 7.
- 0x273E60 `db.PresetIndex(paintType, name)`.
- 0x272A20 `CarCfg::GetName(type, axle, bike)` maps back from the CarCfg byte to a name.

### 2.1 Part type ids (garage+0x9CC; arg 1 of CarCfg::Set 0x272020 / GetName 0x272A20 / db.Index 0x273EE0)

| Type | Part | CarCfg field (offset) | DB list | Dirty bit (CarCfg+8) | Garage category (0x13E8D0) |
|---|---|---|---|---|---|
| 1 | front/rear logos (custom decals) | Logos[0..1] u16 +0x78+axle*2, clears UseCustomLogos +0x65+axle | +0x08 | 0x20 | 0x80/0x81 window decal cats |
| 2 | side/rear badges | Logos[2..3] u16 +0x7C+axle*2 | +0x10 | 0x20 | 0x15..0x1E decal cats, 0x61/0x62 badges |
| 3 | custom window font | CustomWindowFont +0x96 | +0x18 | 0x20 | 0x7A |
| 4 | paint type | PaintType +0x3F | +0x120 (Gloss, Metallic, ColorShift, Pearlescent, Matte, Enamel) | 0x100 | 0x0E |
| 5 | paint preset | PaintPreset +0x40 (-1 = custom colour) | +0x128 = array of lists, one per paint type | 0x100 | 0x30..0x35 |
| 6 | vinyl | Vinyl u16 +0x94 | +0x20 | 0x40 | vinyl cats 0x64..0x6D, 0x82, 0x90, 0x91 |
| 7 | tire model (car / bike) | TireMdlIdx +0x21 / BikeTireMdlIdx +0x22 | +0x28 / +0x30 | 0x04 | 0x24 |
| 8 | rim model | RimMdlIdx +0x20 | +0x38 | 0x04 | 0x23 |
| 9 | brake model | BrakeMdlIdx +0x23 | +0x40 | 0x04 | — |
| 10 | exhaust tips | ExhaustRear +0x33 or ExhaustSide +0x35 (UseSideExhaust +0x34 = index >= count(db+0x48)) | +0x48 / +0xE0 | 0x08 | 0x0D |
| 0x0B | front bumper | +0x24 | +0x68 | 0x02 | 7 |
| 0x0C | rear bumper | +0x25 | +0x70 | 0x02 | 8 |
| 0x0D | side skirt | +0x26 | +0x78 | 0x02 | 9 |
| 0x0E | hood style | HoodIdx +0x29 | +0x80 | 0x02 | 0x5D |
| 0x0F | hood material (carbon) | CarbonFiberHood +0x2A | +0x88 | 0x02 | 0x5C |
| 0x10 | blower | BlowerIdx +0x2B (stored +1 in render) | +0x90 | 0x02 | — |
| 0x11 | spoiler | SpoilerIdx +0x28 | +0x98 | 0x02 | 10 |
| 0x12 | (unused: Set returns 0) | — | +0xA0 | — | — |
| 0x13 | taillight geometry | TaillightGeomIdx +0x30 | +0xA8 | 0x02 | 0x5F |
| 0x14 | taillight material | TaillightMaterialIdx +0x32 | +0xB0 | 0x02 | 0x5E, 0x83 |
| 0x15 | body kit / chop top | BodyIdx +0x27 | +0xB8 | 0x02 | 0x12 |
| 0x16 | license plate style | LicenseBG +0x3E | +0x60 | 0x10 | 0x21 |
| 0x17 | plate letter slot | letter1..7 +0x36+slot (raw char) | — | 0x10 | 0x22 |
| **0x18** | **rim size** | RimSize0/1 +0x1A+axle, **byte = index+12 (inches)** | +0xE8 (rim_12..rim_28) | 0x81 | 0x28 |
| **0x19** | **tire profile** | TireProfile0/1 +0x1C+axle (-1 = auto) | +0xF0 (Profile_0..7) | 0x81 | 0x29 |
| **0x1A** | **tire width** | TireWidth0/1 +0x18+axle | +0xF8 (tw_195..tw_355) | 0x01 | 0x2A |
| 0x1B | wheelie bar | +0x2C | +0xD8 | 0x02 | 0x11 |
| 0x1C | one-shot kit / mud flaps | OneShotKitIdx +0x2D | +0xD0 | 0x02 | 0x79, 0x74 |
| 0x1D | brush guard | +0x2E | +0xC0 | 0x02 | 0x75 |
| 0x1E | front grill / louvers | +0x2F | +0xC8 | 0x02 | 0x76, 0x73 |
| **0x1F** | **ride height** | RideHeight0/1 +0x16+axle | +0x100 (15 names) | 0x80 | 0x2D front, 0x2E rear |
| 0x20 | (not settable) | — | +0x108 | — | — |
| 0x21 | window tint | WindowTintIdx +0xA7 (-1 custom) | +0x130 | 0x200 | 0x0F |
| 0x22 | neon colour | NeonIdx +0xA8 (0 = none, -1 custom) | +0x138 | 0x200 | 0x40 |
| 0x23 | performance upgrade state | EngineState.. (Set clears byte +0x56+index) | +0x180 | 0x80 | 0x45..0x54 |
| 0x24 | airbags | HydraulicsState0 +0x63 (bool) | +0x110 (ab_none, ab_same, ab_f2b, ab_b2f, ab_l2r, ab_r2l) | 0x80 | 0x2B |
| 0x25 | hydraulics | HydraulicsState1 +0x64 (bool) | +0x118 (off_hdralics, on_hydraulics) | 0x80 | 0x2C |
| 0x26 | rim chrome colour | Rim_Chrome_color +0xAA | +0x150 | 0x800 | 0x37 |
| 0x27 | rim insert colour | Rim_Insert_Color +0xAC | +0x168 | 0x800 | 0x43 |
| 0x28 | caliper colour | Caliper_Color +0xAE | +0x160 | 0x1000 | 0x38 |
| 0x29 | exhaust chrome colour | Exhaust_Chrome_Color +0xAB | +0x150 | 0x400 | 0x39 |
| 0x2A | nitrous (bottle) colour | Nitrous_Color +0xAD | +0x170 (nbc_default, white, red, ... 11) | 0x200 | 0x3A |
| 0x2B | vinyl colour A/B | VinylColorA/B +0xA1/+0xA2 (-1 custom) | +0x160 | 0x200 | 0x3B |
| 0x2C | decal colour | ChromeDecalColorIdx +0xA5 if UseChromeDecals else DecalColorIdx +0xA6 | +0x150 / +0x158 | 0x200 | 0x3D, 0x3E |
| 0x2D | decal material normal/chrome | UseChromeDecals +0xA4 | +0x140 (normal, chrome) | 0x200 | — |
| 0x2E | trim chrome colour | Chrome_Color +0xA9 | +0x150 | 0x200 | 0x3F |
| 0x2F | neon flash rate | NeonFlashRate +0xA3 | +0x148 (nflash_none, default, 1..4) | 0x200 | 0x41 |
| 0x30 | rider | RiderIdx +0x31 | +0x50 | 0x2000 | 0x84..0x8F |
| 0x31/0x32 | custom front/rear message letter | CustomFrontMsg1..8 +0x67+i / CustomRearMsg +0x6F+i | — | 0x20 | 0x7B |
| 0x33 | HUD colour | Hud_Color +0xAF (-1 custom) | +0x178 (black, white, red, orange, ..., hud_orange ... hud_yellow) | 0x200 | 0x77 |

Static name tables (indices are what saves store):

| Table | Contents |
|---|---|
| 0x4719C8 | rim_12..rim_28 (17) |
| 0x471938 | Profile_0..7 (8) |
| 0x471A54 | ride heights (15): rh_0, rh_100..rh_800, rh+600..rh+100 |
| 0x471ADC | tw_195..tw_355 (9) |
| 0x471990 | airbags (6) |
| 0x471978 | hydraulics (2) |
| 0x4719A8 | nflash (6) |

### 2.2 Value tables (the real per-step limits)

| Table | Indexed by | Values | Readers |
|---|---|---|---|
| 0x471A90 f32[15] ride height (m) | RideHeight byte | 0, -0.025, -0.05, -0.075, -0.1, -0.1125, -0.125, -0.1375, -0.15, +0.125, +0.1125, +0.1, +0.075, +0.05, +0.025. Then 4 zeros at 0x471ACC, then the tw name pointers | 0x270BE9 (GetRideHeight), 0x270B86 (fit) |
| 0x4B3918 f32[9] tire width (m) | TireWidth byte | 0.195 .. 0.355 step 0.02. 0x4B393C=0 | 0x270C04, 0x272FC9 |
| 0x4B3910[byte] f32 "rim+3in" diameter (m) | **RimSize byte 12..28** (table really starts at 0x4B3940) | 0.381 .. 0.7874 = (rim+3)*0.0254. **Byte 29+ reads 0x4B3984 = pointer "Invalid" -> ~0** | 0x270AF4, 0x270B44, 0x270B68, 0x273006 |
| 0x471958 f32[8] profile divisor | TireProfile byte | 0.4155, 0.4759, 0.5389, 0.6, 0.66, 0.725, 0.782, 0.83 (Profile_0 = tallest sidewall) | 0x270B19.., 0x27300D |
| 0x471A0C f32[17] | — | 0.0762, 0.254 .. 0.6604 (3", 10".. 26") | **unreferenced** (only loop-bound compare 0x270C51) |

Tire geometry:
- `tireDiameter = table4B3910[rimByte] / profileDiv[profile]` (0x270B40, 0x272FE0). Physics radius = 0.5 x that.
- Example: rim 18 with Profile_6 gives 0.5334/0.782 = 0.682 m.
- Profile -1 (auto): 0x270AF0(rim, origDiameter) picks the profile whose divisor is closest to rim/origDiameter.
  origDiameter comes from the car model's wheel (0xF11E0: `[[car+0x1800]+0x18]+0x3818+axle*4` -> +0x14, times 2;
  default -0.3).

---

## 3. CarCfg (the saved car configuration)

The visitor 0x270E80 registers every field. The parser registration call is 0x32F3D0(type, name, ptr, count, 0).

Parser types:

| Type | Meaning |
|---|---|
| 0 | string |
| 1 | bool |
| 3 | u8 |
| 4 | u16 |
| 5 | int |
| 6 | float |
| 8 | vec3 |
| 9 | vec4 |
| 10 | block |

The text form is what `customdata/*.carcfg` uses. Saves hold the same bytes **(guess for the exact save
serialisation)**: the save path goes through the same visitor or a memcpy of the struct, and HANDOFF's earlier
note is consistent.

| Offset | Field (file name) | Type | Notes |
|---|---|---|---|
| +0x08 | dirty flags | u32 | bits per §2.1. Bit 0x80 (sign of byte +8) makes the car component re-run ApplyCarCfg (0xFFD90). F32A0 clears & 0xFFFFC080 |
| +0x0C/+0x10 | Camber0/1 | f32 | degrees? -2.5/-1.0 in nearly all files. Copied to render +0x12E8/+0x12EC |
| +0x14/+0x15 | TireOffset0/1 | u8 | 0 in all 1,086 files. -> 0x270BD0(offset, axle) -> track offset in 0xFB4E0 |
| +0x16/+0x17 | RideHeight0/1 | u8 | index into 0x471A90. Stock files use 0..14 |
| +0x18/+0x19 | TireWidth0/1 | u8 | 0..8 |
| +0x1A/+0x1B | RimSize0/1 | u8 | **inches** (12..28). db index + 12 |
| +0x1C/+0x1D | TireProfile0/1 | s8 | 0..7, -1 auto |
| +0x20 | RimMdlIdx | u8 | rim model (customwheels `rims`). Render clamps to the colour DB count (0xF3640) |
| +0x21/+0x22 | TireMdlIdx / BikeTireMdlIdx | u8 | |
| +0x23 | BrakeMdlIdx | u8 | clamped by `[[0x509E1C]+0x34]+4` |
| +0x24..+0x32 | body parts | u8/s8 | see §2.1 (FrontBumper +0x24, RearBumper +0x25, SideSkirt +0x26, Body +0x27, Spoiler +0x28, Hood +0x29, CarbonHood +0x2A, Blower +0x2B, WheelieBar +0x2C, OneShotKit +0x2D, BrushGuard +0x2E, FrontGrill +0x2F, TaillightGeom +0x30, Rider +0x31, TaillightMaterial +0x32) |
| +0x33/+0x34/+0x35 | ExhaustRear / UseSideExhaust / ExhaustSide | | |
| +0x36..+0x3C | letter1..7 | char | plate text. Render maps them through ">ABC..." (0x454827, 42 symbols) |
| +0x3E | LicenseBG | u8 | plate style |
| +0x3F | PaintType | u8 | 0 Gloss, 1 Metallic, 2 ColorShift, 3 Pearlescent, 4 Matte, 5 Enamel |
| +0x40 | PaintPreset | s8 | -1 = custom colour from the X/Y/A bytes |
| +0x41..+0x4F | PaintColorX/Y/A 0..4 | u8 x3 x5 | colour picker coordinates (§10.3). Index 0 = base, 1..4 gradient colours, X/Y for colour shift |
| +0x50..+0x54 | ColorShift0..4 | u8 | gradient positions |
| +0x55 | NumGradients | u8 | gradient count (PaintType 2) |
| +0x56..+0x5C | EngineState0..6 | u8 | upgrade levels: Exhaust, Headers, Intake, Turbo, Upgrade, Chip, Nitro (§6) |
| +0x5D/+0x5E | TransState0/1 | u8 | Clutch, Gears |
| +0x5F/+0x60 | SuspensionState0/1 | u8 | ShocksAndSpring, SwayBar. **SuspensionState0 != 0 is required for ride height** |
| +0x61 | BrakeState0 | u8 | |
| +0x62 | TireState0 | u8 | Tires (3 levels: L1 33%, L2 67%) |
| +0x63/+0x64 | HydraulicsState0/1 | u8 | airbags (Pneumatic) / hydraulics. Set by types 0x24/0x25 |
| +0x65/+0x66 | UseCustomLogosFront/Rear | bool | |
| +0x67..+0x6E / +0x6F..+0x76 | CustomFrontMsg1..8 / CustomRearMsg1..8 | char | |
| +0x78..+0x93 | Logos[14] | u16 | [0..1] type 1, [2..3] type 2, rest are decal slots (DecalsSlot1..8 at 0x458204..) **(guess for 4..13)** |
| +0x94 | Vinyl | u16 | up to 956 in shipped files |
| +0x96 | CustomWindowFontFront **and** Rear | u8 | **both file fields register the same byte +0x96** (0x271Bxx): the rear value overwrites the front |
| +0xA0 | UseDefaultVinylColor | u8 | |
| +0xA1/+0xA2 | VinylColorA/B | s8 | -1 custom |
| +0xA3 | NeonFlashRate | s8 | render: `(sin(t * rate * 2pi) + 1) / 2` |
| +0xA4 | UseChromeDecals | u8 | |
| +0xA5/+0xA6 | ChromeDecalColorIdx / DecalColorIdx | s8 | |
| +0xA7 | WindowTintIdx | s8 | -1 custom (304 files) |
| +0xA8 | NeonIdx | s8 | 0 off, -1 custom, else neoncolors.clib entry |
| +0xA9..+0xAF | Chrome/RimChrome/ExhaustChrome/RimInsert/Nitrous/Caliper/Hud colour | s8 | -1 custom. HudColor 11 or 255 in files |
| +0xB0..+0xB5 | VinylColorAX/AY/AAlpha, BX/BY/BAlpha | u8 | |
| +0xB6..+0xD6 | *CustomX/Y/A for ChromeDecal, NormalDecal, Chrome, Rim, Exhaust, RimInsert, Nitrous, Caliper, WindowTint, Neon, Hud | u8 x3 each | picker coordinates. A is 0..100 (x0.01) or 0..255 (x0.00667) per field |
| +0xE1 | (not a file field) | s8 | **(guess)** hydraulic/airbag pump state: 0xFFD90 uses `cfg[+0xE1]*0.01` and calls 0x103A50; -1 -> 0x103C50 |
| +0xE2 | (not a file field) | u8 | locked flag checked by the shoulder-button paint cycling (0x14E6C0) |
| +0xE3 | TypeName | string, 0x16 max (0x270E9x sets descriptor+10 = 0x16) | car type; 0x270AE0 maps it to the car index |
| +0xF9..+0x105 | ProfileName1..13 | u8 chars | owner profile name |

Car info tables: `[0x557EB0] + carIdx*0x28` (+8 = class: 4 = sport bike, 5 = chopper; see 0x145DD4/0x146040)
and `0x5499F8 + carIdx*0x1BC` (0x14661D).

Garage-relevant CarCfg getters:
- 0x270BE0 GetRideHeight(axle) -> 0x471A90.
- 0x272FC0 GetTireWidth(axle).
- 0x272FE0 GetTireDiameter(axle, origDia).
- 0x270C00 WidthOfIndex.
- 0x270B40 Dia(rimByte, profile).
- 0x270BC0 ProfileDiv(idx).
- 0x2707F0 State(kind, i), with kind 0 engine +0x56, 1 trans +0x5D, 2 susp +0x5F, 3 brake +0x61, 4 tire +0x62,
  5 hydraulics +0x63.

---

## 4. Garage C++ class (this = garage object)

| Offset | Meaning |
|---|---|
| +0x18 | Flash wrapper |
| +0x1A0 + depth*0x28 | per-depth string (previous selection or "custom") |
| +0x1A8/+0x1AC/+0x1B0/+0x1B4/+0x1B8 + depth*4 | category id at each menu depth. 0x1B4 = 0x25 Front-Rear dims, 0x26 front, 0x27 rear |
| +0x3E4 | names variable (char[0x32]) |
| +0x416 | status variable (char[0x32]) |
| +0x448 | menu depth |
| +0x470/+0x4A0 [depth] | selection index / committed index (1-based) |
| +0x48C[depth] | current item index |
| +0x46C/+0x470 | last type/axle previewed |
| +0x71C[+0x72C] | current CarCfg |
| +0x714[...] | car |
| +0x774/+0x775 | list refresh mode / dirty |
| +0x968 + slot*0x32 | saved previous name (revert on cancel) |
| +0x9CC | current part type id (§2.1) |
| +0x9D8/+0x9DC | front-rear both-axle flags |

Flow:
1. **Category switch** 0x13E8D0(category, &type, namesBuf, statusBuf, &isPart) maps a category to a pair of
   Flash array names and a part type. Full table in `scratchpad/cust/dec/13e8d0.c`. Callers: 0x14A44A, 0x14C4AE,
   0x14D706, 0x14DA47.
2. **Status pass** 0x1465E0(firstIndex). Called from the update 0x1519D0 (at 0x151BBA) when +0x775 is set. That
   update is called from 0x152420, the garage state machine. Walks `i < Length(status)` and reads `names[i]`
   (§0.3 for the status codes).
3. **Availability** 0x145DC0(item, axle). 1465E0 calls it only for types 0x18/0x19/0x1F. For 0x1A (width) it
   just picks the axle (0x25 -> 0, 0x27 -> 1) and breaks out of the switch into the generic "installed/owned"
   compare. **Width is never fit-checked** (0x145DC0 itself would return false for 0x1A). Fit check:
   0xEF590(axle, profileIdx, rimIdx+12, rideIdx) -> 0x270B60:
   - `r = dia/2` (from rim and profile).
   - Pass if `GroundClearance + SuspensionClearance[axle] <= r + ride`
     (lowering is limited by the clearance; ride is negative when lowered).
   - and `r - ride <= SuspensionLimit[axle]`.
   - and `r <= TireDiameterLimit[axle]/2` (falls back to SuspensionLimit if <= 0).
   - The unknowns are filled from the CarCfg's *current* other parts (0x272A20 + 0x273EE0). So rim, profile and
     ride height limit **each other**.
4. **Preview/apply** 0x14C830(type, flashIndex 1-based, namesVar, axle, depth, slot):
   - Reads `names[idx-1]` and stores the old name for revert.
   - Calls 0x2730B0(type, name, axle) = `db.Index` -> CarCfg::Set 0x272020.
   - Special cases: 4 (paint type, 0x13E5A0), 0xE (hood, 0x13E4A0), 0x2C (hydraulics, also sets type 0x2D...),
     0x2B (airbags/vinyl colour presets mapping white/black/... names), 5 (paint preset needs PaintTypeNames
     index).
5. **Buying** 0x14E6C0 / 0x145A10 (Flash vars "oktobuy", "ShoulderInput", "InShowroom", "selected2"). Prices
   live in the movie (`...Price` arrays, "$549.00" strings). The C++ only reads `TopLevelStatus`, `ShowRoomPrice`
   and the performance `*UpgradePrice` arrays (0x1423F3..). The money check itself was not followed.
6. Owned parts: bitfield via 0x273360(type, idx) (0x272960 gives the count and list, 0x270860 gives the bit
   array).

Flash array names the C++ uses (all refs in 0x13E8D0 unless noted). Each has a matching `...Status`:
- UpgradeTypes, BodyUpgrade, WheelUpgrades, LicencePlateUpgrade, DecalChoice, VinylChoice,
  ChassisSuspensionUpgrade(Name), BodyUpgrade{FrontBumper,RearBumper,SideSkirt,Spoiler,Hood,Taillights,
  ExhaustTips,WheelieBar,ChopTop,HoodMaterial,HoodStyle,TaillightsMaterial,TaillightsStyle,Louvers,MudFlap,
  BrushGuard,FrontGrill,OneShotKit}.
- PaintType, ColorWindowsTint, ColorNeonType, DecalCategory, DecalSideCategory1, DecalFrontWindowCategory1/2,
  LicencePlatesStyle, LicencePlatesLettersSlot, Rims, Tires, WheelDimension, RimSize, TiresProfile, TiresWidth,
  Airbag, Hydraulics, RideHeight, RideHeightRear.
- ColorGl/Met/CS/Prl/Mtl/Enl (paint presets per type), ColorRims, ColorCalipers, ColorExhaust, ColorNitrous,
  ColorVynils, ColorDecalsNormal/Chrome, ColorTrim, ColorNeon, FlashingNeon, ColorRimsType, ColorRimsinsert,
  ColorHud.
- Power/Transmission/Chassis/Tires/Brakes/Turbo/Intake/Headers/Exhaust/Computer/Engine/Nitrous/Clutch/GearKits/
  Suspension/SwayBars Upgrade.
- Shop, ShowRoom, Badges, BadgesPositions, the 12 vinyl families, DecalCustomChoice/Font/Letters,
  DecalPositionsBody/Window, VinylUpgrades, Rider*.

Per-car movie contents (vs_vp_350z_04): RimSizeNames = rim_20..rim_15 (6), TiresProfileNames = Profile_2..7,
TiresWidthNames = tw_355..tw_195 (9), RideHeightFrontNames = rh_800..rh_0 (9; raised heights are not offered),
RimsNames = 69 rim models with prices, TiresNames = 11.

---

## 5. How a selection reaches physics and visuals

### 5.1 Physics: vehSim::ApplyCarCfg 0x121970(cfg, model)
thiscall, this = vehSim (car+0x50). Called from 0xFFCC1 (spawn), 0x100592 and 0x1006CA (car component update
0xFFD90, when CarCfg dirty bit 0x80 is set).

For front (`[sim+0x54]`) and rear (`[sim+0x58]`) wheel parameter blocks:
- `+0x04 radius = 0.5 * GetTireDiameter(axle, model.origDia)`.
- `+0x08 width = GetTireWidth(axle)`.
- `+0x48 SuspensionOffset = GetRideHeight(axle)`.

**It overwrites** the .vehsim SuspensionOffset (0 in the 350Z file). Anything else that edits SuspensionOffset
(tuning.cpp) must re-apply after every ApplyCarCfg. garage.cpp already notifies tuning.cpp.

The wheel parameter block layout is the 0xA6DA0 visitor:

| Offset | Field |
|---|---|
| +0x00 | SlidingFric |
| +0x04 | radius (from the CarCfg) |
| +0x08 | width (from the CarCfg) |
| +0x0C | TireDispLimitLat |
| +0x10 | TireDispLimitLong |
| +0x14 | TireDampCoefLat |
| +0x18 | TireDampCoefLong |
| +0x1C | SurfaceDragCoefLat |
| +0x20 | SurfaceDragCoefLong |
| +0x24 | SteeringLimit |
| +0x28 | CamberLimit |
| +0x2C | WobbleLimit |
| +0x30 | AxleLimit |
| +0x34 | BrakeCoef |
| +0x38 | HandbrakeCoef |
| +0x3C | SteeringOffset |
| +0x40 | TravelLimit |
| +0x44 | TravelExtent |
| +0x48 | SuspensionOffset |
| +0x4C | SuspensionLimit |
| +0x50 | SuspensionExtent |
| +0x54 | SuspensionClamp |
| +0x58 | SuspensionFactor |
| +0x5C | SuspensionDampCoef |
| +0x60 | SuspensionDampFactor |
| +0x64 | OptimumSlipPercent |
| +0x68 | StaticFric |

vehSim (0xA3CA0):

| Offset | Field |
|---|---|
| +0x0C | ModelOffset (vec3) |
| +0x18 | CenterOfMass |
| +0x24 | Mass |
| +0x28 | Size (vec3) |
| +0x34 | InertiaBox |
| +0x40 | BoundFriction |
| +0x44 | BoundElasticity |
| +0x48 | BoundGravity |
| +0x4C | AirGravity |
| +0x50 | DrivetrainType |

Then come the sub-blocks Aero, Fluid, Engine, Trans, Drivetrain, Freetrain, WheelFront, WheelBack, AxleFront and
AxleBack. HANDOFF puts the wheel pointers at +0x54/+0x58. The .vehsim text puts DrivetrainType at +0x50, so the
pointers really are at +0x54/+0x58 as ApplyCarCfg uses them.

### 5.2 Visuals: car render CarCfg apply 0xF32A0(cfg, force, playerIdx)
this = car render object (has +0x1800 = model/instance). Called with dirty bits, or 0x3FFF when forced.

Bits 0x01|0x04 (wheels):
- +0x12D0 / +0x12C8 = tire **width** front/rear via **0x270C00(index)** (the separate width table getter
  garage.cpp hooks).
- +0x11B4/+0x11B8 = profile index (auto-picked through 0x270AF0 or 0x273020 when -1, using the model's wheel
  radius x2 from `[[+0x1800]+0x18]+0x2664..`).
- +0x11BC/+0x11C0 = rim byte.
- +0x12D4 / +0x12CC = tire **diameter** front/rear = 0x270B40(rimByte, profile). Read by 0x103C50 (§6) and by
  the wheel visuals in 0x106F90 (0x1073A2, 0x107F80, 0x108CD5, 0x10990D) and 0x105165.
- +0x12E8/+0x12EC = camber.
- +0x12D8/+0x12DC = 0xFB4E0(width, 0x270BD0(TireOffset, axle), axle) = track/hub offset.
- +0x12E0/+0x12E4 = model wheel radius (+0x14 of `[[..+0x18]+0x3818/+0x381C]`) plus the hub bone height.
- +0x11AC rim model, +0x11B0 tire model, +0x11C8 bike tire, +0x11C4 brake.
- Sets `[+0x1800]+0x15F` (and +0x15C) "rebuild wheels" flags.

Other bits:

| Bit | Fields |
|---|---|
| 0x02 | body parts -> +0x1180.. (front bumper +0x1180, rear +0x1188, skirt +0x1190, body +0x11DC, spoiler +0x11EC, hood +0x11F4, carbon +0x11F8, blower-1 +0x11FC, taillight +0x12B8, wheelie bar +0x11A4, one-shot +0x11A0, brush guard +0x1198, grill +0x119C, taillight material +0x120C) |
| 0x08 | exhaust |
| 0x10 | plate letters (+0x116E..) and style (+0x1178) |
| 0x20 | logos and messages (+0x10B0..+0x112C, +0x1164) |
| 0x40 | vinyl (+0x1200) |
| 0x100 | paint (§10) |
| 0x1E00 | colours (§10.2) |

At the end `cfg[+8] &= 0xFFFFC080`.

**Model swapping:** part indices are per-car mesh names baked in the car bundle, e.g.
`vroot_bmpf_bone_<partname>_geo.mesh.xbck` in `resources/vehicle/vp_<car>/` (archive `_2`). The movie list order
(`BodyUpgradeFrontBumperNames` ...) is the order of those meshes. Adding parts means adding meshes to the car
bundle **and** names to the movie arrays **and** to the database (the DB body lists are loaded... **(guess)** from
the car bundle's part list, not found in the text files).

### 5.3 Which limits live where

| Limit | Where | Per car? |
|---|---|---|
| Rim sizes offered | car movie `RimSizeNames` (4..12 entries) | per car (movie) |
| Rim size fit | vehModel TireDiameterLimit/SuspensionLimit/Clearance + 0x270B60 | per car (vehModel), formula hard-coded |
| Rim diameter values | 0x4B3940 table (12..28") | hard-coded |
| Profiles | movie list + 0x471958 (8) | hard-coded values |
| Tire widths | movie list + 0x4B3918 (9) | hard-coded values, no fit check |
| Ride heights | movie list (most cars 9 lowered only) + 0x471A90 (15) | values hard-coded; list per car |
| Max raise/lower | vehModel clearance via the fit check; SuspensionLimit in .vehsim limits travel | per car |
| Upgrade effect | base/mods .vehsim + .vehmods percentages | per car |
| Pool sizes / 128-element arrays | Flash VM, §0.2 | global |

---

## 6. Performance upgrades (.vehmods) and how they reach vehSim

Each car has `<car>_base.vehsim` (stock) and `<car>_mods.vehsim` (everything installed). The engine
**interpolates** each physics parameter between the two.

0x103C50(car, cfg) is called from 0xFFCAB/0x103C6E (spawn, upgrade change) and 0x142CCC (garage stats preview). It:
1. Calls 0x129800(out, cfg+0x56) (vehMods::Sum). For each of the 7 engine parts it reads the level byte
   `cfg[+0x56+part]`. The part record is 15 shorts (30 bytes): a level table at +4 (L0=0, L1=L1Percentage
   (e.g. 50), L2=100, ...) and 9 effect percentages at +0xC (EngineHP, EngineRPM, GearRatios, Drag, Wheelie,
   Burnout, Nitro, Wobble, Backfire). 0x1292D0 scales the effect shorts by `levelPct*0.01`, and 0x129800 sums
   them per effect.
2. Writes `param = base + (mods - base) * sumPct * 0.01` into the live vehSim for every affected field. For
   example, `[[car+0x50]+0x5C]+4` uses EngineHP%. The base/mods sims are at component +0x34/+0x38 (engine),
   +0x3C/+0x40 (wheels)... (0x103C50 lines 52-460).
3. Also scales the tire diameter by +0x12D4/+0x12CC (line 460-468) **(guess: wheel inertia/gearing correction
   for the bigger wheel)**.

The shipped 350Z percentages per effect column sum to 100 over the parts. So "all parts at max level" = the mods
.vehsim exactly. Category/part names are tabled at 0x4B1408:
- Engine {Exhaust, Headers, Intake, Turbo, Upgrade, Chip, Nitro}.
- Trans {Clutch, Gears} with effects ShiftTime/Boost/NumGears/GearRatios.
- Suspension {ShocksAndSpring, SwayBar} with effects Spring/Shock/Drift/Turn/SSS/SimCG/Spin/Axle/Inertia/AngDamp.
- Brake {Brakes} with BrakeCoef/HandbrakeCoef.
- Tire {Tires: L1 33, L2 67} with Tread/Sidewall/Compound.
- Hydraulics {Pneumatic, Hydraulic} with Pump.

Extension risks:
- More upgrade levels: the level table in each part record is 4 shorts at +4..+0xB. A level byte >= 4 reads the
  effect shorts as level percentages.
- More parts: needs new CarCfg bytes. +0x56..+0x64 are packed with no room.
- Percentages > 100 total extrapolate beyond the mods .vehsim. That probably works numerically but is untested.

---

## 7. Save format, network and AI

- The CarCfg is one fixed struct. New indices beyond the stock tables are stored as plain bytes:
  - RideHeight 15..28 and TireWidth 9..15 (garage.cpp) load as the same bytes.
  - An unpatched game, or the console, maps them through tables past their end: ride height becomes 0 m or
    garbage tiny values, width becomes 0 or a pointer reinterpreted as a float.
  - **Saves made with the extension are not safe on original hardware or emulators.**
  - garage.cpp keeps its getters active even with the setting off. Good.
- Network: car configs are sent to other players **(guess)**. String "NumCarConfigNames" (0x4740A8,
  refs 0x280609/0x28319A) is near the network/profile code. A remote stock game would mis-render extended values.
  Not traced.
- AI/rival cars load `customdata/*.carcfg` through the same visitor and ApplyCarCfg. Shipped ranges (all 1,086
  files):

  | Field | Shipped range |
  |---|---|
  | RideHeight | 0..14 |
  | TireWidth | 0..8 |
  | RimSize | 12..28 |
  | TireProfile | 1..7 |
  | PaintType | 0..5 |
  | NeonIdx | 0..4 or 255 |
  | WindowTint | 0..4 or 255 |
  | Vinyl | <= 956 |
  | TireOffset | always 0 |

  So every stock table index is already in use by some AI car.

---

# PRIORITY SECTIONS: tire width, collision bound and scrapes, tuck vs ride height, neon, vinyls/decals, paint

These sections are for the agents working on these features. Each one says what is proven and what is a guess.

## 8. Tire width (complete chain)

| Step | Where | Detail |
|---|---|---|
| Options | DB list +0xF8 (tw_195..tw_355, 9 names, table 0x471ADC). Movie `TiresWidthNames` (350Z: all 9, widest first) | Garage category 0x2A, type 0x1A |
| Status | 0x1465E0: forced "1". **No fit check** (§4 step 3). Installed/owned markers through the generic name compare | |
| Store | CarCfg::Set case 0x1A: `cfg[+0x18+axle] = index`, dirty `|= 1` (note: **not** 0x80) | 0x272020 |
| Physics | ApplyCarCfg 0x121970: wheelParams+0x08 = 0x272FC0(axle) = 0x4B3918[idx] (0.195..0.355 m) | ApplyCarCfg re-runs only on dirty 0x80 or a respawn. A width change sets only bit 1, so physics width updates once something else sets 0x80 or the car respawns **(guess: leaving the garage respawns it)** |
| Visual | 0xF32A0 bit 0x01: render+0x12D0 (front) / +0x12C8 (rear) = 0x270C00(idx) (same table). +0x12D8/+0x12DC = 0xFB4E0(width, offsetTable(TireOffset), axle) = hub/track offset **(guess on the meaning of FB4E0)**. Sets `[render+0x1800]+0x15C` (rebuild) | Readers of +0x12D0: 0xF3320, 0xF46E0, 0xF7214, 0xFF243 (car component copies) |
| Limits | 9 entries, then 0.0 at 0x4B393C. Index 10+ reads the rim diameter table (0.381..), then pointers | garage.cpp hooks 0x272FC0 and 0x270C00 for 9..15 |
| Save/network | raw byte (§7) | |

The physics consumer of wheel+0x08 was not traced **(guess: contact patch width / lateral contact offset)**.

## 9. Collision bound, chassis-ground contact and scrape sparks

### 9.1 The bound
- 0x121DE0 (car physics instance init) creates the phArchetype (`new 0x78` -> 0x2A3310). It sets mass and
  inertia from vehSim (+0x24 Mass; InertiaBox +0x34/+0x38/+0x3C via 0x341670), then loads the bound resource
  "<carTypeName>_bound" (suffix string 0x44EB8C) through 0x2A03A0 and attaches it (vtbl +0x34/+0x38).
- The car bundle `resources/vehicle/vp_<car>/vp_<car>.xbck` has an "@boundbone" node, so the bound is **embedded
  in the car's own bundle (guess)**. It is a phBoundGeometry polygon mesh (loader strings 0x46F1C8..,
  "BoundsPolygon" 0x46DBC0).
- Surface response comes from .vehsim:

  | Offset | Field | 350Z value | Note |
  |---|---|---|---|
  | +0x40 | BoundFriction | 0.15 | |
  | +0x44 | BoundElasticity | 0.2 | |
  | +0x48 | BoundGravity | 2.0 | **(guess)** extra gravity while the body is in contact |
  | +0x4C | AirGravity | — | |

- **The bound is rigid in chassis space. Ride height does not move it relative to the body.** SuspensionOffset
  (wheelParams+0x48, written by ApplyCarCfg) moves where the wheels sit relative to the chassis. Lowering
  (negative) brings the chassis, the visual body *and the bound*, closer to the road. Below a few cm the bound's
  underside touches the ground. That causes:
  - sparks and scrape sounds (§9.2);
  - BoundFriction drag;
  - the car resting on its bound instead of the springs.
- The only stock guard is the garage fit check 0x270B60 (`GroundClearance + SuspensionClearance <= tireRadius +
  rideHeight`). garage.cpp forces every option available, so very low ride heights *will* drag the bound. That is
  what produces the scrape sparks the feature wants. Lowering only visually (render offset) gives **no** sparks.

### 9.2 Sparks: vehDamage + the collision handler 0x1027D0
`<car>.vehdamage` (visitor 0x101AE0, vtable 0x454FB8) fields:

| Offset | Field |
|---|---|
| +0x0C | MaxDamage |
| +0x10 | MedDamage |
| +0x14 | ImpactThreshold |
| +0x18 | WipeoutThreshold |
| +0x1C | WeightxferThreshold |
| +0x20 | ExplodeThreshold |
| +0x24 | JoltThreshold |
| +0x28 | FatalBlow |
| +0x2C | WipeoutDelay |
| +0x30 | DamageOutDelay |
| +0x34 | ExplosionDelay |
| +0x3C | ExplodeImpulse |
| +0x40 | **SparkStartColor** (vec4) |
| +0x50 | **SparkEndColor** (vec4) |
| +0x60 | **SparkVelVar** (vec3) |
| +0x6C | **RoofSparkVelVar** (vec3) |
| +0x78 | **SparkLifeTime** |
| +0x7C | **SparkEmitMod** |
| +0x80 | **SparkSize** |
| +0x84 | **BottomOutCap** (int, 50) |

0x1027D0 (damage component; `this[1]` = car, `this[3]` = vehDamage) handles one contact set. `param_1` =
{count; contacts at +0x20, stride 0x80 bytes; position at +0, normal near +0x38}. Steps:
1. Classify the two bodies and compute the impact speed `local_c8`. Apply deformation at the contact points
   (0xEF6E0 over render zones at +0x2310, count +0x1828).
2. **Roof sparks:** if speed > `[0x448D84]` and the car's up.y (`local_20`) < `[0x447E50]` (on the roof or side),
   then per contact (skipping surface material 6, from 0x100EF0), call 0x36F60(n, pos, vel, &RoofSparkVelVar,
   &StartColor, &EndColor, LifeTime, ..., SparkSize). n is capped at 25 and halved per point.
3. **Scrape sparks (chassis on ground):** when the contact is sliding (|dot(normal, velocity dir)| < 0.75, or
   0.25 depending on normal.y):
   - n = min(50, SparkEmitMod * |dv^2| * 0.01);
   - velocity variance = SparkVelVar, doubled one time in 10 (LCG `[0x4B2D04]`);
   - same 0x36F60 emitter;
   - scrape sound 0x10B130(speed, 1, 1, 0, -1);
   - `[car+0x18]` vtbl+0x1C(2) (rumble or camera shake, **guess**).
   Hard impacts instead call 0x10B130(speed, 0, ...) and vtbl+0x1C(1).
4. 0x35100 (at 0x102C9A) emits a second particle type on strong impacts **(guess: debris)**.

Sound tables for scrape/explode per impact size: visitor 0x21CA50 (ScrapeSound +0x24, entries {SoundName,
MaxForce, Min/MaxVolume, Near/FarDistance}).

BottomOutCap (+0x84): no reader found in a scan of 0xF0000..0x140000 **(open)**.

Tuning knobs: SparkEmitMod / SparkLifeTime / SparkSize / colours in `.vehdamage`, or hook the 0x36F60 calls in
0x1027D0.

## 10. Wheel tuck vs ride height

Proven:
- Ride height = wheelParams+0x48 SuspensionOffset, replaced by ApplyCarCfg (0x121970) on spawn and on dirty
  0x80. tuning.cpp's F1 offset is re-applied after it.
- The garage fit check (0xEF590 -> 0x270B60) uses these vehModel fields:
  - SuspensionLimitF/R +0x0C/+0x10;
  - TireDiameterLimitF/R +0x14/+0x18;
  - TireExtentF/R +0x1C/+0x20 (registered, not used by 0xEF590);
  - SuspensionClearanceF/R +0x24/+0x28;
  - GroundClearance +0x30.
- The original setup/blending body 0x103C50 also consumes SuspensionLimit (+0x0C/+0x10), GroundClearance (+0x30)
  and SuspensionClearance (the instruction sequence uses +0x24 for both axles) at 0x105165..0x105208 when writing
  axle mount heights through vehSim+0x88. These fields are therefore **not exclusive to the fit check**. The
  F1 wheel-size multiplier changes TireDiameterLimit, whose role remains the fit-size cap; it does not itself
  scale tire geometry.
- Wheel visual inputs written by 0xF32A0: tire diameter +0x12D4/+0x12CC, width +0x12D0/+0x12C8, track offset
  +0x12D8/+0x12DC, model wheel radius + hub bone y +0x12E0/+0x12E4 (from model nodes `+0x3818/+0x381C`),
  camber +0x12E8/+0x12EC.

**(guess, consistent with what was read)**:
- The visual wheel is drawn at the physics wheel position. Compression is limited by
  TravelLimit/SuspensionLimit (+0x40/+0x4C). The body is drawn at the chassis matrix. A big negative
  SuspensionOffset moves the wheel centre up into the arch, and nothing hides or moves the tire: stock "tuck" =
  the tire clips the fender. The garage fit check is what normally prevents visible clipping.
- Beyond the stock range the suspension can bottom out at SuspensionLimit at once. The body then rests on the
  bound (§9).
- A "tuck without clipping" mode needs the wheel render transform: the functions reading +0x12D4 at
  0x106F90 (0x1073A2, 0x107F80, 0x108CD5, 0x10990D) and 0x105165, and car-draw readers of +0x12E0/+0x12E4
  (0x1C4189, 0x1D7989 ...). Not traced.

Note: the rider "Tuck" string (0x476B70, set up at 0x2CE650) is the **bike rider pose** (Tuck/Wheelie/Nosie/Side),
unrelated.

2026-10-22 continuation: see `garage_tuning.md` §7 for the 38-width append-only map, direct-to-garage preview
ownership fix, garage/F1 rebase regression and bounded sampling of the unchanged 0xA7250 wheel contact solver.
The earlier garage notes mislabeled 0x103C00 as mount setup; it is disposal. Runtime chassis contact/sparks and
rendered tire bottoms still require separate validation; widened travel alone must not be described as proven
ground-intersection prevention.

## 11. Neon ground effect

| Item | Detail |
|---|---|
| Selection | type 0x22 NeonIdx (+0xA8): 0 none, -1 custom (NeonCustomX/Y/A +0xD1..+0xD3), else a neoncolors.clib entry (11 entries; RGBA with **A = 0.3**, lemon 0.4). Flash rate is type 0x2F (+0xA3: nflash_none, default, 1..4). Movie categories 0x10 ColorNeonType, 0x40 ColorNeon, 0x41 FlashingNeon |
| Render state (0xF32A0, bits 0x1E00) | render+0x1384 = enabled (NeonIdx != 0); +0x138C..+0x1398 = RGBA from `[[0x509E1C]+0x90]+4 + idx*16`, or custom via 0x13D300 with A forced to 1.0; +0x1388 = (float)NeonFlashRate (custom only, else from the palette path) |
| Textures | 0xFB9A0: `fx_car_neon_glow` -> `[0x509F6C]`, `vp_shadow_tex` -> `[0x509F70]` (pack `$/vehicle/shared_texture`) |
| Code | **0xFA5A0(mode, carMatrix, fade)**, this = per-car ground-effect object (`this[0]` = car render). Called from 0xFE059, 0xFE06E, 0xFEA8F |
| mode 1 = update | 4 quads (3x4 matrices at this+1, +0xD, +0x19, +0x25: front, rear, left, right) sized by the car box (0xA2450: +0x8/+0x10/+0x14/+0x1C) and the axle nodes (`[[car+4]+8]+0xD8/+0xDC`, positions +0x34..+0x3C). Edge factors 0.9 (0x44A714) and 0.95 (0x44EB94). **Per-quad alpha this[0x61..0x64] = 1 - clamp((quadY - carY) * 0.5, 0, 1)**: the glow fades as the body rises. Flash factor `k = (sin(t[0x53475C] * rate * 2pi) + 1) / 2`. Colour this[0x71..0x73] = neonRGB * k. Intensity this[0x74] = clamp(dot * 1.2) * (1 - fade) * 1.2 |
| mode 0x10 = draw | render states 0x2D3370, 0x2D35F0, 0x2D3880(0x207), 0x2D3840(0), 0x2D3620(0x203), 0x2D3940(1,1), 0x2D3660(2,0x80) (additive, alpha-ref 0x80, no depth write: **guess on names**). Per quad: texture `[0x509F6C]`, 4 immediate vertices (0x2D5D90(6,4) .. 0x2D5E40), packed ARGB = alpha x RGB, UVs x -0.375..0.375, y -0.5..0.25. Then the drop shadow with `[0x509F70]` (6 vertices, colour 0x30680) |
| mode 4 | dynamic light 0x97AD0(4, matrix this+0x31, 2.0, 8.0, height, RGB * 0.75, 1, 0). **(guess)** radius 2..8 m: it lights the road |
| Assets | the car bundle also has a `neonglow.mesh` and a `neon` node (mesh neon tube, not traced) |
| Not neon | 0x327670 `m_groundGlowScale`/`m_groundGlowColor1/2` belong to the power-up pickup effect |

## 12. Vinyls, decals, logos and tinting

- **Lists** (DB loader 0x275EF0):
  - `decaltextures.txt`: 7 categories, names table 0x4B39BC; category 6 is not stored. Feeds DB +0x20
    (vinyls; CarCfg Vinyl is a u16, up to 956 in shipped files).
  - `logotextures.txt`: `big` / `logos` / `fonts`.
  - `windowtextures.txt`: `all`.
  - `shared_vehicle_textures.txt`: `textures` -> DB +0x58.
- **Resources:** 0xF5E70 loads `$/resources/vehicle/%s` packs (wheel, brake, ..., rider, shared_tex; `.xbpf` on
  the HD path), the colour/decal DB object `[0x509E1C]` (resource type 0x25) and "decal_g" pages (0xF6191).
  Vinyl images are .xbpf gfxImage pages, tinted at runtime (docs/notes/texture_extraction.md).
- **Vinyl colours:** UseDefaultVinylColor (+0xA0); VinylColorA/B (+0xA1/+0xA2) = palette index into
  `[[0x509E1C]+0x84]+4` (16-byte RGBA), or -1 = custom AX/AY/AAlpha (+0xB0..+0xB2) / BX/BY/BAlpha (+0xB3..+0xB5),
  alpha x0.01.
  - Render: +0x13BC = colour A, +0x13CC = colour B, +0x1204 = use-default, +0x3134 = 1 (dirty).
  - Per-vinyl defaults: `vinylcolors/<vinyl>[_share].mcvinylpreset`. Visitor 0xF1970: +0x0C m_useRawPalette,
    +0x10 m_colorA (vec4), +0x20 m_colorB (vec4).
  - **(guess)** A vinyl is a two-colour mask whose channels select colour A or colour B in the paint shader.
- **Decals/logos:** UseChromeDecals (+0xA4) selects `car_decal_chrome.shadert` or `car_decal.shadert` (both
  created in 0x1B7250).
  - Chrome decal colour: +0xA5, palette `[0x509E1C]+0x7C`, or custom ChromeDecalCustom x0.00667. Render +0x135C.
  - Normal decal colour: +0xA6, palette +0x88, or custom NormalDecalCustom x0.01. Render +0x136C.
  - Shader vars: DecalShaderIdx, DecalShaderTintR/G/B/A (defaults 1, 1, 1, 0.8).
- **Window tint:** WindowTintIdx (+0xA7), palette +0x8C (windowcolors), or custom -> render +0x139C.
  - Window shader (0x1B77F0, `__envmap__`): tintR/G/B/A (defaults 0.25, 0.285, 0.25, 0.5),
    WinFresnelEnvScale 0.2, WinFresnelMin 0.2, WinFresnelMax 0.9, WinFresnelScale 0.99, WindowsBroken 0.
  - Textures: blank and `car_window_blank_dmg`.
- **Other colours** (all in 0xF32A0):

  | Colour | Render offset | Palette | Custom alpha scale |
  |---|---|---|---|
  | trim chrome | +0x12FC | +0x7C | x0.00667 |
  | rim chrome | +0x130C | +0x7C | |
  | exhaust chrome | +0x131C | +0x7C | |
  | rim insert | +0x132C | +0x80 | x0.01 |
  | nitrous | +0x133C | +0x80 | |
  | caliper | +0x134C | +0x84 | |

- **Plates:** customplates.txt has 81 styles {background, font, textoffset x y, numletters}. Parser 0xF1260:
  0x4C-byte records at 0x508010; unique backgrounds in 0x509C90[] (count 0x509E10), fonts in 0x507E90[] (count
  0x509E14), style count 0x509E18. Letters are 7 chars at CarCfg +0x36, mapped through the 42-symbol alphabet
  (0x454827 render / 0x4581D8 garage). The capacity of those arrays was not checked.
- **Custom text:** CustomFrontMsg/RearMsg (8 letters each). The font byte +0x96 is shared by front and rear
  (both visitor names point at it).

## 13. Car paint and shading

- **Selection:** PaintType (+0x3F) indexes DB +0x120 (0 Gloss, 1 Metallic, 2 ColorShift, 3 Pearlescent,
  4 Matte, 5 Enamel; built in 0x275EF0). PaintPreset (+0x40) indexes DB +0x128[type]: names from
  `paintjobs/paintjoblist.txt` ("Gloss 16 { ciendefault yellowgloss ... }"), each naming a
  `paintjobs/<name>.mccarpaintjob`. -1 = custom picker colour.
- **mcCarPaintJob** (visitor 0xF2110). Fields read by the game:

  | Offset | Field |
  |---|---|
  | +0x18 | m_numGradients |
  | +0x1C | gradient array pointer (16-byte {m_startPos%d int, m_color%d vec3}, **max 8**; loop bound 0x80 bytes at 0xF2231) |
  | +0x20 | m_paintType |
  | +0x24 | m_metallicPaintHilightScale |
  | +0x28 | m_gradientHighlightSize |
  | +0x2C | m_paintColor1 |
  | +0x38 | m_mShift |
  | +0x44 | m_additiveHilight |
  | +0x45 | m_additivePaint |

  **Ignored by the shipped parser** (these strings are not in the XBE, although every file has them):
  `m_shSize`, `m_highlightSize`, `m_highlightAlpha`, `m_carpaintFresnelParams`.
- **Render (0xF32A0, dirty 0x100):**
  - +0x1214 = +0x1248 = paint type; +0x1218 = preset, or -1 for custom.
  - Custom: 0x13D300(PaintColorX0, Y0, -1) -> base RGB at +0x1254..+0x125C.
  - Type 2 (ColorShift): +0x1240 = NumGradients; entries at `[+0x1244]` stride 16 = {pos = ColorShift[i]
    (+0x50+i), RGB from PaintColorX/Y[i]}; then 0xF24A0 **(guess: builds a ramp texture)**.
  - Type 3 (Pearlescent): second colour from X/Y1 -> +0x1260.
  - Type 1 (Metallic): +0x124C = PaintColorA0 x 0.0125 (A=100 gives 1.25, matching the presets).
  - Finally +0x1270 = 1.
- **Picker math 0x13D300(X, Y, sign, out, clamp):**
  - X and Y bytes map to [-1, 1]. A high Y gives a grey row.
  - Otherwise radius gives saturation (square-to-circle correction) and atan2 gives hue (6 sectors), then
    HSV->RGB (0x8C0B0).
  - sign < 0 gives the lightness-ramp variant; sign >= 0 goes to 0x13D200.
- **Shader constants:**
  - car paint (0x1B7190; `carpaint.shadert`, `garage_car_carpaint.shadert`, `carpaint_novinyl.shadert`):
    CarPaintEnvScale default 0, CarPaintFresnelMin 0.2, CarPaintFresnelMax 0.9, CarPaintFresnelScale 0.99.
    0xE5990 registers a named constant with a default; 0xE58B0 finds its slot, kept as bytes at
    material+0x14..+0x17.
  - Riders use cloth_fresnel_*.
  - The upload of the per-car paint colours (+0x1254.., +0x13BC..) into those constants was **not traced**.
    Only writes were seen in 0xF32A0.

---

## 14. Extension checklist

| Extension | Must touch | Fails how if forgotten |
|---|---|---|
| More ride heights | DB +0x100 (append after 14), getter 0x270BE0, fit reader 0x270B60 (or keep IsOptionAvailable forced), movie Names/Status(/Price) <= 128, labels, VM symbol-pool room | wrong height on stock builds; "6" greying; VM hang (pool full) / crash (no context) |
| Lower/raise in physics | wheel SuspensionLimit/TravelLimit, vehModel GroundClearance | bound drags (§9), bottoming |
| Rims > 28" | DB +0xE8 (byte = idx+12 < 256) **and** every 0x4B3910[byte] reader (0x270AF4, 0x270B44, 0x270B68, 0x273006): byte 29+ reads garbage; movie RimSize arrays | radius ~0 |
| More profiles | 0x471958 readers (0x270B19, 0x270B4F, 0x270B75, 0x270BC4, 0x27300D), auto-pick loop bound 8 in 0x270AF0 | |
| More widths | 0x270C00 and 0x272FC0 (done) | width 0 |
| More parts | DB list (grows), car-bundle mesh, movie arrays, CarCfg byte (s8 fields max 127) | missing mesh |
| More upgrade levels | vehMods part level table (4 shorts) | effect columns read as levels |
| More colours | colour DB `[0x509E1C]+0x7C..+0x90` (resource type 0x25) and movie lists | garbage colour |
| Saves / network | raw bytes everywhere | stock builds mis-read them |

## 15. Open leads
1. The reader of vehDamage BottomOutCap (+0x84).
2. Shader upload of the render paint/vinyl/neon colours (+0x1254.., +0x13BC.., +0x138C).
3. The wheel render transform (0x106F90 family, 0x1C3D30, 0x1C4170, 0x1D7980).
4. Network serialisation of CarCfg ("NumCarConfigNames" 0x4740A8; refs 0x280609, 0x28319A).
5. The CarCfg save write path (assumed to be the 0x270E80 visitor or a raw copy).
6. Which VM pool mode the garage uses (410 or 4000 symbols; game state `[0x53F2D4] == 5`?).
7. 0xFB4E0 (hub/track offset from width + TireOffset) and 0x270BD0 (the TireOffset table).
