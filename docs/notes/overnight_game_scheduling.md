# GPU scheduling around human play

2026-10-24: an installed Fortnite client was running while MC3 native validation was queued. No game window
was focused or controlled. The existing root-owned cooperative stop had already drained the movie worker at
the end of `reward/rg55.bik`; the latest strict progress report is9/106 verified, zero failures.

`tools/maintenance/overnight_pipeline.py` now treats the exact installed client name,
`FortniteClient-Win64-Shipping.exe`, like `mc3.exe` for GPU scheduling. Its inventory matches names without
case sensitivity. A running client prevents a new GPU child. If one starts during processing, the coordinator
requests its own child's batch/movie boundary, waits for the client to close, then resumes the same durable
phase. It never kills/suspends a game or worker, changes physical input or alters user stop flags. The check
is an explicit small process list; it does not claim to detect every installed game.

The full coordinator fixture suite passes28/28. The new regression verifies another installed game's arrival,
boundary exit, wait and resume ordering, with no GPU or real game process. The actual read-only process
inventory also returns the live client. Existing user-cancellation priority and marker-ownership tests pass.

The current marker `.local/stop-overnight-pipeline` contains exactly
`root-tire-bead-native-qa-20261024`. It belongs to this root QA task. Before resuming, verify no active native
QA or candidate worker, preserve any replaced marker or other user stop flag, and remove only this unchanged
owned marker. A newly started coordinator will still wait for human game clients to close. CPU-only texture
analysis can proceed without using the GPU; explicit two-thread CPU model work has its own staged outputs.
