# Offline texture extraction: P8, linear, .tex, .xbpf, XPR

Updated 2026-10-21. CPU-only work in `tools/texture_pack/`; no build, no game launch, no upscaling run. Output:
`templates/game/out/native/textures/source/<HASH>.png` plus `source_index.json` (hash -> where it came from).

Run: `py -3.12 tools/texture_pack/extract_all.py` (incremental: existing PNGs are kept, the index is merged).
Modules: `xbtex.py` (Hash, .xbck objects, P8 pairing, linear), `texfile.py` (.tex), `xbpf.py` (paged pools),
`xiso.py` (read pristine files from the disc image), `dave.py` (archives).

## The name the runtime gives a texture

`runtime/gpu/texture_pack.cpp ContentHash`:
`Hash(u32 shape[6] = {format, width, height, depth, levels, cube})`, then `Hash(TotalSize(d) guest bytes, h)`, then
for P8 only, when a palette is bound, `Hash(u64 palette_hash, h)`.

Palette hash (`runtime/gpu/d3d12.cpp ReadStagePalette`, checked again after the media audit's changes): a
`StagePalette.colours[256]` zero-initialised, `(256 >> size_code) * 4` bytes copied from the palette's Data
(`size_code` = D3DPalette Common >> 30), then `Hash(colours, 1024)`. So the tail past a small palette is **zero**,
not memory. `xbtex.palette_hash()` does exactly this. Palette colours are D3DCOLOR, bytes B G R A.

Linear sizing (`runtime/gpu/texture.cpp Describe/LevelSize`): with `Size != 0`: width `(Size & 0xFFF) + 1`, height
`((Size >> 12) & 0xFFF) + 1`, pitch `((Size >> 24) + 1) * 64`, depth 1, levels 1, bytes `pitch * height`. A linear
format with `Size == 0`: dimensions from Format, pitch `width * bpp`, and LevelSize is `pitch * height` for *every*
level (so `levels * pitch * height`). Only textures with `!cube && depth <= 1` and fewer than 4 uploads are packable.

## 1. P8 in .xbck bundles

All 6698 P8 texture objects: 6697 in the 755 loose `flash/*.xbck` (UI) bundles, 1 in `assets_x.dat_5:
resources/city/detroit_dusk_clear.xbck`.

Pairing found in the flash bundles (all 6697 pair this way, 1 reference each, 1 palette each, size code 0):
a 0x30+ byte record `{1, ptr, texture object*, 0x00800005, ..., u16 w, u16 h (+0x28 from the texture pointer),
0x00060000, wrapper*, self*, ...}`; `wrapper` = `{u32 0x01000001, D3DPalette{0x00030001, Data, 0}, colours VA}`, so the
palette object is at `wrapper + 4` and its colours right after it (aligned). Example `flash/bg_main.xbck`: texture
object at 0x118B60, record at 0x11CB80 (texture pointer at +0x8, wrapper pointer 0x82D124C0 at +0x30), palette object
at 0x11CBC4, colours 0x11CC00 (an alpha ramp of white).
The city bundle's texture has no pointer to it in its bundle; its one palette object (0x9EF890, colours at 0x9EF8C0)
sits right before the texture object, so the fallback (the two palette objects nearest the texture object) takes it.

Verification: 104 unique P8 names; 3 of them equal runtime dump names in `textures/dump` (e.g. 31BE74CF4433BF1A,
`bg_main.xbck`), confirming the palette hash including the 1024-byte zero-padded rule.

## 2. Linear textures

`xbck_textures` now accepts `Size != 0` (linear formats only) and linear formats with `Size == 0`, sized as above;
`decode()` reads rows of `pitch` bytes. A full scan of every .xbck (archives + loose, 5965 bundles) finds **no**
linear texture objects (only DXT1 21552, DXT5 47801, P8 6698 instances). The linear textures the runtime sees
(e.g. LIN_X8R8G8B8 movie frames, render targets) are created at run time, not stored on disc. The two XPRs have none.

## 3. .tex files (4416: 4040 in assets_x.dat, 376 in assets_x.dat_1)

Traced in default.xbe:
- `sub_002D57B0(name, mip, ..)`: texture cache; calls the image loader `[0x4B584C]` = `sub_002D8D50(path, mip)`,
  the image hook `[0x4B4E9C]` = `sub_002D5700` (drops levels above `[0x559A3C]`, a .bss value never written: inactive),
  then `gfxTexture::Create` `sub_002D6170(image, mip)`.
- `sub_002D8D50` picks a loader by extension; `.tex` -> `sub_002D7710(path, mip)`.
- `.tex` header (14 bytes): `u16 width, u16 height, u16 type, u16 levels, u16 flags_lo (+0xA), u16 flags_hi (+0xC),
  u16`; image flags = `flags_hi << 16 | flags_lo`; bit 0x400000 would make a linear texture (no file has it).
  Width 0 and height 0 would mean a chunked 'tex2' file (magic 0x32786574): none on disc. If `mip` is 0 only level 0
  is read. Type dispatch: byte table 0x2D88A8, jump table 0x2D885C. Flags `[0x4B5828]=1`, `[0x4B5829]=1`,
  `[0x55C110]=0`, alpha scale `[0x4B5830]=0xFF` (static data, no writes in .text).
- Image chain `sub_002D8FA0(w, h, imgtype, extra levels)`: stride = `bpp[imgtype] * w >> 3` (table 0x4B5850:
  P8 8, DXT1 4, DXT3/5 8); a further level is only made while **both** sides are > 4.
- Palette read `sub_002D76B0`: file bytes b0 b1 b2 b3 -> image palette b2 b1 b0 (b3*[0x4B5830]/255).
  Palette upload `sub_002D8F50` swaps bytes 0 and 2 again, so the **D3D palette = the file's bytes** (B G R A).
  The D3DPalette is made by `sub_002D8E60`: Common 0x00030001 (256 entries), 1024 bytes.

| .tex type | count | image type | D3D format | guest bytes |
|---|---|---|---|---|
| 0x01, 0x0E | 3456 + 78 | 5 | P8 | 1024 palette + indices; swizzled per level |
| 0x0F, 0x10 | 7 + 159 | 5 | P8 | 64 byte palette (16 colours) + 4-bit pixels, low nibble first, expanded to 8-bit indices; palette entries 16..255 = 0xFF000000 (opaque black) |
| 0x16 | 267 | 0x0A | DXT1 | copied unchanged |
| 0x18 | 1 | 0x0F | DXT3 | copied unchanged |
| 0x1A | 448 | 0x10 | DXT5 | copied unchanged |

(Type 0x21 maps to image type 0x11 but does not occur; nor do the other types of the jump table.)

- `gfxTexture::Create` `sub_002D6050`: format from table 0x4B5CC0 (swizzled) / 0x4B5C78 (linear) indexed by image
  type; levels = 1 + image chain length (capped at `[0x4B4EAC]` = 8); P8 types get a palette (`sub_002D8E60(0x20,
  0x100)`) filled by `sub_002D8F50`.
- Upload `sub_002DA9F0` per level: LockRect, then by D3D format a row copier (byte table 0x2DABC4, jump table
  0x2DABAC): copy (L8, P8, DXT3/5), copy 2n (16-bit), R/B swap (A8R8G8B8), RGB->BGRX (X8R8G8B8), copy n/2 (DXT1).
  DXT: pitch >> 2 and `height` rows, i.e. the level's bytes unchanged. Others: rows into a temporary buffer then
  `XGSwizzleRect` (0x3821A5, bpp = table/8) into the texture.
- Which mip flag a texture gets depends on the call site (`sub_002D57B0` callers: about 69 pass 1, 33 pass 0),
  so for files with more than one level both variants (all levels, level 0 only) are written.

Verification against runtime dumps (741 PNGs in `textures/dump`): **149** .tex names equal dump names: DXT5 1 level
(82), DXT1 1 level (44), DXT1 3 levels (17, the fx_anim_wtr_bump_* set), DXT5 4 levels (4, fx_alphabet*), P8 type
0x0E (2). With P8 xbck, .tex and the earlier DXT sources, every dump but two 640x480 movie frames is now accounted
for. P4 (fonts, types 0x0F/0x10) is derived from the code only: no font texture was in the dump set.

## 4. Paged pools: resources/**/*.xbpf + .xbpmap

Layout (9 pools; 8 vehicle with a map, `city/hudmap.xbpf` without): `'pf05', u32 page count, u32 page slot size,
u32 table[count]`; page n at `(table[n] & 0xFFFFF) * 2048`, extent `(table[n] >> 20) * 2048`. Records are back to
back in a page; the map's CSV gives `page, offset in page, name, type, size`. Every record has a 0x20 byte header:
`{u32 baked address of the payload (= record + 0x20), 0, 0, u32 record size, 0xCDCDCDCD, ...}`; pointers inside a
record are relative to that address. Without a map, records are walked by their size field.

- **mip0 / mip1..3 records** (1790 + 6 mapped, 160 walked in hudmap): built by the title's pool-texture builder `sub_000E46F0`, which writes this
  exact layout (it is reachable at run time too): D3D texture object at record +0x20 (Data = record + 0x80), pixels
  at record + 0x80, `size - 0x100` bytes of mips. Each record is its own texture (the mip1..3 records are separate 32/16/8 textures,
  not a chain). Formats (mapped pools): DXT5 1580, DXT1 212, P8 4 (rim chevelle_70); hudmap: DXT5 256x256 tiles.
  The page is used in place, so the guest bytes are the record's. 184 of these names were already in `source/`
  (the same textures inside .xbck bundles), confirming header/data offsets.
- **P8 mip0** (4 rims): their D3DPalette lives in the model's `drwShaderModel` record: `{0x01000001, 0, ptr, 0,
  ptr, colours*, "texture name\0"...}` then the palette object `{0x00030001, Data, 0}` with colours at Data. Paired
  by name; `whld_rm_chevelle_70_H` has two palettes (two models), so both variants are written.
- **gfxImage records** (decal 4257, decal_g 1512; vinyls): a baked gfxImage `{u16 w, h, stride, u8 type, u8, u32
  flags, data*, palette*, refs, next*}` at record + 0x30 (pointed to by +0x24) or at record + 0x20; all are type 5
  (P8) 128/256/512 square, no mip chain, palette after the pixels in image byte order (R G B A). The "gfxImage"
  resource factory (`sub_000E57C0`, referenced from data tables) -> `sub_000E5680` -> `sub_000E46F0` turns one into a
  pool texture: D3DFMT_P8, levels = 1 (`sub_00210700` returns 0: `sub_002C2190` is `xor al, al`), indices
  `XGSwizzleRect`ed, D3DPalette 0x00030001 with the 1024 colour bytes, bytes 0 and 2 swapped (-> B G R A).
  **Not verified** against a dump (none of these was dumped), and the vinyl colour options may rewrite the
  palette at run time; the written PNG is the default palette.

## 5. XPR bundles (media/dlcustom.xpr, media/dlres.xpr)

The loose copies were rewritten on 2026-10-21 by an old repack script (size and bytes differ from the disc), so
`extract_all.py` reads them from the disc image (`.local/source-disc.txt`, `xiso.py`). XPR0: `{'XPR0', total, header
size}`, resource headers from +12 (texture 20 bytes, Data = offset into the data block at header size), end
0xFFFFFFFF. dlcustom: 3 DXT3 1024x512; dlres: 17 (DXT3, A8R8G8B8). Loaded as is by `XPR` loading, so hashed like
.xbck textures. A full compare of the 908 disc files with `templates/game/assets`: 903 identical, these two different, 3 updater .xbe files not copied.

## What cannot be produced offline

- Textures the title makes at run time: render targets, movie frames (Bink -> LIN_X8R8G8B8), shadow/reflection
  maps, anything written 4+ times (the runtime never replaces those anyway).
- Vinyl palettes changed by the player's colour choice (only the default palette is produced), and any texture
  whose palette the title edits after load.
- P8 textures drawn with a different palette than the one stored with them (the hash includes the bound palette).

## Results (run of 2026-10-21)

`extract_all.py` visited 88,284 texture variants; `source/` went from 4,776 to **12,714** PNGs (7,938 new, none
undecodable, the 4,776 earlier names all produced again unchanged). New unique textures by source:

| source | new PNGs |
|---|---|
| .xbck P8 (flash UI + 1 city) | 105 |
| .tex P8 / P4 (cutscene peds, fonts, fx) | 286 |
| .tex DXT1 / DXT3 / DXT5 | 228 / 1 / 317 |
| .xbpf mip records DXT5 (rim 667, decal 234, decal_g 234, tire 128, rider 69, brake 38) | 1,370 |
| .xbpf mip records DXT1 (shared_tex) / P8 (rim, with model palettes) | 3 / 5 |
| .xbpf gfxImage vinyls P8 (decal 256/128, decal_g 512/256) | 5,603 |
| XPR (disc copies): DXT3 / A8R8G8B8 | 7 / 13 |
| linear (any source) | 0 (none on disc) |

(The other pool records, e.g. all hudmap tiles, were already in `source/` from .xbck bundles.)
739 of the 741 runtime dumps now have a source of the same name; the two left are 640x480 movie frames.
Spot checks of decoded PNGs: ped faces, P4 font sheets (glyph order correct, so nibble order and swizzle are right),
flash glyphs, rims, vinyl UV layouts (the vinyls use pure red/green, which looks like colour masks the game tints
or a palette it rewrites: unverified, see above).
