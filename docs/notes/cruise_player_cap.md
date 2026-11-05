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

## Next steps

1. Find the Create Match option table: the code that cycles `Players:` with left/right and clamps to 4 or 8 by
   mode (look for the mode index compared to the Cruise value next to clamps of 4 and 8).
2. Find the per-player arrays' real size (the session/peer table that `XNet` join fills, and the car array the
   cruise spawns into). If they are larger than 8 (some Xbox titles size for 16), raise the Cruise clamp to that.
3. If they are exactly 8, resizing them is a large change: list every table indexed by slot first.
4. Test with three or more copies (instances 1-6, 8; never 7) in a private room.
