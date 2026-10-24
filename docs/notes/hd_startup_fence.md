# HD startup and GPU completion diagnostics

2026-10-23. A native startup with HD movies enabled stalled on a black first intro at 1 fps using the previously
published executable (`72ade3b85ea2292c57867f5ae6d635a20304d83d9830742c1c96b85eca12c5ab`). Its window and F3
diagnostic remained responsive. The original movie-picture path subsequently reached the menus and driving.

A 0.7-second minimal-dump capture and offline PDB stack walk identified the render worker waiting for the
frame allocator's GPU completion fence in `D3D12Backend::ResetFrame`. The title thread waited for rendering;
the decoder waited for a requested movie frame. This establishes a GPU-fence wait, not a keyboard/controller
lock or a Media Foundation decode call. Some deeper unwind frames were invalid and were excluded from the
conclusion. Local evidence is retained in `.local/mc3-hang-7120-evidence.md`, its guest log, dump and stack file.
Personal dumps and PDBs are not release assets.

The backend now checks command-list close, queue signal and fence-event HRESULTs. The first eight submissions
and first eight delayed waits report frame/slot, target and completed fence values, upload bytes and device-removal
status. Failed submission/device removal is logged and flushed before stopping the render path. A wait checks
actual fence completion at one-second intervals; it never resets an allocator or reuses an upload ring merely
because a timeout elapsed. This also avoids relying indefinitely on a missed event after actual completion.

The corrected vehicle/diagnostic candidate, SHA-256
`b3b6bd221d22055860de276447911be25fcda81f3ef60039102145f3bd0a14b2`, subsequently played all three HD startup
movies at 30 fps. Its first eight submissions showed completed fences advancing normally, and no delayed wait
was reported. This successful run does not establish the original stall's underlying driver/submission cause
or certify every future startup.

A second bounded HD-enabled start on a copied `hdd8` profile also played those three movies, then reached the
menus, garage and driving without delayed-fence/submission/device-removal diagnostics. The log ended normally
at 378.750 seconds. Exact evidence, vehicle checks and remaining limits are recorded in
[native hotfix QA](native_hotfix_20261023.md). These two passes still do not establish the earlier stall's cause.

The complete 21-check CTest suite passed against the corresponding sources, including the real Media Foundation
HD decoder test. The candidate was compiled and linked separately before replacing the idle QA executable;
the user's newly saved ride settings were preserved. Subsequent vehicle QA used `hdd8`, copied from `hdd7`;
all five original hdd7 save-file hashes and the restored settings were verified against the pre-QA backup.
