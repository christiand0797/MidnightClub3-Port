# Cooperative game pauses for overnight media work

The coordinator now watches for `mc3.exe` while its owned texture or movie child
is running, as well as before launch. On game arrival it requests the child's
existing `--stop-file` mechanism. The texture worker finishes its current batch;
the movie worker finishes its current movie. Neither the game nor its input is
stopped, and no child is killed or suspended. Detection uses the existing
five-second monitoring interval; the boundary itself may take longer.

After the child acknowledges the request with exit code 2, the coordinator waits
for MC3 and existing media workers to close, then starts the same resumable phase
again. Named texture-pass records and verified movie recipes retain completed
work. A successful exit means the phase finished; a failed exit is reported
instead of being retried as a game pause. An unexplained exit 2 stays paused for
inspection rather than entering an automatic retry loop.

Each child receives a unique `child-stop-<UUID>` path under
`.local/overnight_pipeline/live`. The coordinator creates that marker exclusively
and records a matching ownership token inside it. After the child exits it
removes only a marker it actually created whose token still matches. A replaced
marker is preserved and stops automatic resume. Old private markers left by an
interrupted coordinator are not reused by a later run.

The shared stop files remain user cancellation inputs:

- `.local/stop-overnight-pipeline`
- `.local/stop-texture-upscale`
- `.local/stop-video-upscale`

The coordinator never creates, overwrites or deletes these shared flags. A flag
that already exists prevents launch. An observed cancellation takes priority
over a game pause and remains latched until the current child exits, even if
someone removes the flag while that child drains. It then returns paused with
exit code 2; a new explicit coordinator invocation is required to continue.
Cancellation is also checked while waiting for the game to close. An interrupt
during child monitoring requests the same safe drain and remains a cancellation.

The existing root-owned stop flags are intentionally retained. Their owner must
remove them when work is authorized to resume. This change itself starts no
worker. After that handoff, use the usual command from the intended checkout:

```powershell
py -3.12 tools/maintenance/overnight_pipeline.py --run
```

The fresh-media audits, verified full-project backup, all-file SHA-256 checks
and backup marker remain mandatory before offline PBR. The CPU PBR stage has no
cooperative stop CLI and is allowed to finish once started; this change does not
interrupt it or change runtime PBR.

Fixture validation on 2026-10-23:

```powershell
py -3.12 -m unittest discover -s tools/maintenance -p test_overnight_pipeline.py -v
```

The fake processes exercise both media phases through game arrival, cooperative
exit, waiting and resume without duplicating completed items. Additional cases
cover user-stop priority, cancellation retained after flag removal, cancellation
during the game wait, foreign/replaced marker preservation, unexpected exit 2,
interrupts during monitoring, and actual child failures. All 21 tests, including
the existing backup/media/PBR gate and OS-lock regressions, pass. These tests
launch no GPU, game, backup-copy or PBR processes. A live
game-arrival exercise remains for the authorized worker handoff.
