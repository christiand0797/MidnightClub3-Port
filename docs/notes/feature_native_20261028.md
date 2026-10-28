# Feature candidate native checks — 2026-10-28

## Current local installation

The canonical desktop-shortcut target now contains the tested R5 executable
`d3879439428a58d1eb06937cda3476462f70fdf8ddbd0825820c66a462258258`
(24,126,976 bytes), built from exact source
`1b414bc7fd9a84a80b0f0c7eac53134adf28ec84`. It includes the World/Career controls,
per-owned-car tuning with global fallback/master, keyboard tap handling and inactive PBR
foundations. The integrated 17 CPU checks pass in `ctest-r9.log`.

The reviewed one-shot transaction installed only this EXE, ten sealed CRT files and the
unchanged tested `Check-PbrMaterials.exe`. Before replacement it independently copied and
verified the old EXE `7bb7fcd1928156db88e33c78cd5a799ed9b04d4c9c19c9a9bbd6b00e2aa9d1ad`.
It rechecked no active game, the exact source/build/native evidence, coordinator video phase
and all 53 personal-file hashes. Runtime publication used same-volume temporary files and
installed the EXE last. The real shortcut, saves, preferences, PC tuning metadata and media
assets were not copied from QA or changed. No canonical game launch or public release occurred.

External install evidence is in `feature-local-install-20261028-r1`; `complete.json` SHA-256:
`07c58cae9fe2a69ad95d3e2a82dc7dc7966ffbbdc663412a22ff5a966bd29c91`.
This is a tested local development installation, not a new immutable release or a complete
R12-style dependency-closure seal. Full PBR rendering remains unfinished and disabled.

## Earlier candidate

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

## R4: private restart and remaining presentation/population gates

Exact source `e7ed92d5f385cce7b91d8f064404af5690b68aa0` built as
`58bdbff5eefbf4a4ee143cba76b9ec6d1d67b44401e33a050c4e412297465449`
(24,122,880 bytes), build receipt
`55414928f8fa306d33de32da9ac7a582a2209bd23eb780c7e8384ed9a2961140`.
The fresh R4 context deliberately inherited only independently sealed R3 **test** saves,
preferences and PC tuning metadata. It did not load the modified test career over real saves.
A new actual cooperative acknowledgment and opt-in startup gate preceded GPU initialization.

- A new process loaded the private saved $1,000,090 balance in both F1 and the original garage
  display. Bel Air owned-slot-2 front camber -5.1° also restored without a roster-reset status.
  This closes those specific process-restart observations, not every car/setting visual.
- One Add $10,000 action changed the actual balance to $1,010,090. The original garage label
  remained $1,000,090 after closing F1 and opening the garage-exit confirmation. A bounded
  exact-owned-process read found the persistent text buffer already `$1010090.00` and valid
  typed wrappers/movie pointers. That buffer can also be formatted by the normal original
  update; it does not prove that all four narrow refresh calls completed. Immediate HUD
  acceptance remains failed and installation remains held.
- The original SavingScreen and garage exit completed. The frozen structural parser then
  confirmed $1,010,090 in the independently sealed private-after save, without changing its
  bytes. The parser still covers only the original career prefix through Money; the tail
  and trailing signature remain outside its scope. Disk proof SHA-256:
  `b99c11d704cff8eca1cb1e500ccecfba152577a5a3d09ada8f473290a46a5263`.
- In actual career free roam, World now shows active traffic 2x/pedestrians 2x and a ready
  pedestrian manager. Traffic roads tracked remains **0**. This proves the corrected
  eligibility gate activates; it does not establish traffic setter coverage, a visible
  twofold population ratio or all population behavior.
- HD startup, actual title/garage/street and short keyboard menu navigation worked with
  GPU debug off. No startup stall occurred in this run; R1's intermittent fence cause remains
  unresolved. The owned window exited normally at 795.660 seconds, and the same worker and
  encoder resumed Suschamp from frame 5,192 onward. All 53 current canonical personal files
  remained byte-identical, including the real shortcut. No installation or release occurred.

R4 controls are in `feature-native-qa-20261028-r4` under the external release-work base.
`native-result-r4/result.json` SHA-256:
`6ea0186fb942ef34a1ce2b2f990477ad350e44ec511d620e25858590def80ef8`.
It authenticates independent log/private-after copies, original Sky captures, exact read-only
probes, current personal-file preservation and same-worker resume. Old helpers and results
remain frozen; another native run requires a fresh reviewed context.

The next presentation observer wraps the normal original `0x152420` once and requires its
entry `this` to remain the current `[0x50F12C]` singleton. Optional `0x1519D0` retains only
its existing navigation lease. The presentation code keeps all career/type/movie/scratch
guards and adds bounded completion/deferral logs, so a native run can distinguish hook
execution from a merely updated string buffer. Four affected fixtures and the integrated
17-check `ctest-r9.log` pass. Traffic's separate original cap interaction has a failing-before/
passing-after retail fixture; see [cap policy and evidence](world_traffic_native_cap_20261028.md).
These source results do not replace the pending native HUD/density evidence.

## R5: wallet and population checks

The R5 build receipt is
`656e072608bd51f6dc02362349aa7ae2857ca9c6dbde1b3ca8ba0df142cfde6b`;
all consumed source inputs stayed unchanged. A fresh reviewed private tree inherited only
the independently sealed R4 test state. Its new actual cooperative pause acknowledgment and
opt-in pre-init gate retained the same movie worker before GPU initialization, with no NCNN
process remaining at release. Old helpers, results and frozen content stayed unchanged.

- HD startup and actual title/garage/street rendered. Short keyboard menu taps and F1
  open/close worked. No startup fence stall occurred in this run; its intermittent cause
  remains unresolved.
- The private saved balance reloaded as $1,010,090. One Add $10,000 action made F1 and the
  original garage label show **$1,020,090**. After F1 closed, the original label remained
  correct. A single completion line at 171.626 seconds records both narrow setter pairs.
  Root observed the update during the live check; the captures do not measure latency.
- The original SavingScreen and garage exit completed. The frozen structural parser
  confirms **1,020,090** in the independently sealed private-after save without changing
  its bytes. Save SHA-256:
  `b9c1806b1187f11b22e5946ed19250799ddc65dacbb5b50d38d3c8229ce0cbe5`.
  Receipt `economy-disk-proof-r5.json` SHA-256:
  `cbfa10f36ebe7f8e999ac7de01a4cb7ef35e6234642747624864dd840bcfa973`.
  The parser's career-prefix/tail/signature limits remain unchanged.
- World retains **382** tracked roads in actual state-2/mode-1 free roam. Three identical
  native road addresses/topologies show density 0.04 at 1x and 0.08 at 2x; corresponding
  target pairs are 6.86/13.72, 2.076/4.152 and 1.5/3.0. The game's current cap integer is
  4, with factor 0.01; its numeric capped baseline is multiplied as documented.
- The pedestrian manager has 1,178 regions. Bounded reads on its first 16 regions retain
  identical authored weights and caps and show target changes consistent with the original
  rounding/clamp policy: examples include 3 to 6, 1 to 2 and 5 to 10. Small rounded targets
  can stay 1. These are target observations, not an exact visible-actor ratio or all-region
  visual audit. Original actor pools remain authoritative.
- The owned window closed normally at **313.813 seconds, Exit 0**. The same coordinator,
  video worker and encoder resumed Suschamp from frame 5,432 onward. All **53** current
  canonical personal-file hashes remain unchanged. QA's millionaire career and tuning
  preferences remain private. Physical-controller and full parameter visuals remain open.

Independent review accepted these bounded evidence joins, including the actual Sky JPEGs,
log, exact source/EXE, startup acknowledgment, typed read-only probes and preserved files.
R5 controls live in `feature-native-qa-20261028-r5` under the external release-work base.
`native-result-r5/result.json` SHA-256:
`fbd0c52ebeb5e7eb58bfdc7fd639e3c0ba4dda9833136ec80df9ed14492ab90d`.
