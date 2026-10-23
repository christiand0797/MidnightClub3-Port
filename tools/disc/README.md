# Disc verification

`verify_xiso.py` reads the XDVDFS filesystem directly with Python 3.10+ and no
external packages. The source ISO is opened read-only. It recognizes an XISO at
offset zero and the Xbox game partition at `0x18300000` in a full disc dump.

From the repository root in PowerShell:

```powershell
python tools/disc/verify_xiso.py 'E:\ORIGINAL XBOX GAMES\unzipped\Midnight Club 3 - DUB Edition Remix (USA).xiso.iso' --verify --hashes --expected-title-id 0x54540079 --expected-xbe-sha1 5bca1cf092edaadf876e8f80129900407a4a07b8
```

`--verify` checks every disc path, directory/file type, and file size against
`templates/game/assets` (or `--assets PATH`). `--hashes` additionally compares the
SHA-256 of every file; this reads the complete disc payload and extracted assets.
Extra local files also count as a mismatch. JSON output includes the XBE's title,
title ID, SHA-1, SHA-256, file counts, byte totals, and any mismatches. Add
`--report NEW.json` to save a report; an existing report will not be overwritten.
Exit status is zero on success, one for a mismatch/error, or two for invalid CLI
arguments. Hashes show that assets match the supplied disc; they do not certify
an external release database or prove that the native port works.

To import just the executable required by the static recompiler:

```powershell
python tools/disc/verify_xiso.py 'E:\ORIGINAL XBOX GAMES\unzipped\Midnight Club 3 - DUB Edition Remix (USA).xiso.iso' --expected-title-id 0x54540079 --expected-xbe-sha1 5bca1cf092edaadf876e8f80129900407a4a07b8 --extract-xbe templates/game/assets/default.xbe
```

The destination directory must exist. A matching existing XBE is left untouched;
a mismatched file is refused. A sibling temporary file is fully written, flushed
and verified before an atomic hard-link publication that cannot replace an
existing file. The destination filesystem must support hard links. The tool does
not execute game code, modify the image, or extract the other assets.

Run the synthetic format, corruption, and extraction-safety checks with:

```powershell
python -m unittest discover -s tools/disc -p 'test_*.py' -v
```
