# Cruise player cap (System Link) — research notes, 2026-11-05

Goal from the user: Cruise sessions with no player limit; race types keep their limits (races 4, Cruise 8 today).

## What is already unlimited

The relay side has no cap: the Main Lobby and named rooms accept any number of players (`maxPlayers: 0`), and
every System Link match hosted in a room is visible to everyone in it. The 8/4 limits are inside the game.

## What the game does (from `templates/game/assets/default.xbe`, recompiled in `generated/game/x86_recomp.*.cpp`)

- Create Match shows `Players: N` per mode (Track Race showed 4; Cruise allows up to 8).
- String addresses: `HS_NumPlayers` 0x45F2D0, `Players` 0x45C5BC, `d_Players` 0x457DAC, `players: %d` 0x4615FA,
  `StartCruise` 0x45D444, `CruiseStartData` 0x474098.
- `HS_NumPlayers` is used only at 0x001737A8 inside sub_001736C0, which builds a results/score table: it adds
  columns via 0x00348D70 with widths 6/8/10 and loops over players (`cmp eax, 0x0A` bound at 0x001738B4). It is
  a display, not the session cap.
- `Players` (0x45C5BC) is referenced at 0x0015C640 in the lobby-details panel (strings formatted into rows at
  `[esi+edi*4+0x22C]`, one row per field). Also display.
- The session's player count travels in the System Link messages the tunnels record (`--record`); message types
  0x2/0x3 (discovery request/reply, 8 and 172 bytes) and 0x4/0x6 (join, 400/412 bytes) are the candidates for the
  advertised maximum.

## Why it is not a one-constant patch

Every racer in a session owns per-player engine state (car, input, damage, network slot). The recorded traffic uses
a one-byte slot (`CHECKPOINT` byte 0, see `tools/multiplayer/relay/race_results.js`), and the lobby UI has fixed
rows. If those per-player arrays are sized 8, a menu limit above 8 would overrun them. "Unlimited" in one game
session is therefore bounded by the engine's arrays; going past them means resizing every per-player table.

## Findings, 2026-11-05 (second pass)

### The `Players:` option (Create Match)

- Row built at 0x0013BDE5 (string `CM_%d_Players` 0x457DA8), value from `[0x53F2E8]+0x4C`. Its left/right handler
  is **sub_0013A250**. Three branches:
  - Hosting a live System Link session (`[0x557EFC]+0x10 >= 0`): low bound = max(joined players via sub_0005C9B0,
    highest joined slot + 1, 2); cycles up to **8** (`cmp edx, 0x08` at 0x13A2AF, `mov ecx/esi, 0x09` at
    0x13A2C7/0x13A2D2). Writes `[cfg+0x4C]`, `[cfg+0x50]=1` and the transport's `+0x414C` (advertised max).
  - System Link, before the session exists (`[0x50F14C] != 0`): `((n-2) mod 7) + 2`, i.e. **2..8**
    (0x13A323..0x13A343).
  - Split screen: 1..(local pads, max 2).
- **No branch reads the game mode.** The handler gives every mode the same 2..8 range; the 4 seen on Track Race is
  the default written at 0x0016CCAA (`mov dword ptr [eax+0x4C], 4`), not a clamp. (Not yet re-checked in game.)
  So "races keep 4" is not something the game enforces today; Cruise and races both stop at 8.

### Network session: parameterised, not the limit

- sub_0001DD0E creates the session with `push 0x08` at **0x0001DD17** -> sub_00024300 (object 0xC4 bytes,
  stored in `[0x557EFC]`) -> base ctor sub_0005CD20 stores the slot count at `+0x08` and allocates `count*4`
  pointers plus one 0x64-byte peer object per slot (sub_001AF9F0). Joined slots are a 32-bit mask at `+0x14`.
- The transport (sub_0002DED0, 0x42A0 bytes: 0x4000 packet buffer, slot count `+0x4140`, max `+0x414C`, joined
  mask `+0x4150`, local slot `+0x4158`) is built from the same count.
- So the net layer would take up to 32 slots by changing that one immediate. Everything above it is fixed at 8.

### Game-side per-player tables: exactly 8 (resizing NOT done)

| Table | Where | Size | Evidence |
|---|---|---|---|
| Race config x3 (`[0x53F2E0]` live, `[0x53F2E4]`, `[0x53F2E8]` menu), 0x3E0 bytes each | sub_00268D00 allocs, ctor sub_0026B9E0 | 8 entries x 0x2C at `+0x114` (to 0x274) and 8 x 0x2C at `+0x274` (to 0x3D4) | ctor loop `mov ebx, 8`, unrolled 8 inits at +0x274 |
| In-race player manager `[0x558814]`, 0x74 bytes | sub_002BE2C0 / ctor sub_002BE100 | 8 player pointers at `+0x08..+0x27`, count at `+0x28` | getter sub_002BD910 `[ecx+i*4+8]`; loops at 0x1C2174, 0x1C28E9, 0x1C2E67, 0x1C2FE9, 0x1C3340, 0x1CC7CD, 0x1D047F, 0x20357, 0x1E8EF/0x1EAE4 step `edi` 8,12,..0x24 |
| Race grid/results block `cfg+0x40` -> heap `n*0x5C` | sub_001DC2F0 | clamps n to 8 | `cmp esi, 8 / mov esi, 8` at 0x1DC2FF |
| Lobby/UI object, 8 pointers at `+0x1118` | sub_001592B0 | 8 | `mov edi, 8` frees at 0x159323 / 0x15937E |
| Lobby player rows, stride 0xB8 | sub_00165280 | 8 | `cmp esi, 8` at 0x1653C9 |
| Per-slot reset of cfg entries | sub_00165D40 | 8 | calls sub_0026B170 for i<8 (0x165DC9) |
| Lobby details panel rows `[esi+edi*4+0x22C]` | 0x0015C640 | fixed rows | (display) |
| Slot index range checks | sub_0015B5B0 (`0<=i<8`), sub_001CF6F0 (clamp 1..8), sub_001CBDAA (`<=8`, loop <8), sub_001D3D59 (clamp 8) | 8 | |
| Net-side 8-loops on the session | sub_001B0020, sub_001B0130 (builds an 8-bit mask), sub_001AE040, sub_001DF179 (`[ebx+0x1258]` mask) | 8 | |
| Other 8-loops near player code | sub_000259D5 (stride 0xB0), sub_0002C38F (`[edi+esi*4+0x14]`, i<=8), sub_001853D0 (8 dwords at 0x4B2634), sub_002BD810 / sub_002BDC16 | 8 | |
| Results screen `HS_NumPlayers` | sub_001736C0 | loop bound 10 | display |
| Wire format | CHECKPOINT byte 0 = slot; join messages 400/412 bytes | 1-byte slot; per-slot payloads unknown | `tools/multiplayer/relay/race_results.js` |

(Found by scanning every function that touches `[0x53F2E0/E4/E8]` or `[0x557EFC]` for an 8 bound; 41 functions,
listed above where the meaning is clear. Functions that index the race config through `+0x114 + i*0x2C` or the
player manager through `+0x08 + i*4` without such a function nearby are not all listed.)

### Conclusion

Raising the Cruise limit above 8 is not a clamp patch. It needs, at minimum: the race-config struct grown (0x3E0
-> new size, with both 8-entry arrays and everything after them at +0x274/+0x3D4/+0x3D8 moved), the player manager
grown (0x74, count field at +0x28 moved), every `8` bound above changed, the session created with a larger count
(0x1DD17), and the lobby UI given more rows; the save/wire formats that embed these structs would have to be checked
too. Per the brief, nothing was resized; this list is the report. A smaller alternative would be keeping the game at
8 per System Link session and having the relay run several 8-player sessions in one Cruise room.

## Next steps (original)


1. Find the Create Match option table: the code that cycles `Players:` with left/right and clamps to 4 or 8 by
   mode (look for the mode index compared to the Cruise value next to clamps of 4 and 8).
2. Find the per-player arrays' real size (the session/peer table that `XNet` join fills, and the car array the
   cruise spawns into). If they are larger than 8 (some Xbox titles size for 16), raise the Cruise clamp to that.
3. If they are exactly 8, resizing them is a large change: list every table indexed by slot first.
4. Test with three or more copies (instances 1-6, 8; never 7) in a private room.
