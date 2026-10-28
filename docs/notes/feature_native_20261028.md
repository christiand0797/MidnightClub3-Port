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
