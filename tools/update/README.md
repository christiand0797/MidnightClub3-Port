# MC3 updates

Double-click **Update MC3.cmd** in the game folder to check for updates. This
launcher only checks; it does not download or replace the game. Windows
PowerShell 5.1 or later is sufficient, with no Python installation needed.

The native menu also exposes **F1 > Info > Check now**, then **Download update**
and **Install after game closes** as separate choices. A startup notification
can point to that page when a published build is available. After choosing
installation, leave the game normally when ready; the updater waits for it.

The updater distinguishes published runnable builds from newer source commits.
A source commit can be reported before a tested runtime release is available.
Only a published `MidnightClub3-Native-…-win64.zip` from this project's fixed
GitHub repository is an installable update. Its GitHub SHA-256, release tag,
source revision, archive paths and per-file manifest are checked.

## Separate choices

From PowerShell in the game folder:

```powershell
# Check: no runtime download or installation.
& .\update\Update-MC3.ps1 -Mode Check -InstallDirectory (Get-Location).Path

# Download the release_id reported by Check into a new local staging folder.
& .\update\Update-MC3.ps1 -Mode Stage -InstallDirectory (Get-Location).Path -ReleaseId RELEASE_ID

# Install the verified stage after deciding to update; use the exact directory Stage reports.
& .\update\Update-MC3.ps1 -Mode Apply -InstallDirectory (Get-Location).Path -StageDirectory 'STAGE_DIRECTORY'
```

Replace `RELEASE_ID` with the numeric release ID and `STAGE_DIRECTORY` with the
absolute stage path printed by the download step. The helper prints JSON with
`status` and a message; success exits 0, an error exits 1. A staged update is not
installed until Apply is chosen.

Apply waits for that installation's game and Make HD jobs to close. It does not
terminate them. If the wait expires, the stage remains available for another
Apply attempt. Verified backups and a durable transaction journal stay in that
stage. If an interrupted application is detected, rerunning Apply restores the
previous files and reports `rolled_back`; the result explains the next action.

Only distributed runtime, compiler, enhancement-tool and documentation files are
updated. Original saves, `settings.toml`, disc/game content, generated textures,
PBR maps, movies and existing content/default metadata stay in place. The
recompiled `mc3.dll` is not downloaded; the native game manages game-code
generation from the player's own supported disc.

The command launcher and helper are included even in packages without **Make
HD**. These controls do not publish a release or turn unbuilt source commits into
runtime files.
