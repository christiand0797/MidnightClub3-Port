# Bel Air career access and save preservation

The earlier 1957 Bel Air career is intact in the original native installation's `hdd7`.
The desktop shortcut now passes `--instance 7` to that same executable. Its target,
working directory, icon and description are unchanged. This selects the existing career
through the original instance routing; no save format, save writer or automatic saving
behavior was changed. No career file was replaced or merged.

`runtime/app/main.cpp` selects `ExeDir()/hdd` without instance arguments and `hddN` for
instances 2–8. Consequently, the clean R12 test in `G:/MC3_QA_r12_offline_r1`, instance 8,
started with a separate empty career. It did not use the canonical Bel Air profile.

The immediate post-launch receipt at 04:29:57 UTC October 27 proves all 53 protected
canonical/previous-private settings, saves and shortcut files still matched staging after
R12 had booted. Receipt SHA256:
`75c6874e2d7cd4633fde695ba19e4814f494ee835212cd3735ad8c82a06acc15`.

A later canonical default-profile session loaded Continue Career, garage and street,
then exited normally. Its default career and settings changed afterward; these newer
files were preserved rather than restored to the old staging hashes. The other 51 files
still matched staging. The user explicitly identified the earlier Bel Air as the career
to select.

Before routing, all 53 current files were independently copied to
`G:/MC3_Backups/saves_2026-10-27_post-user-session_pre-r12-followup`.
Before/copy/after hashes and original timestamps match; copied files have independent
file identities. Backup receipt SHA256:
`ddde7dcfaa883d420c833f211b471dc865f0212b2eb516afe3bd33d84640a913`.
All five original `hdd7` files also match the October 23 pre-hotfix backup.

The shortcut-only routing receipt is in the external corrected-content release-work
directory, `belair-shortcut-routing-20261027-r1/receipt.json`, SHA256
`207a5d086b94b4116b41a1d6209b161dfd1059ca9e9889e44e5354129d44c433`.
Its post-check verifies the other 52 protected files unchanged. The default career remains
available separately. Existing exact-path shortcut metadata prevents ordinary startup
from replacing the selected arguments; moving the installation or recreating a missing
shortcut can recreate the default route.

If another career appears missing, first check the executable directory and instance
arguments. Preserve all differing careers before selecting one. Do not copy an older
`Player 1.mc3Remix` over a newer file based only on its common filename or timestamp.
