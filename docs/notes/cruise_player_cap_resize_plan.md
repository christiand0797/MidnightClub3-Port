# Plan: more than 8 players in Cruise (System Link), races unchanged — 2026-11-05

Background and the table list: [cruise_player_cap.md](cruise_player_cap.md). Short version: the network session is
sized by one immediate, but the game's per-player tables are fixed at 8. Nothing below has been implemented.

## Target

- `N` = new slot count for Cruise. **Proposal: 16.** Limits that make 16 cheap: the joined-slot masks are 32 bits,
  the wire slot is one byte, and the transport's packet buffer is 0x4000 bytes. Going past 32 would mean widening
  every mask; past 16 the lobby UI and memory are the real problem (see Risks).
- Races: the game today allows 2..8 in every mode (4 is only a default). The Players handler will cap races at 8
  (or 4, if the user prefers; that is one constant) and only Cruise gets `N`.
- Both ends of a session run the same build, so changing message sizes between builds is acceptable, but a 16-slot
  build must refuse to join an 8-slot build's session (version byte in the discovery reply; see Phase 4).

## How the patches get in

The port can only replace whole functions (`X86_FUNC(sub_X)` in `templates/game/src/game/*.cpp`). Most of this
change is constants inside big functions: loop bounds, allocation sizes, displacements. Rewriting ~150 functions
in C++ by hand is error-prone.

**Phase 1 adds a recompiler feature:** a `[patches]` table in `templates/game/config/game.toml`, for example
`0x0001DD17 = { imm = 16 }` or `0x002693DD = { disp = 0x3E0 }`. `Codegen::EmitInsn` (tools/recomp/codegen*.cpp)
already gets the Zydis operands, so the override is a few lines: look up the VA and rewrite `ops[i].imm.value` or
`ops[i].mem.disp.value` before emitting, then add the override to the disassembly comment. A patch whose VA doesn't
decode to the expected instruction must fail the build. Unit test in `tests/`.
Cost: changing game.toml regenerates all the recompiled code (slow, but only once per batch).

**Status 2026-11-05: Phase 1 done.** `tools/recomp/patch.{h,cpp}` + `[[patch]]` parsing in `main.cpp`, applied in
`Analysis::Decode` (so analysis and codegen see the same instruction), checked in `CheckOverrides` against the raw
instruction (`Analysis::DecodeRaw`). Each patched line in the generated code gets a `// patched, was: ... (why)`
comment. Control-flow instructions are refused; an immediate must fit the operand size. Unit test
`tests/recomp_patch_test.cpp` (ctest `recomp_patch`). End to end: an unpatched run is byte-identical to the current
`generated/game`; a run with `0x1DD17 push 0x08 -> 16` and `0x2693DD disp 0x114 -> 0x3E0` changed exactly those two
lines; a wrong `expect` stops the recompiler with exit 1. No patch is in `game.toml` yet.

Small functions whose logic changes, not just a constant (the Players handler, the accessors), are C++ hooks.

## Phase 2: move the arrays instead of growing structs in place

Growing a struct in place shifts every field after the array, and fields after it (+0x3D4/+0x3D8/+0x3DC on the
race config, +0x28 on the player manager) are read in hundreds of places. Instead, each struct keeps its layout,
gets a bigger allocation, and **only the slot arrays move to the new tail**. The old in-struct arrays stay unused.

| Struct | Allocation | Array moves | Accesses to repoint |
|---|---|---|---|
| Race config x3 (`[0x53F2E0/E4/E8]`) | `push 0x3E0` x3 at 0x268D00/29/52 -> `0x3E0 + 2*N*0x2C` (0x968 for 16) | +0x114 -> +0x3E0; +0x274 -> +0x3E0 + N*0x2C | accessors sub_002693D0 (112 callers), 0x269410/30/50/70, 0x2694F0; ctor sub_0026B9E0 (loop + unrolled inits); copy sub_0026B2E0 (two 8-loops); ~46 functions with direct `i*0x2C + 0x114..0x3D3` sites (~84 sites; the list from the scan is in the appendix) |
| Player manager `[0x558814]` | `push 0x74` at 0x2BE2C0 -> `0x74 + N*4` | pointer array +0x08 -> +0x74 | getter sub_002BD910 (178 callers + sub_002BF570, 203 callers), sub_002BD920 (61); ~80 functions with direct `[mgr + i*4 + 8]` or `edi`-stepped (8, 12, ... 0x24) loops (~118 sites) |
| Race grid block | sub_001DC2F0 clamp 8 -> N (heap, already `n*0x5C`) | none | the clamp only, plus readers of `[ebx+0x127C]` |
| Lobby UI object | 8 pointers at +0x1118 (sub_001592B0) | needs its object size; likely moves to a tail too | to measure |

Step one of the phase is a better scanner than the regex pass used so far: track registers loaded from the three
config globals, from `[0x558814]` and from the accessors' return values through each function, and report every
memory operand with a displacement inside the moved ranges. Its output becomes the patch list, reviewed by hand. The
heuristic numbers above have false positives (other structs with a 0x2C stride) and probably misses.

Constant slot accesses also count: `[cfg+0x140]` is slot 1, field 0x00, and must move too. The scanner reports any
displacement in 0x114..0x3D3 on a config pointer, not only indexed ones.

**Status 2026-11-05: the scanner is done; no patches applied yet.** `tools/diagnostics/slot_scan.py` (tests:
`python -m unittest tools/diagnostics/test_slot_scan.py`, 13 tests) reads the disassembly in `generated/game`, runs a
forward data flow per function over registers and stack slots (esp/ebp frames, push/pop, callee `ret n`), and carries
struct pointers between functions through stack arguments, `this` (ecx), return values, and constructors (a function
whose result is stored into a struct global gets `this` = that struct: sub_0026B9E0, sub_002BE100). Jump tables follow
the generated `switch` case targets. It converges in 7 rounds (~50 s, 822 functions looked at, 142 take a struct
pointer, 40 return one).

Result: **328 sites**, all listed in [cruise_slot_sites.tsv](cruise_slot_sites.tsv) (va, function, kind, struct, array,
struct offset, current operand, operand for 16 slots, how the pointer was found, instruction):

| | disp | add | const | copy |
|---|---|---|---|---|
| race config | 217 | 5 | 1 | 2 |
| player manager | 52 | 0 | 51 | 0 |

- `disp`/`add` (274): a displacement or `add reg, imm` that steps from the struct base into an array. These map 1:1 to
  `[[patch]]` entries; `slot_scan.py --toml` writes them, with the new operand for `--slots N` (array 1 -> +0x3E0,
  array 2 -> +0x3E0 + N*0x2C, manager -> +0x74).
- `const` (52): a register set to a constant (`mov edi, 0x08`, `mov edi, 0x134`) and then used as the offset. The patch
  is the `mov`'s immediate, but each one needs a look: the loop that steps the register may also compare it against a
  bound. Most manager loops end on the count at `+0x28` instead (`cmp esi, [ecx+0x28]`), which doesn't move. 82
  "loop register" notes in 31 functions (`--notes`) list where a stepped register meets a struct pointer.
- `copy` (2): the config copy sub_0026B2E0 `rep movsd` at 0x26B3B5 (0x80 bytes from +0x68, the name area, not an
  array) and 0x26B4F8 (8 dwords from **+0xE8..+0x107**). That second one is a **third 8-entry table** in the config
  (outside both arrays); find its users before raising N.

Checked by hand: every accessor (0x2693DD, 0x26941D, 0x26943D, 0x26945D, 0x26947E, 0x269501), the manager getter
0x2BD914, the constructor (0x26B9E5 and the unrolled +0x274.. inits), the copy (0x26B457, 0x26B489, 0x26B48F), the
Players handler's `add ecx, 0x124` (0x13A3DB), and the `edi = 8` manager loops (0x1C2174, 0x1C28E9, 0x1C2E67, 0x1C2FE9,
0x1C3340, 0x1CC7CD, 0x1D047F, 0x20357, 0x1E8EF, 0x1EAE4). Against the heuristic appendix lists: of its 46 config
functions, the misses were 0x26E70 (a `cmovnz` load, now handled) and five that don't address the arrays (UI rows at
`[esi+edi*4+0x22C]`, other structs). Its manager list was mostly calls to the getter sub_002BD910, which need no patch.
The first run, which treated every instruction as a possible jump-table target, also reported the menu list widget
sub_00348BE0 and others as config users; those were false positives and are gone.

Known gaps (each needs a manual pass before the patches go in):
- Pointer differences: sub_00155AF0 computes `[0x53F2E4] - [0x53F2E8]` and adds it to an entry pointer (0x155B1F) to
  reach the same slot in the other config. It still works after the move (same layout in both), but nothing like it is
  checked automatically.
- Indirect calls (vtables, callbacks) don't carry struct arguments; neither do pointers stored into heap objects and
  read back elsewhere.
- Struct pointers kept in globals other than the four known ones, and any copy of the config embedded in another
  object or a network message (Phase 4).
- Constant registers that reach the struct through arithmetic the scanner doesn't model (`imul`, `shl`) appear as
  unknown index, which is fine for `disp` sites but would hide a `const` one.
- Struct offsets the move doesn't touch but which follow an array the code steps past (a loop over all 8 entries that
  ends by comparing a pointer to `base+0x274`): look for `cmp reg, imm` near the `const`/`add` sites.

## Phase 3: raise the bounds

- Session size: `push 0x08` at 0x0001DD17 -> N.
- The loops and clamps listed in cruise_player_cap.md (0x1653C9, 0x165DC9, 0x15B5B8, 0x1CF7CA, 0x1CC10A/0x1CC180,
  0x1D42C1, 0x1B0091, 0x1B0184, 0x1AE0A2, 0x1DF1AC, 0x25AAA, 0x2C3F4, 0x1853EA, 0x2BD882, 0x2BDE14, the edi=8
  loops on the player manager ...). Each needs a reason it is a slot bound; some 8s are something else (game types,
  pad counts).
- sub_001B0130 builds an 8-bit slot mask. Check where that mask is stored (a byte field would cap at 8 and needs
  widening; a dword is fine).
- **Players handler sub_0013A250 becomes a C++ hook**: same three branches, max = N when the menu's mode is Cruise,
  8 otherwise. Writes `+0x4C`, `+0x50` and the transport's `+0x414C` as the original does. Find the Cruise mode id
  first (`[cfg+0x18]`; sub_0013A200's jump table at 0x13A238 covers modes 1..21; StartCruise is pushed at 0x161E53
  and 0x16C685).
- Changing the mode in Create Match from Cruise to a race must clamp `+0x4C` back to 8.

## Phase 4: wire formats

Nothing about the message layouts has been traced yet (opencode_online_progress.md says the 0x40..0x68 message-id
claim is unproven). Hypothesis worth checking first: the 400-byte join message = 48 bytes + 8 x 0x2C (352) player
entries, i.e. the race config's player array is sent whole. If so, the serializer has an 8 bound and a fixed size.
Work:
1. Record a 3-copy session with the tunnels (`--record`) and diff join messages as players are added.
2. Find the serializers: break on the transport's send with the config's array address in a register.
3. Raise their bounds and sizes, keeping each datagram under the XNet/VDP payload limit (16 x 0x2C + 48 = 752
   bytes, fine).
4. Add a capacity byte to the discovery reply (172 bytes, type 0x3) or reject on size, so 8-slot and N-slot builds
   don't join each other.

## Phase 5: everything else a 9th player touches

Not yet looked at; each needs a check before the first 9-player run.
- Cruise spawn points (`CruiseStartData` 0x474098, used at 0x2806A2/0x2831EE): how many start positions it has.
- Per-slot colours/names: `LineColor%d`, `CursorTeamColor*`, name tags, minimap icons. Tables of 8 will read past
  the end for slot 8+.
- Lobby list (fixed rows, `playerListHeight`), lobby details panel, results screen (`HS_NumPlayers`, loop bound 10).
  Needs smaller rows or scrolling for 16.
- Car model and texture memory: Xbox-era pools sized for 8 cars plus traffic. Check the heap the port gives the
  guest and lower Cruise traffic density if needed.
- Audio voices per car (engine sounds), replay buffers, ghost/recording code if it is per slot.
- Saves: the race config is in memory only, as far as known; confirm nothing saves it into `UDATA`.

## Phase 6: test

1. Build with N = 8 and every patch applied (all moves, no bound changes): 3-copy Cruise and a 3-copy race must
   behave as today. This proves the moves without changing behaviour.
2. N = 16: 3 copies, Cruise and a race, then 9 copies in Cruise (instances 1-6 and 8 give 7; two more need new
   instance numbers 9+; **never 7**). Check: everyone spawns, sees all cars, names and colours right, leaving and
   rejoining works, no crash after 10 minutes.
3. Races: Players cycles to 8 at most, a race with 3 copies starts and finishes, rankings on mc3.top still count.
4. Memory: 9+ copies on one PC may not fit; if so, test with two PCs or a fake-peer harness that replays recorded
   join/state messages from the relay.

## Size of the job

Phase 1: one session (recompiler feature + test). Phase 2 scanner: one session. Phases 2-3 patch list and hooks:
the largest part, several sessions with builds between. Phase 4: one or two sessions, needs live captures.
Phases 5-6: depends on what 5 finds. Realistically a multi-day piece of work, with the risk concentrated in
Phase 2 (a missed access is silent memory corruption for slot 8+) and Phase 5 (unknown fixed tables).

## Alternative

Keep each System Link session at 8 and let the relay fill one Cruise room with several 8-player sessions (players see
the room's chat and rankings, but cars only from their own session). No game change; one evening of relay work.
Worth doing first anyway, since it gives "no limit" in the room today and the resize can follow.

## Appendix: scan output (heuristic)

Race config, functions with indexed array access: 0x24420 0x24539 0x246b3 0x257e0 0x259d5 0x25e48 0x26370 0x26848
0x26e70 0x2d6f6 0x2df80 0x2e8e0 0x2ecc8 0xb1b70 0xb64e0 0xb6a90 0xb7080 0xd11a0 0xdbf70 0xfdc60 0x115ab0 0x117880
0x13ab4d 0x13b2e0 0x13ba60 0x13c310 0x1403c0 0x155920 0x15b5e0 0x15baea 0x15be30 0x15e660 0x18bc47 0x19a57d
0x1cd9f9 0x1de4b0 0x269080 0x2693d0 0x269410 0x269430 0x269450 0x269470 0x2694f0 0x2bd8a0 0x2be100 0x332460

Player manager, functions with direct indexed access: 0x1ddcd 0x20260 0x217b0 0x23c80 0x23d50 0x24c70 0x24cc0
0x24f50 0x25220 0x253d0 0x25580 0x256e0 0x25e48 0x269b0 0x26ad0 0x26c20 0x28c00 0x2b0e0 0x2f5e0 0x31fd0 0x35740
0xd6bd0 0xd6fcd 0xf32a0 0xfdc60 0x12350a 0x129070 0x134690 0x164ca0 0x1655e9 0x1657da 0x16599d 0x16a9f0 0x16cb7a
0x1906a0 0x198f0a 0x1c1dc0 0x1c20e0 0x1c28f6 0x1c2cc0 0x1c2ef0 0x1c3347 0x1c4480 0x1c5ee8 0x1cb7d0 0x1cba50
0x1cbab7 0x1cc450 0x1cc7b0 0x1cc910 0x1cf4ca 0x1cf58a 0x1d0487 0x1d0710 0x1d0800 0x1d15b0 0x1d1870 0x1d69d0
0x1d7bf0 0x1da0f0 0x1da299 0x1dc170 0x1dce50 0x1dcfa0 0x1dd049 0x1dd548 0x1dd988 0x1de4b0 0x1dfc00 0x1e06e0
0x1e5570 0x1ea3b0 0x1ea680 0x1eba20 0x1ecac0 0x1efcc0 0x1f4790 0x225e70 0x2c8710 0x3337c0 0x3641b0

(Some addresses are mid-function block starts from the function map, not function entries.)
