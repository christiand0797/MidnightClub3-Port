# Replays (Instant Replay): how MC3 does them, why they failed, the fix

2026-10-22. Static analysis of default.xbe plus the user's log of 2026-10-21 18:01. The game was not launched.
Code: `templates/game/src/game/replay.cpp` (new) and the fixed-step branches of `sub_0020B6F0` in
`templates/game/src/game/hooks.cpp`. Built OK; `tests/timing_test` still passes; a scratch test that runs the real
hooks.cpp + replay.cpp against mocked guest bodies (record with late frames, play back with different late frames,
check every step is identical, buffer hook ABI) passes.

## How the replay works

MC3 does not store car positions. A replay **re-simulates the race** from the same random seed with the recorded pad
input, and checks the result against recorded positions.

| What | Where |
|---|---|
| Replay object (0x40 bytes, vtable 0x449050) | global `[0x4CBAE0]`, created by 0x23C10; recorder at +0x0C; +0x38 = replay ended ("EndReplay" menu item instead of "ContinueReplay", 0x16C038) |
| Recorder (vtable 0x44A4A8) | ctor 0x2D870, dtor 0x2D8E0; +0x08 random seed; +0x10 frame counter for the check; +0x14 **diverged**; +0x18 stream object; +0x1C..+0x28 buffer {base, cursor, capacity, used} |
| Buffer | `0x1DA90` (manager `[0x4CB6D0]`, thiscall, `int* size`): **256 KB** (0x40000) allocated once by 0x1DC00 via 0x20DB20(0x40000, 0x80) |
| Start recording | game state 2 entry 0x21170 -> 0x23DC0 -> 0x2D960: format 1, 0x0F bytes per frame (0x1F two players) via 0x20E220; open 0x20EA20 (header "RPY1"); new seed = MSVC LCG of `0x4B2D00`, kept at recorder+8 |
| Start playback | state 3 entry 0x212E0 -> 0x23D50 -> 0x2D900: `0x4B2D00` = recorder+8 (same seed), open 0x20EAC0 (checks "RPY1") |
| Stop/cancel | 0x23DD0 -> 0x2D9E0 (clears stream globals and the recorded frame count) |
| Per frame | game frame 0x1F9D0: 0x23E30 (frame begin 0x20E630 via 0x2D9F0; if the recorder is diverged -> 0x23C80 EndReplay) ... time manager 0x20B6F0 ... simulation ... 0x23D00 -> 0x2DAC0 (frame end 0x20EB90 + position check) |
| Stream globals | `0x534AC4` stream object (0 = closed), `0x534A9C` 1 = playing / 0 = recording, `0x534AC8` format, `0x4B2DC8` bytes per frame, `0x534AD4` frames recorded, `0x534AD8` frames played (playback ends when equal) |
| Stream encoding | per frame the changed bytes of a 15-byte record with a change mask; unchanged-mask frames only bump a run count (RLE). Writes past the capacity are silently dropped and stop the frame count (0x212510), so a full buffer = the replay ends there |
| What is recorded | pad bytes (0x2C9500 / 0x35FA10 write, 0x2C9260 / 0x35F7A0 read), time scale `0x4B2D6C` as a 16-bit value (0x1EDB0), frame time **only in measured mode** (0x20B7DB..0x20B8C5). **In fixed step mode the step is not recorded** (on the console it was always 1/(rate*substeps)) |
| Position check | 0x2DAC0 every 30 frames: recording writes the player car position (3 floats), playback reads them and compares **bit for bit**; a mismatch sets recorder+0x14, the next frame ends the replay |

## Root cause

The port runs races at a 60 Hz fixed step (0x20BB00 rate doubling) and, in `sub_0020B6F0`, makes a frame the PC was
late with advance 2..4 fields (with a carried remainder) so race time keeps to real time. That makes the fixed step
vary frame to frame, differently while recording and while playing back. Because the step is not in the stream, the
re-simulated race diverged at the first position check after the first late frame and the game ended the replay.
Race starts nearly always hitch, so this was the first check, about half a second in.

The log matches this exactly: "InstantReplay" at 264.57, replay start (fixed step requests) at 267.50, the Replay menu
with "RestartReplay" already at 268.28 (0.8 s later, i.e. the EndReplay menu), then restarts every ~1 s (271.79, 272.97,
273.92, ...) each with a new music pick, each ending after a moment.

The 256 KB buffer was also sized for 30 frames/s; at 60 it holds about half the race time it did on the console, so
long races would have been cut short at the end (secondary).

## Fix

- `replay.cpp` keeps the step (`0x4B2D58` bits) each recorded frame ran with, keyed by the stream's own frame number
  (`0x534AD4` while recording, `0x534AD8` while playing; both count completed frames, so frame k matches frame k;
  a frame seen twice, e.g. paused, keeps its last value). `hooks.cpp` calls it (through `g_replay_fixed_step`, so the
  timing test still links hooks.cpp alone) in both fixed-step branches after choosing the step: recording stores it,
  playback forces the recorded value (one 60 Hz field, or the game's own step with 60 fps off, if none) and drops the
  real-time carry. Live races keep the real-time compensation unchanged; without an open stream nothing changes.
  The step is stored as exact bits, so a replay plays back right even if 60 fps was toggled in between (it then runs
  at the host cap, i.e. slower on screen, but stays in sync).
- Hooks on 0x20EA20 / 0x20EAC0 clear the step record on a new recording and log playback starts.
- `sub_0001DA90` returns a 1 MB buffer from system memory (`mem::AllocSystem`, allocated once) instead of the stock
  256 KB: 2x for the doubled step rate, 2x headroom. Only the recorder's constructor (0x2D887) asks for it.
- Log lines (`templates/game/out/native/logs/mc3.log`):
  - `replay: recording (1024 KB buffer)` at every race start;
  - `replay: playing N frames (X of 1024 KB), steps known for N, M of them late frames (longer steps)`;
  - `replay: playback left the recording at frame K of N (position check), the game ends the replay` (should not
    appear any more; if it does, there is another source of non-determinism, see below);
  - `replay: played to the end (N frames)`;
  - `replay: the recording buffer is full after N frames` (very long races only).

## What to test (user)

1. Finish a race, choose Instant Replay: it should play the whole race to the end ("Replay Over"), not stop after a
   second. Check the log for `played to the end` and no `left the recording`.
2. In the Replay menu choose Restart Replay a couple of times: each run must be identical and complete.
3. A long race (Tokyo highway / tournament) replay reaches the finish.
4. Live race timing must be unchanged (timing lines still x1.000).
5. Optional: toggle 60 fps off between race and replay; the replay should still stay in sync.

## Remaining risks

- If `left the recording` still appears, something else in the port is non-deterministic between two runs of the
  same race (candidates: tuning values changed in F1 between race and replay — `ApplyVehicleTuning` writes them every
  frame; guest threads such as streaming affecting simulation; the frame-time adaptive value at 0xA18C2 that reads
  the raw frame time `0x4B2D74`, which looks render-side but is unverified). The frame number in the log line tells
  how far it got.
- During playback a recorded late frame replays as a 2-field jump even if the PC is on time (it is what happened
  live); a late frame during playback slows the replay slightly instead of skipping (needed for exactness).
- The step record lives in host memory only; MC3 has no replay saving (the "replay" string at 0x476A6C is a camera
  name, "replay_chase1"), so nothing else needs it.

## 2026-10-29: still ended at frame 30; now plays on (Claude session)

Native check (Tokyo Arcade cruise, about 20 s recorded): every frame's step was recorded and forced (`steps known for
N of N`), yet the first position check (frame 30) failed and the game ended the replay. A per-frame trace of the
player position (`replay.cpp`, logged once per replay as `replay: frame K first differs from the recording`) showed:

- some sessions differed from frame 0 by about 2 ULPs in x, others matched for 360+ frames and then drifted by
  1e-6..2e-5; the recording itself started from different values across sessions. The step, frame time (now set to
  one field at every stream open, `CanonicalStart`) and pad input match, so the remaining source is timing-dependent
  (most likely world streaming on other threads racing the simulation). Making it bit-exact is a larger project.
- So when a playback check fails, `sub_0002DAC0` clears recorder +0x14 and the replay plays on (logged once:
  `playback drifted ... playing on`); the stream stays in step because the check still consumes its positions.
  Result: a 1244-frame replay played to the end (`replay: played to the end (1244 frames)`), drift starting at 2e-6.
- Note the "view a replay?" prompt defaults to No.

### Later the same night: the car is kept on its recorded path

Playing on still let the player car slowly wander from what was driven. `replay.cpp` now stores the player car
transform at the end of every recorded frame (the Matrix34 holding the checked position; `TransformSpan` tells a
3x4 from a 4x4 by unit rotation rows and logs which, `3x4` in MC3) and writes it back at the same point while playing,
before the game's check. Result: a 1600-frame Tokyo replay with 22 late frames played to the end with no drift flag;
the trace still reports the underlying float drift (frame 475, 3e-6) that each frame's correction removes. Memory is
68 bytes a frame (about 2.4 MB for ten minutes). The play-on fallback remains for a frame the transform could not be
read.
