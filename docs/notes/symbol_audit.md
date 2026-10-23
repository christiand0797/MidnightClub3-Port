# Zero-symbol audit: `templates/game/config/game.toml`

Updated 2026-10-21. This is a static audit of `templates/game/assets/default.xbe` (USA, XDK 5849). No build was run and the game was not launched.

**Result: all 55 `= 0` entries are really absent from the image. No address was changed.** `game.toml` was not edited. Every one of the 55 names has an HLE body in `runtime/xdk/*.cpp`. Since none of them is linked, those bodies are never reached in MC3, and setting them to 0 does not change behaviour. This is not the same situation as `D3DDevice_SetPalette`, which is linked and is called 8 times.

## Method and evidence classes

The scripts are in the session scratchpad under `audit/`. They use a private copy of the disassembler, because the shared `xdis.py` was changed by another agent while this audit was running.

- **X: .XTLID.** The XBE's `.XTLID` section (raw 0x4DC000, 0xB30 bytes) is a table of (library export id, VA) pairs for every linked XDK export that has an id. `xdk/5849/signatures.toml` records the id for 48 of the 55 names. **Calibration:** 73 of the nonzero `game.toml` symbols also have an id in the signatures file, and all 73 are in `.XTLID` at exactly the configured address (0 mismatches). An id that is missing from the table therefore means the export is not linked. Within each library the ids are in alphabetical order, and the neighbouring ids are present (for example 0xB0002 DirectSoundCreate and 0xB0004 DirectSoundCreateStream around the missing 0xB0003 DirectSoundCreateBuffer). That shows the table is complete for these ranges.
- **C: callers.** I disassembled every code section and listed every call, jump, immediate or data pointer that targets the library's section from outside it. Each unnamed target was then identified. None of them is one of the missing functions.
- **K: constants.** Some opcodes or constants appear only in the missing function, according to its signature `k` list. They do not occur anywhere in the library section, or they occur only inside other, identified functions.
- **H: shared helpers.** All x87 CRT functions return through the same helper. Nothing calls that helper except the three CRT functions the image does contain.

Why the finder writes 0: the XDK libraries are static and the title is linked with `/OPT:REF`. Functions the game never references are not linked at all.

## Table

| Symbol | Verdict | Evidence |
|---|---|---|
| accept | absent | X: id 0x40001 missing. Winsock stubs 0x3CC035–0x3CC0EB are all named. |
| getsockname | absent | X: id 0x40006 missing |
| getsockopt | absent | X: id 0x40007 missing |
| listen | absent | X: id 0x4000C missing |
| recv | absent | X: id 0x4000F missing. The title uses recvfrom/WSARecv. |
| send | absent | X: id 0x40012 missing. The title uses sendto/WSASend. |
| WSARecvFrom | absent | X: id 0x40020 missing |
| XNetInAddrToXnAddr | absent | X: id 0x4002E missing. 0x4002D and 0x40030 are present. |
| XOnlineCleanup | absent | X: id 0x30002 missing. 0x30001 XOnlineStartup is present. |
| BinkGetSummary | absent | C: the game calls exactly 7 BINK entry points: 0x3AB590 (open), 0x3ACDF0 Wait, 0x3AD0F0 (SetVolume, ret 0xC), 0x3AD280 (CopyToBuffer, 7 args), 0x3AD2C0 DoFrame, 0x3AD8D0 NextFrame and 0x3ADA70 (close). None is GetSummary (len 0x1C9, ret 8). There are no data pointers into BINK. |
| crt_sin, crt_cos, crt_tan, crt_atan, crt_sqrt, crt_log, crt_log10 | absent | H: every CRT signature exits through the 0xD-byte `_math_exit` helper. That helper is at 0x2E08DE, its bytes are unique in the file, and all 10 of its references come from pow 0x2DC494, acos 0x2DC6A1 and asin 0x2DCD41. Only "pow", "acos" and "asin" have per-function name strings next to the CRT code. The game uses inline x87 instead: 775 fsqrt, 135 fsin, 104 fcos, 50 fpatan, 26 fyl2x. |
| CDirectSoundStream_SetFrequency | absent | X: the stream wrapper id 0xB008F is missing. C: the CDirectSoundStream method bodies 0x3C1C79–0x3C1F5B are 10 contiguous 0x52-byte bodies, and all of them are accounted for (SetPitch, SetVolume, SetLFO, SetEG, SetFilter, SetHeadroom, SetMixBinVolumes, GetVoiceProperties, Set3DVoiceData, Use3DVoiceData). Each is reached from its IDirectSoundStream jmp thunk at 0x3C2294–0x3C22DE. There is no SetFrequency body or thunk. |
| CDirectSoundStream_SetI3DL2Source | absent | X: id 0xB0091 missing. C as above. |
| CDirectSoundStream_SetMaxDistance | absent | X: id 0xB0093 missing. C as above. |
| CDirectSoundStream_SetMinDistance | absent | X: id 0xB0094 missing. C as above. |
| CDirectSoundStream_SetPosition | absent | X: id 0xB009A missing. C as above. |
| CDirectSoundStream_SetVelocity | absent | X: id 0xB009D missing. C as above. |
| DirectSoundCreateBuffer | absent | X: id 0xB0003 missing. The title uses IDirectSound_CreateSoundBuffer. |
| DirectSoundUseFullHRTF | absent | X: id 0xB002F missing |
| DirectSoundUseLightHRTF | absent | X: id 0xB0031 missing |
| IDirectSound_CommitEffectData | absent | X: id 0xB0038 missing |
| IDirectSound_GetEffectData | absent | X: id 0xB003F missing |
| IDirectSound_GetSpeakerConfig | absent | X: id 0xB0041 missing |
| IDirectSound_SetDistanceFactor | absent | X: id 0xB0048 missing |
| IDirectSound_SetDopplerFactor | absent | X: id 0xB0049 missing |
| IDirectSound_SetOrientation | absent | X: id 0xB004D missing |
| IDirectSound_SetPosition | absent | X: id 0xB004E missing |
| IDirectSound_SetRolloffFactor | absent | X: id 0xB004F missing |
| IDirectSound_SetVelocity | absent | X: id 0xB0050 missing |
| IDirectSound_SynchPlayback | absent | X: id 0xB0051 missing |
| IDirectSoundBuffer_SetAllParameters | absent | X: id 0xB005F missing |
| IDirectSoundBuffer_SetI3DL2Source | absent | X: id 0xB006C missing |
| IDirectSoundBuffer_SetMaxDistance | absent | X: id 0xB006F missing |
| IDirectSoundBuffer_SetMinDistance | absent | X: id 0xB0070 missing |
| IDirectSoundBuffer_SetPosition | absent | X: id 0xB0078 missing |
| IDirectSoundBuffer_SetRolloffCurve | absent | X: id 0xB0079 missing |
| IDirectSoundBuffer_SetVelocity | absent | X: id 0xB007B missing |
| XAudioDownloadEffectsImage | absent | X: id 0xB00A6 missing. The title uses IDirectSound_DownloadEffectsImage 0x3C1FC7. |
| D3DResource_IsBusy | absent | X: id 0x8006F missing. 0x80069–0x80072 neighbours are present. |
| D3DDevice_DrawVertices | absent | K: the NV097 begin/end constant 0x417FC occurs only inside DrawVerticesUP, DrawIndexedVerticesUP, DrawIndexedVertices, Begin and End, and in game push-buffer code. There is no 0x40001810 draw-arrays word. C: no unnamed D3D target has ret 0xC. |
| D3DDevice_EnableOverlay | absent | K: 0x8700, 0x8704, 0x8900 and 0x8910 (overlay registers) occur nowhere in D3D. |
| D3DDevice_FlushVertexCache | absent | K: 0x41710 occurs once in the whole image, inlined in the vertex-buffer lock at 0x417460 (XTLID 0x8009B). There is no standalone function. |
| D3DDevice_GetVertexShaderFunction | absent | K: D3DERR_MOREDATA 0x88760867 occurs nowhere. |
| D3DDevice_InsertFence | absent | C: an 8-byte `push 0; call D3D_SetFence; ret` would be needed. All 10 callers of D3D_SetFence 0x4164E0 are inside larger functions (BlockOnTime, MakeRequestedSpace, draws, End, Swap). |
| D3DDevice_BlockOnFence | absent | C: all 9 callers of D3D_BlockOnTime 0x4165A0 are inside KickOffAndWaitForIdle, BlockOnResource, the resource-busy helper 0x416950 and Swap. None is a 0xF-byte wrapper. |
| D3DDevice_IsFencePending | absent | K: it would read the GPU-time pointer at [g_pDevice+0x30]. That pattern occurs only in BlockOnTime. |
| D3DDevice_PersistDisplay | absent | K: it needs 0x1C30 and two uses of 0x80004005. 0x1C30 occurs only in the Swap-internal flip helper 0x417DA0, whose only caller is Swap at 0x418797. The only 0x80004005 in D3D is a format-table loop at 0x41AFEE. |
| D3DDevice_SetTile | absent | C: it would be `call KickOffAndWaitForIdle; jmp SetTileNoWait`. The callers of 0x4168B0 are BlockUntilIdle (jmp at 0x411560), CreateDevice and 0x41A11A. None is a 10-byte wrapper. |
| D3DDevice_SwitchTexture | absent | C: the game calls D3DDevice_SetTexture 0x4123D0 14 times. The only fastcall (ecx, edx, ret 4) external D3D target is 0x4150F0, which is SetVertexShaderConstant (copies constants to 0x421CC0 and calls ...NotInlineFast). |

## HLE impact

All 55 names have HLE bodies. Files: `xnet.cpp` for the Winsock/XNet/XOnline names, `bink.cpp` for BinkGetSummary, `crt_math.cpp` for crt_*, `dsound.cpp` for the audio names, and `d3d8.cpp` for the D3D names. With 0 in the config they keep their placeholder `SYM_` values (`FFFFFDxx`/`FFFFFFxx`) and are never dispatched. Nothing in the game reaches them, so nothing is lost.

## Side findings, outside the zero list (no action taken)

- Some IDirectSoundStream wrappers have no name in `game.toml`: 0x3C2294 SetVolume, 0x3C22A8 SetFilter, 0x3C22AD SetHeadroom, 0x3C22B2 SetMixBinVolumes, 0x3C22B7 Pause, 0x3C22BC FlushEx, 0x3C2938 SetMixBins and 0x3C2F98 SetFormat. Each is a jmp or forwarder into a body that is already named, so the existing HLE covers it.
- Two thunk pairs have no name and no XTLID: 0x3C2F9D → 0x3C2C7C (buffer) and 0x3C22E3 → 0x3C1627 (stream). XACT calls them at 0x40F83C/0x40F859 and 0x40F89E/0x40F8BB, right before Set3DVoiceData. They take a listener block at 0x411238 and a voice 3D block, so they look like XDK 3D calculation helpers that fill the 3D voice data. They run as recompiled guest code. Whether they need host attention depends on how `Set3DVoiceData` is consumed.
- The `CDirectSoundStream_SetPitch` and `_SetLFO` entries point at the IDirectSoundStream thunks (0x3C2299/0x3C229E). `CDirectSoundStream_SetVolume` and the others point at the method bodies. Both work, because each thunk only jumps to its body.
