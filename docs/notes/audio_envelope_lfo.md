# DirectSound envelopes and LFO: implementation evidence

Updated 2026-10-21. Research only; no audio source changes, native build, or game launch. This continues the observed `buffer envelope modulation is not implemented` log at 31.660 s. Current `dsound.cpp` still discards EG/LFO descriptors. Do not mark them implemented.

## Confirmed descriptor and selector

The open Cxbx declarations describe a 40-byte little-endian EG descriptor; every field occupies four bytes. Offsets: generator `+00`, mode `+04`, delay `+08`, attack `+0C`, hold `+10`, decay `+14`, release `+18`, sustain `+1C`, signed pitch scale `+20`, signed filter-cutoff scale `+24`. This is an encoded hardware-style descriptor, not floating-point ADSR durations. [Cxbx descriptor and flag definitions](https://github.com/Cxbx-Reloaded/Cxbx-Reloaded/blob/585c49a50af1255ab155099e06f24505f9c5a800/src/core/hle/DSOUND/XbDSoundTypes.h).

The title's own captured generated original code independently exposes the packing at `sub_003C6F1B`, in `templates/game/generated/game/x86_recomp.113.cpp:8702`:

| Descriptor | Native-library storage/PIO written | Meaning established by register definitions |
|---|---|---|
| Generator **0** | `this+40` bits 24–27; `+24/+28`; release `+18`; scales bytes `+1F/+27` | **EF: filter/pitch envelope** |
| Generator **1** | `this+40` bits 28–31; `+1C/+20`; release `+30` | **EA: amplitude envelope** |
| attack / delay | low 12 bits / next 12 bits of relevant envelope register | 12-bit encoded timing |
| decay / hold / sustain | low 12 bits / next 12 bits / top byte | 12-bit rates/times; 8-bit sustain |
| pitch / cutoff | `mov dl,[descriptor+20]` / `mov dl,[descriptor+24]` in generator-0 branch | Low-byte scales; signed interpretation needed by modulation |

Do **not** assume generator 0 is amplitude. Unknown generator values skip both descriptor-copy branches and the original function eventually returns success; a replacement should avoid modifying either envelope in that case. The selector/packing observations above come from the generated original library instructions, not a behavioral emulator assumption.

The writes at guest `0x3C7051..0x3C7081` send cached words to `0xFE820318`, `308`, `30C`, `310`, `314`, `36C`. Those methods map to MISC, ENV0, ENVA, ENV1, ENVF, LFO_ENV; EA and EF state nibble names and widths are defined in [xemu APU registers](https://github.com/xemu-project/xemu/blob/bf817de2b0354b7afe5e70efcd4f9c53bc7d4c67/hw/xbox/mcpx/apu/apu_regs.h#L251). These definitions also establish phase encoding: 0 off, 1 delay, 2 attack, 3 hold, 4 decay, 5 sustain, 6 release, 7 force release. Descriptor modes are stored into the matching nibble; their exact update/retrigger behavior while a voice is already playing still needs a focused original-library start-path analysis.

## What MC3 actually supplies

`templates/game/generated/game/x86_recomp.121.cpp` contains the descriptor construction at guest `0x40944B..0x4094B8`:

- It zeroes ten dwords, fills attack/decay/delay/hold/release from unsigned 16-bit source fields, mode and sustain from bytes, then selects generator 0 for command type `0x0B` and generator 1 otherwise in this branch.
- Generator 0 additionally sign-extends the bank event's pitch/cutoff bytes into the descriptor. Generator 1 leaves those scales zero.
- At `0x4094B8` it calls `sub_0040F663`. That common wrapper chooses buffer SetEG (`0x3C20C8`) or stream SetEG (`0x3C22A3`). Both paths therefore need consistent descriptor handling.
- Buffer SetEG's captured original wrapper adjusts the interface pointer by `-0x1C`, calls `0x3C1A59`, then `0x3C0C0A`, which calls `0x3C6F1B` on its hardware voice object.

This makes an amplitude-only handler insufficient for every title event: the same construction code supports both independent envelope generators. No live descriptor values were captured during this research, so it does not establish which mode or timings triggered the recorded log.

## Timing and curve evidence; distinguish model from certainty

Cxbx's StopEx implementation computes release duration as `dwRelease * 512` samples at 48 kHz before converting to 100-ns ticks. Thus one encoded release unit corresponds to about **10.6667 ms** in that implementation. Cxbx SetEG itself only saves the descriptor and logs unsupported; it is not evidence of a working ADSR implementation. [Cxbx buffer SetEG/StopEx](https://github.com/Cxbx-Reloaded/Cxbx-Reloaded/blob/585c49a50af1255ab155099e06f24505f9c5a800/src/core/hle/DSOUND/DirectSound/DirectSoundBuffer.cpp).

[xemu voice processing](https://github.com/xemu-project/xemu/blob/bf817de2b0354b7afe5e70efcd4f9c53bc7d4c67/hw/xbox/mcpx/apu/vp/vp.c) maintains separate EA/EF phase counters. Delay/hold/attack/release use encoded units multiplied by 16 processing frames. Attack rises linearly; sustain is byte/255; EA multiplies output gain. EF contributes `signed_pitch_scale * 32 * envelope_value` to the logarithmic pitch (4096 units/octave). Paused voices skip envelope stepping. Voice-on initializes selected phases; release changes both envelopes and EA completion disables the voice. Its decay/release curves, zero-duration edges, repeated release, and cutoff modulation have explicit accuracy/TODO comments. Treat those as approximations requiring validation, not hardware guarantees. In particular, a generic linear ramp to sustain would disagree with this exponential decay model.

The port currently advances sample position using `Voice3DData::PitchRatio()` and mixes in output-sample time. A proposed host implementation should advance envelopes on **48-kHz output time**, independently of source rate/pitch, while freezing paused/synchronization-held voices. This is an engineering proposal; tests must verify it against the original start/release paths.

## Release semantics and status to implement together

Cxbx declares buffer StopEx flags immediate=0, envelope=1, release-waveform=2; stream FlushEx has async=1, envelope=2, envelope2=4, and stream envelope-complete status bit `0x00080000`. These contracts are absent from the port's immediate-stop/flush bodies. The descriptor definition source linked above provides these constants; its presence does not settle all combinations.

Before enabling EG audibly, complete these connected behaviors:

1. Store **two** independent descriptors/phase states per host Voice. Apply the actual masks and selector; generator 0 must never mute amplitude by mistake.
2. Recover original Play / Pause / Process activation handling to determine phase reset versus continuation. SetEG changes configuration, not necessarily a fresh attack on every call. The low-level original function does not itself issue a VOICE_ON method.
3. Implement StopEx scheduling and release-waveform loop exit while samples continue during the release. Decide when playing/status drops using the original code, not an unconditional immediate stop.
4. Handle stream FlushEx completion/callback ordering and envelope-complete status without releasing guest packets while they are still read. Streams must not restart attack at every packet boundary.
5. Keep EA gain separate from EF pitch and filter scales. Pitch contribution can use the evidenced logarithmic units; filter cutoff requires the filter path, which is currently also a no-op in `dsound.cpp`. Do not silently apply EF cutoff as volume.

## LFO evidence and unresolved mappings

The same Cxbx declarations define a 24-byte DSLFODESC: `dwLFO`, `dwDelay`, `dwDelta`, `lPitchModulation`, `lFilterCutOffRange`, `lAmplitudeModulation`. Cxbx's buffer/stream SetLFO handlers are also unsupported. No authoritative complete waveform/frequency/phase/retrigger mapping was recovered here. The field name `dwDelta` must not be interpreted as Hz by guesswork. A later investigation should follow original `0x3C20AC`/`0x3C229E` wrappers into their packing routine and observe a real title descriptor before implementation.

## Proposed meaningful isolated checks

Tests should cover generator independence; EA-off unity gain and the separately validated EF-off contribution; 12-/8-bit truncation; signed pitch scaling; fixed output-time advancement across 22.05/44.1/48-kHz source formats; delay/attack/hold/decay/sustain/release transitions including zero duration; pause/sync freeze; resuming versus Play-from-start; releasing from attack and sustain; repeated release; loop exit; stream packet boundaries and callback/status ordering. Curve tests must state which original or reference model they validate.

## Evidence lifecycle

Reference source downloads are saved under `out/analysis/audio-reference/`; source URLs are pinned above. Original PE identity: SHA256 `010e2ce8c8b82bddc51f0ccd008a16135422d8aa6dc19afa79fdcecef83490df`. Fresh REA auto-analysis finished after the report draft. Ghidra evidence `ev_6db1050b3096495a4757bec8aef096e866337dd31a701be68e6c9472f2a1e3a2` confirms the original buffer wrapper at `0x3C20C8` is 28 bytes, adjusts the nonnull interface pointer by -0x1C, and calls `0x3C1A59`; its caller is `0x40F663`. The deeper packing observations remain explicitly labeled captured generated original code, not fresh decompiler evidence. The queued `close_binary` completed successfully and `binary_session` verified `open: false`. Session close cleared the transient records before export, so there is no saved fresh Evidence bundle; the evidence identifier and bounded wrapper finding are retained here.

## Implementation (2026-10-21)

This section supersedes the "research only / do not mark implemented" status at the top: the envelopes and both
LFOs are now implemented in `runtime/audio/modulation.h` (new, header only, so no CMake change) and
`runtime/xdk/dsound.cpp`. Not built or run by this pass. `dsound.cpp` passes clang++ `-std=gnu++20 -Wall -Wextra
-fsyntax-only` with the build's own include paths and defines (an injected error was caught, so the check is real),
and a scratch g++ test of `modulation.h` checked: unity gain and zero pitch for a voice with no SetEG/SetLFO, zero
release time ending the voice on the next frame, the delay/attack/hold/decay/sustain sequence (sustain 128 gives
0.502), a release time of 2 units lasting 32 frames, the pitch LFO range and the tremolo range.

### More evidence from the XBE (capstone over `templates/game/assets/default.xbe`)

| Guest address | What it is | What it settles |
|---|---|---|
| `0x3C20C8` → `0x3C1A59` → `0x3C0C0A` → `0x3C6F1B` | buffer SetEG chain | `0x3C6F1B` sends MISC/ENV0/ENVA/ENV1/ENVF/LFO_ENV (PIO `0x318,0x308,0x30C,0x310,0x314,0x36C`) only while the voice is allocated (`[client+0x12] & 1`), otherwise only its cache changes |
| `0x3C22A3` (jmp) → `0x3C1D6F` → `0x3C0C0A` | stream SetEG (IDirectSoundStream thunk) | same voice routine, on stream object + 4 |
| `0x3C20AC` → `0x3C1A0B` → `0x3C0BF7` → `0x3C6DEC` | buffer SetLFO chain | LFO packing below |
| `0x3C229E` (jmp) → `0x3C1D1D` | stream SetLFO | same packing |
| `0x3C7CB5` | voice on | returns early when `[client+0x12] & 2` (voice already running); writes PIO `0x124` VOICE_ON = handle OR (`[client+0x40]` & `0xFFFF0000`): the EG modes stored by SetEG are read **only at voice on** |
| `0x3C6ABF` | voice release | sets state bit 8 and writes PIO `0x12C` VOICE_RELEASE; it does not check for a release already running |
| `0x3C78AA` | voice off | PIO `0x128` VOICE_OFF |
| `0x3C87ED` | CMcpxBuffer StopEx(flags), reached by `0x3C2174` → `0x3C101B` → `0x3C8A56` | flags & 1 (DSBSTOPEX_ENVELOPE): when also & 2 and the buffer loops, Play(0) leaves the loop (`0x3C86D4`), then release. Without flag 1: immediate voice off |
| `0x3C4F37` | CMcpxStream flush(flags); FlushEx is `0x3C53E8`, Flush `0x3C5526` passes 1 | flags & 6: release; & 4 also sets state bit `0x1000` and keeps the packets; without 4 the packets are flushed at once |
| `0x3C86D4` | CMcpxBuffer Play | voice on goes through `0x3C7CB5`, so Play on a voice that is still releasing does not restart its envelopes |
| `0x40F229` (XACTENG) | XACT voice stop | buffer StopEx(0, 0, release ? 3 : 0); stream FlushEx(0, 0, release ? 5 : 1) through `0x3C22BC` |
| `0x4094C2..0x409527` (XACTENG) | XACT LFO event | event type 8: DSLFO_PITCH with delay (u16 at +8), delta (u8 at +0xA, shifted left 2), pitch depth (s8 at +0xB); other types: DSLFO_MULTI with delay, delta, pitch (+0xB), cutoff (+0xC), amplitude (+0xD). It calls `0x40F6A9`, which picks buffer `0x3C20AC` or stream `0x3C229E` |

The client object never initialises its register cache (the constructor at `0x3C7845` leaves it as allocated, zero),
so a voice that never had SetEG has every mode 0 (off) and release time 0, and VOICE_RELEASE turns it off at once.

### DSLFODESC packing (`0x3C6DEC`), offsets in bytes

| Field | Offset | DSLFO_MULTI (0) → LFOA | DSLFO_PITCH (1) → LFOF |
|---|---|---|---|
| dwLFO | +00 | selector; any other value changes nothing and returns 0 | |
| dwDelay | +04 | MISC bit 14 = (dwDelay != 0); LFODLY (PIO `0x350`, cache +0x2C) bits 0-14 | MISC bit 15; LFODLY bits 16-30 |
| dwDelta | +08 | LFO_ENV bits 12-21 (10 bits) | LFO_ENV bits 22-31 |
| lPitchModulation | +0C | LFO_MOD (PIO `0x370`) byte 1 | LFO_MOD byte 3 |
| lFilterCutOffRange | +10 | LFO_MOD byte 2 | not written |
| lAmplitudeModulation | +14 | LFO_MOD byte 0 | not written |

### Host model (what the code does)

- Each `Voice` holds an `audio::Modulation`: two `Envelope`s (generator 0 multi, generator 1 amplitude), generator 0's
  signed pitch and cutoff scales, and two `Lfo`s (multi, pitch). `SetEG` / `SetLFO` in `dsound.cpp` copy the fields
  with the hardware widths (12-bit times, 8-bit sustain and depths, 4-bit mode, 15-bit LFO delay, 10-bit delta).
  Buffer `SetEG`/`SetLFO`, `CDirectSoundStream_SetLFO` and `IDirectSoundStream_SetEG` all use them. Times, sustain,
  depths, delta and the LFO enable apply at once to a running voice; the EG mode only at the next voice on.
- Voice on (`Modulation::On`): buffer `Play` when it was not playing; stream `Process` of the first packet since the
  stream went quiet (never at packet boundaries). Phases start at the descriptor mode as xemu's VOICE_ON does: delay
  counts `delay*16` frames, hold `hold*16`, release `release*16` from full level, decay from full; modes 8-15 act as off.
- Stepping: once per 32 samples of 48 kHz output (`audio::kFrameSamples`), independent of the source rate and pitch,
  kept aligned across render blocks by `Modulation::sample`. Paused and synch-held voices are skipped, so they freeze.
  An active stream that runs out of packets keeps stepping for the rest of the block.
- Curves (xemu `voice_step_envelope`): off = 1.0; delay = 0; attack linear over `attack*512` samples; hold = 1.0;
  decay `255 * 0.99988799^(frames*4096/decay)` (about 64 dB over the decay time) until it reaches the sustain level,
  which then holds as `sustain/255`; release `level * e^(-6.91 * t)` over `release*512` samples from the level at the
  release, then force release. When the amplitude envelope reaches force release the voice turns off.
- Output: amplitude envelope × LFO tremolo multiplies the voice's samples (after interpolation, before the mix-bin
  gains). Pitch modulation, 4096 an octave, is added to the voice pitch before Doppler and the hardware clamp in
  `Voice3DData::PitchRatio`: `32 * (pitch_scale * EF + multi.pitch * LFOA + pitch.pitch * LFOF)` (xemu's EF formula,
  extended to the LFO depths).
- Release: buffer `StopEx` with DSBSTOPEX_ENVELOPE releases both envelopes; RELEASEWAVEFORM also clears looping so
  the buffer plays out to its end. The buffer keeps reporting DSBSTATUS_PLAYING until the release ends, then stops
  where it is (`ReleaseEnded`). Stream `FlushEx` with DSSTREAMFLUSHEX_ENVELOPE2 releases and keeps the packets
  playing; when the release ends the queued packets complete as flushed (`E_ABORT`) through the usual
  DirectSoundDoWork callback queue and the stream goes quiet. A zero amplitude release time (every voice that never had
  SetEG) keeps the old immediate stop / flush on the calling thread, so those sounds behave exactly as before.

### Uncertain choices (verify by ear or against console recordings)

1. **LFO frequency and waveform**: no reference implements the LFO. Modelled as a 16-bit phase advanced by dwDelta
   each 32-sample frame with a triangle output that starts at 0 rising: `f = dwDelta * 48000 / (32 * 65536)`, about
   `dwDelta * 0.0229 Hz` (XACT's `byte << 2` deltas give 0.09 to 23 Hz). A different phase width would scale every LFO
   rate by a power of two.
2. **LFO delay unit**: taken as the envelope unit (512 samples). The 15-bit field could instead count 32-sample frames
   (16 times shorter delays).
3. **LFO enable = dwDelay != 0** is taken literally from `0x3C6DEC`, so a zero-delay LFO is off.
4. **LFO depth scales**: the pitch depth uses the EF pitch-scale step (32 pitch units, 127 is about an octave at the
   peak). The amplitude depth is an attenuation-only tremolo: gain 1 at the LFO's peak, `1 - |depth|/128` at its
   trough (a negative depth inverts the phase).
5. **EF while off** returns 1.0 (xemu), so a nonzero generator-0 pitch scale with mode 0 gives a constant offset.
6. **Repeated release** is ignored while a release runs (xemu would restart the count from the held level).
7. **Filter**: the generator-0 cutoff scale and the LFO cutoff depth are stored but have no effect, because the voice
   filter itself (SetFilter, DLS2/ParamEQ) does not run on the host.
8. **StopEx / FlushEx timestamps** are still not scheduled (both act at once), unchanged from before.
9. `DSSTREAMFLUSHEX_ENVELOPE` (2) without 4 flushes at once: the original flushes the packets immediately, leaving the
   voice no data to release over.

### Diagnostics

The first 16 SetEG and 8 SetLFO descriptors are logged at info level (`dsound: <obj> amplitude envelope mode ...`,
`dsound: <obj> multi LFO delay ...`); the old "envelope/LFO modulation is not implemented" lines are gone. Listen at a
race start (XACT engine and tyre sounds) and when sounds stop: where the bank sets a release they should now fade
instead of cutting off.

Info lines only reach the log in a verbose run: the release build writes errors and reports only unless the
environment variable `XBE_VERBOSE=1` is set (`runtime/core/log.cpp`). The 18:01 user session log therefore has no
`dsound:` lines at all.

## Music overlap fix (2026-10-21, after the first playtest)

User report: songs play over each other when skipping tracks or going from driving into a cutscene.

### Cause

The music is XACT streamed wave banks (`assets/audio/XStreams/WMA.xsb/.xwb`; `dx`, `City_Walla`, `Engine_Revs` and
`RaceStarters` stream the same way). XACT's streams became host streams only when `IDirectSound_CreateSoundStream`
(called by XACT at `0x40F105`) got a host implementation in this working tree; before that they never sounded. Two
stream calls did not do what XACT expects:

1. **`CDirectSoundStream_Pause` (0x3C156C → 0x3C5A8C) ignored mode 3, DSSTREAMPAUSE_PAUSENOACTIVATE.** XACT prepares
   every streamed wave by calling Pause(3) and submitting its first packets (`0x40FED4`), and starts it later with
   Pause(0) when the cue plays (`0x40F390`). On the console mode 3 on a stream that is not running sets state bit
   `0x20`, and Process (`0x3C5A75`) does not start the voice while that bit is set; Pause(0) clears it and starts the
   voice (`0x3C5706`). On a running stream, mode 3 clears the pause and synch bits instead. The host treated 3 as
   "not paused", so the prepared next song (or the cutscene's dialogue/music stream) started as soon as its packets
   arrived, over the current one.
2. **`CDirectSoundStream_GetStatus` never reported DSSTREAMSTATUS_PLAYING (0x10000).** XACT's is-playing check
   (`0x40F198`) reads `GetStatus & 0x10000` for streams (`& DSBSTATUS_PLAYING` for buffers), and its voice reclaim
   (`0x406CA4`) frees a voice it thinks has stopped. Every stream looked stopped, so XACT could recycle a stream voice
   that was still playing. The XDK's status (`0x3C4FA9`): READY with a free packet slot; for a running voice
   PLAYING, or PAUSED (`0x20000`) when paused/synch-held/starved, plus STARVED (`0x40000`) when starved.

The envelope release paths were checked and are not involved: the game's own stream player (`0x2EC5A8`, `0x2ED825`,
`0x2EDA24`, `0x2EDDAA`) only calls FlushEx with flags 1 (async), which flushes at once as before; XACT's FlushEx(5) and
StopEx(3) only defer a stop when the voice has an amplitude envelope with a nonzero release time, and then for exactly
`release * 512` samples (the envelope stops stepping only while the voice is paused, synch-held or held). The game's
code never calls SetEG/SetLFO itself; only XACT does. Bink cutscene audio uses plain buffers (`0x3AF8F7` create,
Stop at `0x3AF5FD`/`0x3AF671`/`0x3AF71D`) with no envelope, so it is unchanged.

### Change (`runtime/xdk/dsound.cpp`)

- `Voice::hold`: set by Pause(3) on a stream that is not running; Render skips a held stream and Process does not
  start its envelopes. Pause(0) clears it and, if packets are queued, starts the voice (envelopes and LFOs restart
  there). Pause(3) on a running stream resumes it. Pause(1) pauses, Pause(2) synch-holds, other modes return E_FAIL.
- `GetStatus`: READY as before, plus PLAYING for a running stream with packets, PAUSED for a paused or synch-held
  one, PAUSED | STARVED for a running stream with no packets; a held or quiet stream reports neither. A stream releasing
  under FlushEx(ENVELOPE2) keeps PLAYING until the release ends, which is what XACT waits for.
- The game's own player waits `while (GetStatus & 0x10000) DirectSoundDoWork()` after FlushEx(1) (`0x2ED82D`); the
  host flush is synchronous and deactivates the stream, so that loop ends at once.

Built with `Build-MC3.ps1 -Jobs 8` (dsound.cpp recompiled, mc3.exe linked); not run.

### Still to check in a playtest

Skipping songs, a song ending by itself (the next one should start after it, not before), driving into and out of a
cutscene, pausing the game during music. If a song now never starts, look at whether XACT calls Pause(0) on it (a held
stream stays silent until then, as on the console).
