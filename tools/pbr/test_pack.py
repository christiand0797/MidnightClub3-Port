#!/usr/bin/env python3
"""Builds a small materials-v1 test pack for the dedicated PBR path from one frame capture.

    py -3.12 -I tools/pbr/test_pack.py --latest                     # newest capture beside mc3.exe
    py -3.12 -I tools/pbr/test_pack.py CAPTURE_DIR --style neutral --install

The capture must come from a build that writes each draw's `keys vs .. ps ..` line and each texture stage's
`hash .. tci ..` (2026-10-29 or later): those are the exact identities a material assignment names. A draw is
picked when the capture shows what TryPbrDraw can check up front: a fixed-function `mul(ip, wvp)` vertex shader, one
texture stage that modulates (cop 4), no blending, depth write on and a texture pack hash. The game still applies
its own gates (exact texture*diffuse cascade, constant alpha, no alpha test, mesh/scene proofs); the F1 PBR catalog
says per material which one stopped it.

Each material's albedo is a byte copy of the colour the game actually uploads for that hash: the HD replacement
(textures/replace, when the texture pack is on) or else the console texture (textures/source). Its alpha is
untouched, so the alpha digest matches. Normal is flat, ORM is AO 1 / roughness 0.5 / metal 0.
--style marked (default) tints albedo RGB 40% toward magenta so replaced surfaces are obvious; --style neutral keeps
it unchanged, which should look like the original (the fidelity check).

It writes PACK/materials-v1.toml and the PNGs, prints the manifest SHA-256 to approve, and with --install puts
pack_path/approved_manifest_sha256/enabled into the [pbr] table of settings.toml (a .bak copy is kept first).
"""
import argparse
import hashlib
import re
import shutil
import struct
import sys
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
NATIVE = ROOT / "templates" / "game" / "out" / "native"
MAX_MATERIALS = 1024  # LoadPolicy::max_materials
DECODE_BUDGET = 1536 << 20  # LoadPolicy::max_decoded_bytes, every material's maps and mips decoded separately
FLAT = 4  # flat normal/ORM maps: only the albedo must match the game texture's size
AUX = 256  # --style real: generated normal/ORM/height maps are resized to this (budget), the albedo is not

DRAW = re.compile(r"^(\d{4}) draw .* vs ([0-9A-F]+) ps ([0-9A-F]+) blend (\d+) \S+ \S+ zen (\d+) \S+ zwrite (\d+)")
STAGE = re.compile(r"t(\d) [0-9A-F]+ fmt [0-9A-F]+ \d+x\d+(?P<flags>[^|]*?) addr \d+ filt \d+ cop (\d+)(?: neon)? "
                   r"hash ([0-9A-F]{16}) tci (\d+)")
KEYS = re.compile(r"^\s+keys vs ([0-9A-F]{16}) ps ([0-9A-F]{16})$")


def candidates(capture):
    """{(vs key, ps key, texture hash, uv index): draws} for draws the capture shows as reachable."""
    fixed = {}

    def is_fixed(vkey):
        if vkey not in fixed:
            p = capture / f"vs_{vkey}.hlsl"
            fixed[vkey] = p.exists() and "mul(ip, wvp)" in p.read_text(encoding="utf-8", errors="replace")
        return fixed[vkey]

    found, cur = Counter(), None
    for line in (capture / "frame.txt").read_text(encoding="utf-8", errors="replace").splitlines():
        m = DRAW.match(line)
        if m:
            stages = [STAGE.match(s.strip()) for s in line.split("| ")[1:]]
            blend, zen, zwrite = m.group(4), m.group(5), m.group(6)
            ok = (len(stages) == 1 and stages[0] and blend == "0" and zen == "1" and zwrite == "1" and
                  "target" not in stages[0]["flags"] and "unbound" not in stages[0]["flags"] and
                  " neon" not in line and stages[0].group(3) == "4" and int(stages[0].group(4), 16) != 0)
            cur = (stages[0].group(4), int(stages[0].group(5))) if ok else None
            continue
        if re.match(r"^\d{4} ", line):
            cur = None
            continue
        k = KEYS.match(line)
        if k and cur:
            if is_fixed(k.group(1)):
                found[(k.group(1), k.group(2), cur[0], cur[1])] += 1
            cur = None
    return found


def accepted_colour(textures, hash_hex, hd, replace_index):
    """Path of the image the game uploads for this content hash, or None."""
    if hd and hash_hex in replace_index:
        return replace_index[hash_hex]
    p = textures / "source" / f"{hash_hex}.png"
    return p if p.exists() else None


def index_replacements(textures):
    """Mirror texture_pack.cpp Index(): recursive, stem starts with 16 hex digits. Later entries win."""
    out = {}
    for p in sorted((textures / "replace").rglob("*")):
        if p.is_file() and p.suffix.lower() in (".png", ".jpg", ".jpeg", ".bmp") and len(p.stem) >= 16:
            try:
                out[f"{int(p.stem[:16], 16):016X}"] = p
            except ValueError:
                pass
    return out


def alpha_digest(width, height, alpha_bytes):
    """pbr::AlphaDigest: SHA256(width u32le || height u32le || level-0 alpha bytes)."""
    return hashlib.sha256(struct.pack("<II", width, height) + alpha_bytes).hexdigest()


def sha256_file(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def texture_pack_enabled(settings):
    if not settings.exists():
        return True
    m = re.search(r"(?ms)^\[graphics\]\s*$(.*?)(?=^\[|\Z)", settings.read_text(encoding="utf-8"))
    t = re.search(r"(?m)^texture_pack\s*=\s*(true|false)", m.group(1)) if m else None
    return t is None or t.group(1) == "true"


def install(settings, pack_name, digest):
    text = settings.read_text(encoding="utf-8")
    m = re.search(r"(?ms)^\[pbr\]\s*$(.*?)(?=^\[|\Z)", text)
    if not m:
        raise SystemExit(f"{settings}: no [pbr] table; open the F1 PBR menu once so the game writes it")
    body = m.group(1)
    for key, value in (("enabled", "true"), ("pack_path", f"'{pack_name}'"), ("approved_manifest_sha256", f"'{digest}'")):
        line = f"{key} = {value}"
        body, n = re.subn(rf"(?m)^{key}\s*=.*$", line, body, count=1)
        if not n:
            body = f"\n{line}" + body
    shutil.copy2(settings, settings.with_suffix(".toml.bak"))
    settings.write_text(text[:m.start(1)] + body + text[m.end(1):], encoding="utf-8")


def decoded_bytes(w, h):
    """RGBA8 level 0 plus every mip down to 1x1, as the loader keeps them."""
    total = 0
    while True:
        total += w * h * 4
        if w == 1 and h == 1:
            return total
        w, h = max(w // 2, 1), max(h // 2, 1)


def generated_maps(textures, tex, out):
    """tools/pbr/generate.py's maps for this hash (textures/pbr/<hash>_n/_h/_r/_ao, optional _m) as the loader wants
    them: an RGBA normal kept unit length with z > 0, ORM = AO/roughness/metal, a 16-bit grey height; None if absent."""
    from PIL import Image
    import numpy as np
    base = textures / "pbr"
    need = {k: base / f"{tex}_{k}.png" for k in ("n", "h", "r", "ao")}
    if not all(p.exists() for p in need.values()):
        return None
    size = (AUX, AUX)
    n = np.asarray(Image.open(need["n"]).convert("RGB").resize(size, Image.LANCZOS)).astype(np.float32) / 127.5 - 1
    n[..., 2] = np.maximum(n[..., 2], 0.2)
    n /= np.linalg.norm(n, axis=2, keepdims=True)
    normal = np.dstack([np.clip(np.round((n + 1) * 127.5), 0, 255).astype(np.uint8), np.full(size, 255, np.uint8)])
    gray = lambda k: np.asarray(Image.open(need[k]).convert("L").resize(size, Image.LANCZOS))
    metal_path = base / f"{tex}_m.png"
    metal = np.zeros(size, np.uint8)
    if metal_path.exists():
        metal = np.asarray(Image.open(metal_path).convert("L").resize(size, Image.LANCZOS))
    orm = np.dstack([gray("ao"), gray("r"), metal, np.full(size, 255, np.uint8)])
    height = np.asarray(Image.open(need["h"]).convert("I").resize(size, Image.LANCZOS), np.float64)
    height = np.clip(height if height.max() > 255 else height * 257, 0, 65535).astype(np.uint16)
    out_files = {}
    for role, image in (("normal", Image.fromarray(normal, "RGBA")), ("orm", Image.fromarray(orm, "RGBA")),
                        ("height", Image.fromarray(height, "I;16"))):
        path = out / f"{tex}_{role}.png"
        image.save(path, optimize=False)
        out_files[role] = (path.name, sha256_file(path))
    return out_files


def build(found, textures, out, style, limit, hd, keep_varying=True, budget=DECODE_BUDGET * 7 // 8):
    from PIL import Image
    import numpy as np

    replace_index = index_replacements(textures) if hd else {}
    out.mkdir(parents=True, exist_ok=True)
    for old in out.glob("*.png"):
        old.unlink()
    review = hashlib.sha256(f"claude test pack, style {style}, not a reviewed material".encode()).hexdigest()
    entries, skipped, written = [], Counter(), {}
    flat = {}
    for role, value in (("normal", [128, 128, 255, 255]), ("orm", [255, 128, 0, 255])):
        path = out / f"flat_{role}.png"
        Image.fromarray(np.full((FLAT, FLAT, 4), value, np.uint8), "RGBA").save(path, optimize=False)
        flat[role] = (path.name, sha256_file(path))
    used = 0
    for (vkey, pkey, tex, uv), draws in found.most_common():
        if len(entries) >= limit:
            skipped["over limit"] += 1
            continue
        src = accepted_colour(textures, tex, hd, replace_index)
        if not src or src.suffix.lower() != ".png":
            skipped["no PNG for the accepted colour"] += 1
            continue
        if tex not in written:
            # IHDR bit depth: Pillow narrows 16-bit images itself, which need not match the game's WIC decode.
            depth = src.read_bytes()[24]
            im = Image.open(src)
            if im.mode not in ("RGB", "RGBA", "P", "L", "LA") or depth > 8:
                skipped[f"image mode {im.mode}/{depth}-bit"] += 1
                continue
            rgba = np.asarray(im.convert("RGBA")).copy()
            alpha = rgba[..., 3]
            if alpha.min() != alpha.max() and not keep_varying:
                skipped["varying alpha (dropped by --constant-alpha-only)"] += 1
                continue
            if style == "marked":
                rgba[..., :3] = (rgba[..., :3] * 0.6 + np.array([255, 0, 255]) * 0.4).round().astype(np.uint8)
            h, w = alpha.shape
            path = out / f"{tex}_albedo.png"
            Image.fromarray(rgba, "RGBA").save(path, optimize=False)
            files = {"albedo": (path.name, sha256_file(path)), **flat}
            cost = decoded_bytes(w, h) + 2 * decoded_bytes(FLAT, FLAT)
            maps = generated_maps(textures, tex, out) if style == "real" else None
            if maps:
                files.update(maps)
                cost = decoded_bytes(w, h) + 2 * decoded_bytes(AUX, AUX) + decoded_bytes(AUX, AUX) // 2
            elif style == "real":
                skipped["no generated maps (flat normal/ORM)"] += 1
            written[tex] = (files, alpha_digest(w, h, alpha.tobytes()), cost)
        files, baseline, cost = written[tex]
        if used + cost > budget:
            skipped["over the loader's decode budget"] += 1
            continue
        used += cost
        entries.append((vkey, pkey, tex, uv, draws, files, baseline))
    lines = ["# Generated by tools/pbr/test_pack.py: a TEST pack, not reviewed materials.", "schema = 1", ""]
    for vkey, pkey, tex, uv, draws, files, baseline in entries:
        lines += ["[[materials]]", f"# {draws} captured draw(s)", f'texture = "{tex}"', f'vertex_shader = "{vkey}"',
                  f'pixel_shader = "{pkey}"', "stage = 0", f"uv_set = {uv}", 'surface = "world"', "cutout = false",
                  'normal_convention = "opengl"', f'baseline_alpha_sha256 = "{baseline}"', f'review_sha256 = "{review}"']
        for role, space in (("albedo", "srgb"), ("normal", "linear"), ("orm", "linear"), ("height", "linear")):
            if role not in files:
                continue
            name, digest = files[role]
            lines.append(f'{role} = {{ file = "{name}", sha256 = "{digest}", color_space = "{space}" }}')
        lines.append("")
    manifest = out / "materials-v1.toml"
    manifest.write_bytes("\n".join(lines).encode("utf-8"))
    return entries, skipped, sha256_file(manifest)


CITIES = ("detroit", "sd", "atlanta", "tokyo")


# surfaces the camera runs along: their generated maps keep more texels than the rest
ROAD_CLASSES = ("road_asphalt", "road_marking", "road_concrete", "concrete", "terrain", "pavement")


def city_map_sizes(cls, orig):
    """(normal, ORM, height) sizes for a city texture: roads and ground get 512/256/128, everything else its own size
    (32 to 128) and a height at half that; together inside the 1.5 GB decode budget for every city texture."""
    if cls in ROAD_CLASSES:
        return 512, 256, 128
    size = min(max(orig, 32), 128)
    return size, size, max(size // 2, 16)


def cities_pack(native, out):
    """Every world texture of the four cities (textures/source_index.json) as an "any shader" material whose albedo is
    the game's own texture, so no capture or albedo copy is needed: the generated normal, AO/roughness/metal and height
    maps at city_map_sizes (the baked city lighting shades the normal map as relief and a roughness gloss).
    Returns (materials written, textures without generated maps, manifest sha256)."""
    import json
    import re
    from PIL import Image
    import numpy as np
    textures = native / "textures"
    index = json.load(open(textures / "source_index.json", encoding="utf-8"))
    pbr = textures / "pbr"
    classes = json.load(open(pbr / "manifest.json", encoding="utf-8")).get("textures", {})
    out.mkdir(parents=True, exist_ok=True)
    for old in out.glob("*.png"):
        old.unlink()
    review = hashlib.sha256(b"claude city pack: game albedo, generated normal/AO/roughness/height").hexdigest()
    lines = ["# Generated by tools/pbr/test_pack.py --cities: every city texture, the game's own albedo.", "schema = 1", ""]
    written, missing = 0, 0
    for h, where in sorted(index.items()):
        if not re.search(r"resources/city/(%s)" % "|".join(CITIES), where):
            continue
        need = {k: pbr / f"{h}_{k}.png" for k in ("n", "h", "r", "ao")}
        if not all(path.exists() for path in need.values()):
            missing += 1
            continue
        entry = classes.get(h, {})
        normal_size, orm_size, height_size = city_map_sizes(entry.get("class", ""), max(entry.get("orig", [128])))
        size = (orm_size, orm_size)
        gray = lambda k, sz: np.asarray(Image.open(need[k]).convert("L").resize(sz, Image.LANCZOS))
        metal = np.zeros(size, np.uint8)
        if (pbr / f"{h}_m.png").exists():
            metal = np.asarray(Image.open(pbr / f"{h}_m.png").convert("L").resize(size, Image.LANCZOS))
        orm = np.dstack([gray("ao", size), gray("r", size), metal, np.full(size, 255, np.uint8)])
        normal = Image.open(need["n"]).convert("RGB").resize((normal_size, normal_size), Image.LANCZOS)
        normal = np.dstack([np.asarray(normal), np.full((normal_size, normal_size), 255, np.uint8)])
        height = np.asarray(Image.open(need["h"]).convert("I").resize((height_size, height_size), Image.LANCZOS), np.float64)
        height = np.clip(height if height.max() > 255 else height * 257, 0, 65535).astype(np.uint16)
        files = {}
        for role, image in (("normal", Image.fromarray(normal, "RGBA")), ("orm", Image.fromarray(orm, "RGBA")),
                            ("height", Image.fromarray(height, "I;16"))):
            path = out / f"{h}_{role}.png"
            image.save(path, optimize=False)
            files[role] = (path.name, sha256_file(path))
        lines += ["[[materials]]", f'texture = "{h}"', 'vertex_shader = "any"', 'pixel_shader = "any"', "stage = 0",
                  'uv_set = "any"', 'surface = "world"', "cutout = false", 'normal_convention = "opengl"',
                  f'baseline_alpha_sha256 = "{review}"', f'review_sha256 = "{review}"', 'albedo = "game"']
        for role in ("normal", "orm", "height"):
            name, digest = files[role]
            lines.append(f'{role} = {{ file = "{name}", sha256 = "{digest}", color_space = "linear" }}')
        lines.append("")
        written += 1
    manifest = out / "materials-v1.toml"
    manifest.write_bytes("\n".join(lines).encode("utf-8"))
    return written, missing, sha256_file(manifest)


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("captures", nargs="*", type=Path, help="one or more capture folders; assignments are merged")
    ap.add_argument("--latest", type=int, nargs="?", const=1, default=0, metavar="N",
                    help="also use the newest N captures beside mc3.exe (default 1)")
    ap.add_argument("--constant-alpha-only", action="store_true",
                    help="skip textures whose alpha varies (the game accepts them only where the written alpha is "
                         "provably zero: no blending, ALWAYS alpha test, zero texture-factor alpha)")
    ap.add_argument("--style", choices=("marked", "neutral", "real"), default="marked",
                    help="marked tints albedo magenta; neutral keeps every map flat; real adds the generated "
                         "normal/ORM/height maps from textures/pbr (tools/pbr/generate.py)")
    ap.add_argument("--out", type=Path, help="pack folder (default: pbr-test-STYLE beside mc3.exe)")
    ap.add_argument("--limit", type=int, default=128, help=f"materials, at most {MAX_MATERIALS}")
    ap.add_argument("--native", type=Path, default=NATIVE, help="folder holding mc3.exe, settings.toml, textures")
    ap.add_argument("--install", action="store_true", help="point settings.toml [pbr] at the pack")
    ap.add_argument("--cities", action="store_true",
                    help="every world texture of the four cities with generated maps, the game's own albedo (no captures)")
    a = ap.parse_args(argv)
    if a.cities:
        out = a.out or a.native / "pbr-cities"
        written, missing, digest = cities_pack(a.native, out)
        print(f"wrote {written} city materials to {out}; {missing} textures have no generated maps yet")
        print(f"approved_manifest_sha256 = {digest}")
        if a.install:
            rel = out.relative_to(a.native) if out.is_relative_to(a.native) else out
            install(a.native / "settings.toml", rel.as_posix(), digest)
            print(f"settings.toml [pbr] now uses pack_path '{rel.as_posix()}'")
        return 0
    if a.latest:
        caps = sorted((p for p in (a.native / "captures").glob("*/") if (p / "frame.txt").exists()),
                      key=lambda p: p.stat().st_mtime)
        if not caps:
            raise SystemExit("no captures beside mc3.exe; press F4 in game first")
        a.captures += caps[-a.latest:]
    if not a.captures or not all((c / "frame.txt").exists() for c in a.captures):
        ap.error("give capture folders or --latest")
    if not 1 <= a.limit <= MAX_MATERIALS:
        ap.error(f"--limit must be 1..{MAX_MATERIALS}")
    found = Counter()
    for capture in a.captures:
        one = candidates(capture)
        if not one:
            text = (capture / "frame.txt").read_text(encoding="utf-8", errors="replace")
            why = "it predates the keys/hash capture lines; take a new one" if " keys vs " not in text else \
                  "no draw in it is a fixed-function, single-stage, opaque, modulated, packed texture"
            raise SystemExit(f"{capture.name}: no candidate draws ({why})")
        found.update(one)
    settings = a.native / "settings.toml"
    hd = texture_pack_enabled(settings)
    out = a.out or a.native / f"pbr-test-{a.style}"
    entries, skipped, digest = build(found, a.native / "textures", out, a.style, a.limit, hd, not a.constant_alpha_only)
    names = ", ".join(c.name.split("-")[0] for c in a.captures)
    print(f"captures {names}: {len(found)} candidate assignments, {sum(found.values())} draws")
    print(f"texture pack {'on: HD replacements' if hd else 'off: console textures'} are the accepted colour")
    print(f"wrote {len(entries)} materials to {out}")
    for why, n in skipped.most_common():
        print(f"  skipped {n}: {why}")
    if not entries:
        return 1
    print(f"approved_manifest_sha256 = {digest}")
    if a.install:
        rel = out.relative_to(a.native) if out.is_relative_to(a.native) else out
        install(settings, rel.as_posix(), digest)
        print(f"settings.toml [pbr] now uses pack_path '{rel.as_posix()}' (previous file kept as settings.toml.bak)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
