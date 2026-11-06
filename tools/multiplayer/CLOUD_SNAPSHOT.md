# Offline original-save snapshot reader

`cloud_snapshot.py` makes an offline proposal compatible with the account
service's `/api/cloud` file envelope. It collects explicitly selected original
files unchanged. It never uploads, restores, alters career memory, edits a save,
launches/closes a game, or reads account credentials.

The helper starts **while the selected game process still exists**. It verifies
its actual executable path, held process handle, creation identity and original
`--instance` parsing, then waits for exit code zero. All MC3 processes from that
installation must be closed during collection because PC preference files are
shared between instances. A new or uninspectable MC3 process causes rejection.

Select exact virtual filenames with repeated `--file`; no directories, globs or
automatic profile discovery are supported. Virtual `hdd` maps to physical `hdd`
for instance 0/1 and `hddN` for instances 2–8, matching `runtime/app/main.cpp`.
The reader supports this title's `hdd/E/UDATA/54540079/...` `.mc3Remix`/`.mc3`
saves and known `.xbx` save metadata. Optional preferences are only
`settings.toml`, `settings.ini`, `config/mc3_enhancements.toml`, and
`config/tuning_profiles.toml`. Other config/account/token/password files are not
selected by this reader. It does not inspect opaque contents for credentials.

Pass the exact cloud revision separately; a future authenticated upload still
has to handle the server's optimistic revision conflict. The structured
`settings` field is `{}`; chosen preference files stay opaque in `files`.

```powershell
& 'C:/Users/purpl/AppData/Local/Programs/Python/Python312/python.exe' `
  tools/multiplayer/cloud_snapshot.py `
  --install-directory 'C:/absolute/selected/install' --instance 7 `
  --process-id 12345 --base-revision 4 `
  --file 'hdd/E/UDATA/54540079/EXPLICIT-SELECTED-DIRECTORY/Player1.mc3Remix' `
  --file 'settings.toml' `
  --output 'C:/absolute/unused/private-proposal'
```

The values above are placeholders. File paths and process ID come from the
selected installation/session, not a historical checkpoint or another profile.
Output must be a new directory outside the installation; existing artifacts are
preserved. It contains `cloud-request.json` and `proposal.json`, including hashes
of the exact bytes and request. The proposal contains personal save/preferences
and stays on disk until the caller chooses what to do with it.

Two bounded reads compare file identity, size, metadata and SHA-256. Reparse
points, symlinks, hard links, reserved Windows names, duplicate/prefix paths,
changes and totals beyond 256 files/16 MiB are rejected. There is no all-file
backup or cross-file atomic transaction claim. Stable bytes after exit do not
prove physical-disk durability, original signature validity, or original-loader
acceptance. The receipt records all three as unproved. `kernel::Shutdown` uses
`std::_Exit`, which skips C stdio/destructor flushing; the normal original save
flow's explicit close remains a separate observation.

Original load/signature boundaries and the future opaque restore design are in
[the save trace](../../docs/notes/career_save_network_trace_20261104.md) and
[integration note](../../docs/notes/integration_plan_20261104.md). No restore or
game-loaded acknowledgment is implemented by this tool.

Synthetic tests use temporary binary files and isolated Python child processes,
including actual Windows image/creation/instance checks. They also call the
actual Node `validateSnapshot` export without starting a server:

```powershell
& 'C:/Users/purpl/AppData/Local/Programs/Python/Python312/python.exe' -I -m unittest discover -s tools/multiplayer/tests -p test_cloud_snapshot.py -v
```
