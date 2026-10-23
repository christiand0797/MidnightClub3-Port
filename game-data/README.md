# Original game data

`original/` contains the supplied original Xbox USA disc, losslessly archived into 35 volumes. Keep every
`mc3-original-usa.7z.NNN` file together. Each volume is below GitHub's ordinary file limit. This fixed input is
separate from frequently updated native executables and generated HD replacements.

The complete archive was streamed back through SHA-256 and matched all 4,840,554,496 original bytes:
`d58490dc8e1837e9f47716268b52ae1f1ac8578e5a5e42e061fa7f1c4b068b52`.
`disc-manifest.json` and `SHA256SUMS.txt` identify every volume.

For a source checkout, install 7-Zip and run:

```powershell
python tools/disc/restore_repo_disc.py --extract
.\Build-MC3.ps1 -DiscImage '.\Midnight Club 3 - DUB Edition Remix (USA).xiso.iso' -Jobs 8
```

The restoration tool checks every volume, verifies the decompressed disc, and publishes it only after validation.
It preserves an existing different image. Without `--extract` it only verifies the archive volumes.

For playing, use the complete portable build on the repository's Releases page. The release includes the native
executable, runtime dependencies and its pinned content installer; source compilation is unnecessary.
