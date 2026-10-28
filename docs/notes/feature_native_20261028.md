# Feature candidate native checks — 2026-10-28

Source `12c4d938ad3db6796ab6cb788957c7a079eac2ff` built in the separate
`G:/MC3_Features_20261028` directory. EXE SHA-256:
`bc90298ec890376001e4da44c12c6c8e3403e15511ad12f7f2e7d799faa7f5b2`.
The build-result receipt is `990f88b936a25f0f7dfb58a98d7f9776b6d95e3a4749e58a877b8e50dd283bc1`;
tracked source/codegen/original-XBE input hashes stayed unchanged through compilation.
This development build does not inherit sealed R12's complete dependency-closure evidence.

The actual `Check-PbrMaterials` executable passed a synthetic valid two-by-two pack,
rejected a wrong approved manifest hash and rejected invalid CLI arguments. The smoke receipt
is `ddd3e47531519c838e8c5b9303621bf423589cc66438d587104c000d0ffb5593`.
These fixtures do not generate original-derived PBR assets or activate a PBR renderer.

## Private native evidence

The private QA used independent **current** `hdd7`/settings copies, independent runtime files,
and the previously accepted immutable playable payload. A new independent backup covers all
53 current personal files, including legitimate later user changes. Neither native run changed
the canonical files or real desktop shortcut. Both launches used the opt-in startup gate only
after a new actual media-pause acknowledgment, with the same coordinator/video identities.

External evidence is under
`G:/MC3_Backups/release-work/corrected-content-0.3.3-dev.20261026-r1/feature-native-qa-20261028-r1`.

| Run | Observed result | Sealed result SHA-256 |
| --- | --- | --- |
| R1 | HD startup stalled waiting for frame fence 7; completed fence stayed at 6. Normal owned-window exit at 108 seconds. | `31abe7044d10bd4c2f84ce0d950bc38c7b4d9b9d914c3f1aa2ffcecab759d0ab` |
| R2 | Explicitly activated window rendered HD intro, actual 3D title world and F1 Display/Career controls. No pending-fence warning. Normal owned-window exit at 421 seconds. | `5c993d56b29da666a7b598127fc3ad244f9345b4b9daacaa5a1f7ab0aa625318` |

R1 log SHA: `63aaab3a0351f53db9ed0ac68feaae60f03d72566182208f370bfb5562a2de93`.
R2 log SHA: `64061b1c18a7b6c4a24eb8de07cf23dd323d48f5a3fc9c102bf74588da8986a8`.
Both used the same user graphics settings: HD textures/movies, borderless desktop output,
200% rendering and actual 3840×2160 internal rendering. R2's different result is not proof
that focus caused or fixed the intermittent GPU stall.

Short guest keyboard taps did not reliably advance the title menu. F1 window events worked;
career was still unloaded, and money controls were correctly disabled. There is no acceptance
yet for wallet changes/persistence, A→B→A car tuning, real visible traffic/pedestrian counts,
or controller-driven native navigation. The installed executable remains unchanged.

## Follow-up source and CPU checks

Foreground keyboard down/up pairs now retain an additive pending edge until a real guest
state read. Device-connection scans preserve pending edges; repeats coalesce. Focus loss clears
pending input, and host F1 widget taps never enqueue gameplay actions. Physical held keys and
controllers keep their existing additive path. CPU checks exercise short taps, enumeration,
repeat/release, focus/F1 boundaries and controller/garage merging.

`XBE_D3D12_DEBUG` now requests the debug layer/DRED before device creation when available,
captures bounded warning/error messages, initial Present results and pending-fence descriptor
counts, and adds allocator/list/Present HRESULT checks. Successful positive DXGI statuses
remain successful. No descriptor reuse, upload-ring or fence-completion policy changed.
This is observability for the unresolved defect, not a claimed repair.

Thirteen focused CPU checks pass in `.local/f1-controls-cpu-20261027/ctest-r7.log`.
The updated source needs a new executable and fresh native evidence before installation.

## R3: actual feature controls

Source `94fc467e5173c2fde0ce5828e0ed9edfde5ee609` built successfully as EXE
`c724425f6b8a72fa139ffcb871c0d88eb12a4e8fed0a0d4691dd30ed863464a3`
(24,108,032 bytes). The new build result is
`e47718942a15e6deb0a37dfc45f4b61f8c055022b7a6bd5d93798937364868bd`.
The earlier candidate and CLI were independently preserved before the incremental build.

New QA controls live in `feature-native-qa-20261028-r3` under the same external release-work
base. Binding `d9a7bea6...`, staging `17f3a62b...` and reviewed helper `f79e8267...` bind the
new independent EXE/current career copies. A fresh actual video-pause acknowledgment preceded
the opt-in startup release. No older helper or frozen release evidence was changed.

Observed native results:

- Short keyboard Start/A/arrows/Back navigate profile selection and the original main/garage
  menus. The copied current career shows Player 1, 4% San Diego and the 1957 Bel Air.
- HD logos/intro and the actual 3D title, garage and street render. No pending-fence warning
  occurred in R3. The requested debug layer/InfoQueue are unavailable on this host; DRED setup
  succeeded. This is not a diagnosis or cure of R1's intermittent stall.
- One **Add $1,000,000** action changes F1's actual career snapshot from $90 to $1,000,090.
  That balance remains after the game's original SavingScreen and garage exit. The garage
  Flash label remains $90; its refresh and actual saved-disk/purchase behavior need separate
  checks. Do not infer disk persistence from this in-memory observation alone.
- Bel Air owned slot 2 gets front camber -5.1°. Golf owned slot 1 initially shows its own zero
  fallback and is changed to +2.0°. Switching back restores Bel Air -5.1°. The saved PC sidecar
  retains both entries. Disabling the per-car master exposes global zero; re-enabling restores
  Bel Air -5.1°. This does not certify every parameter's visual effect or a process restart.
- World targets can be set to 2x and remain inactive in the garage as intended, but incorrectly
  remain inactive in the actual street. A bounded read-only probe of the exact owned QA process
  proves career state 2, mission mode 1 (`find_hook`), a typed human controller, reciprocal car
  pointers and AI flag zero. The original dispatch at `0x1D024B..0x1D0261` checks state 2/mode 1;
  the population path at `0x1CB290` uses the existing writers. The narrow mode-0 guard is the
  defect. The correction explicitly allows modes 0/1 in state 2 and preserves original caps,
  pools and all other mode exclusions. Its failing-before/passing-after CPU regression is
  under `.local/world-free-roam-guard-cpu-20261028-r1`; native correction acceptance is pending.

Quota interruption left the owned session idle in F1 longer than intended. Root resumed,
verified that owned idle session, closed it normally at 15,112.077 seconds with Exit 0, and
observed the same coordinator/video/encoder resume Sgameint from frame 1,336 onward. The
entire elapsed session must not be described as a short bounded run. All 53 current canonical
personal files still match the current backup, including the real shortcut; QA's modified
career remains private and must never replace the user's career.

`native-result-r3/result.json` SHA-256 is
`9dd07222a27b42a49fdc5d07154da69e777e1e2b4914accdf10f32026aec0ac9`.
It seals the independent session log, private-after saves/settings/sidecar, original Sky JPEG
observations, exact read-only world probe and same-worker resume. Installation and release
publication remain pending the remaining native gates.

## Money persistence and presentation follow-up

A read-only structural walk of the original save schema confirms **1,000,090** in R3's private
`Player 1.mc3Remix`. It walks the raw XJ97 version-1 career prefix and original aligned field
order rather than searching for the expected cash value. The selected model is `vp_belair_57`;
the source file SHA-256 stayed `b45c8ef33cde8b715c38e04db774359c53d5071551faef8877155c4c17246ba3`
before/after. Twenty-two positive/malformed/decoy parser checks passed. Career tail and console
signature validation are outside this read-only proof.

Receipt `economy-disk-proof-r1/disk-result-r1.json` under the R3 control directory has SHA-256
`3959e62415b81b358efc1d19a1ec05500346fb650ba618f64034cd968e3f6fa7`.
This closes the saved-cash observation for R3; its original garage label still needs refresh.

The presentation patch observes the actual original garage regular-update `this`, verifies
the typed garage/current career and movie wrappers, formats into the original persistent
garage-owned buffer, and uses only the original `Fortune`/`updatemoney` setters. A separate
guest scratch stack and full context copy preserve the original update's registers and stack.
It does not change cash again, invoke refund/spending logic, trigger saving or change a save
format. Focused fixtures execute the original formatter/Flash wrapper/value-writer instructions.
The integrated 17 CPU checks, including garage/profile/tire hook contracts and PBR geometry,
pass. New native immediate-HUD and corrected population acceptance are pending.
