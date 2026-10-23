#!/usr/bin/env python3
"""Builds the HD texture pack: the game's dumped textures upscaled with Real-ESRGAN.

The runtime (runtime/gpu/texture_pack.cpp) saves every texture it sees to textures/dump/<hash>.png when "Dump textures"
is on, and shows textures/replace/<hash>.png in place of the console's texture when "HD texture pack" is on. This script
fills replace/ from dump/ and source/ (every texture on the disc, from extract_all.py):

  py -3.12 tools/texture_pack/upscale.py            # every dumped texture not yet in replace/
  py -3.12 tools/texture_pack/upscale.py --force    # all of them again

Each texture is padded before the upscale and cropped after: wrapped along an axis where it tiles (roads,
walls) stay seamless. RGB goes through the network; alpha is resized with Lanczos, so cut-out edges stay where the
console had them. Textures that are flat or tiny gain nothing and are skipped. Needs Pillow and numpy.
"""
import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[2]
ESRGAN_DIR = ROOT / "upscale_tools" / "realesrgan-20220424"
ESRGAN = ESRGAN_DIR / "realesrgan-ncnn-vulkan.exe"
MODELS = {  # name: folder of its .param/.bin (all 4x)
    "ultrasharp-4x": ROOT / "upscale_tools" / "models_extra",   # keeps fine grain (concrete, brick): the default
    "remacri-4x": ROOT / "upscale_tools" / "models_extra",
    "realesrgan-x4plus": ESRGAN_DIR / "models",                  # the first pass's, smoother
}
DEFAULT_MODEL = "ultrasharp-4x"
TEXTURES = ROOT / "templates" / "game" / "out" / "native" / "textures"

NET_SCALE = 4        # the model's
MAX_SIZE = 2048      # longest side of a replacement
MIN_SIZE = 16        # smaller textures are left as they are
FLAT_STDDEV = 2.0    # textures this uniform (0..255) are left as they are


def texture_name(name):
    return Path(name).stem.upper() + ".png"


def source_paths(textures):
    """Always start from console-resolution art; never feed an existing upscale back into the model."""
    found = {}
    for directory in ("source", "dump"):
        for path in sorted((textures / directory).glob("*.png")):
            found.setdefault(path.name, path)
    return found


def excluded_masks(textures):
    """Palette-tinted vinyls are categorical masks, including when only the saved hash list survives."""
    skipped = set()
    index = textures / "source_index.json"
    if index.exists():
        for name, where in json.loads(index.read_text(encoding="utf-8")).items():
            location = where.replace(chr(92), "/").lower()
            if (re.search(r"(?:^|:)resources/vehicle/decal[^/:]*\.xbpf(?:[:@]|$)", location)
                    and re.search(r"\bp8\b", location)):
                skipped.add(name.upper() + ".png")
    hashes = textures / "vinyl_hashes.txt"
    if hashes.exists():
        for line in hashes.read_text(encoding="utf-8").splitlines():
            name = line.strip().removesuffix(".png")
            if re.fullmatch(r"[0-9A-Fa-f]{16}", name):
                skipped.add(name.upper() + ".png")
    return skipped


def publish_json(value, destination):
    """An interrupted bookkeeping write must not erase completed model provenance."""
    destination = Path(destination)
    fd, name = tempfile.mkstemp(prefix=f".{destination.name}.", suffix=".tmp", dir=destination.parent)
    temporary = Path(name)
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as stream:
            json.dump(value, stream, sort_keys=True)
            stream.write("\n")
            stream.flush()
            os.fsync(stream.fileno())
        with temporary.open(encoding="utf-8") as stream:
            json.load(stream)
        os.replace(temporary, destination)
    finally:
        temporary.unlink(missing_ok=True)


def file_digest(path):
    digest = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def pass_context(model):
    return {"model": model, "pipeline_sha256": file_digest(Path(__file__)),
            "models": {suffix: file_digest(MODELS[model] / (model + suffix)) for suffix in (".param", ".bin")}}


def pass_complete(record, source, replacement):
    """A named pass resumes only output produced from these original pixels and still intact on disk."""
    try:
        return (bool(record) and record.get("source_sha256") == file_digest(source)
                and record.get("output_sha256") == file_digest(replacement))
    except OSError:
        return False


def target_scale(w, h):
    """4x, or 2x when 4x would pass MAX_SIZE; 0 skips."""
    for s in (4, 2):
        if max(w, h) * s <= MAX_SIZE:
            return s
    return 0


def pad_of(w, h):
    return max(8, min(w, h) // 8)


def tiles(a):
    """Per axis, whether the texture continues across its opposite edges (a tiling road or wall): the step across
    the seam is no bigger than a typical step inside. Sky panels, decals and UI art do not tile."""
    f = a.astype(np.float32)

    def seam(e1, e2, i1, i2):
        across = np.abs(e1 - e2).mean()
        inside = 0.5 * (np.abs(e1 - i1).mean() + np.abs(e2 - i2).mean())
        return across <= 2.0 * inside + 2.0

    return seam(f[:, 0], f[:, -1], f[:, 1], f[:, -2]), seam(f[0], f[-1], f[1], f[-2])


def edge_pad(a, p, wrap_x, wrap_y):
    """Context for the network past each edge: the opposite edge where the texture tiles, a mirror where it does
    not (wrapping a sky panel or a decal would bleed its far side into the near edge: visible seams)."""
    a = np.pad(a, ((0, 0), (p, p), (0, 0)), mode="wrap" if wrap_x else "reflect")
    return np.pad(a, ((p, p), (0, 0), (0, 0)), mode="wrap" if wrap_y else "reflect")


def replacement_valid(path, source):
    """A resumed batch must not mistake an interrupted or wrong-size PNG for a finished replacement."""
    try:
        with Image.open(source) as original:
            w, h = original.size
        scale = target_scale(w, h)
        with Image.open(path) as replacement:
            if not scale or replacement.format != "PNG" or replacement.size != (w * scale, h * scale):
                return False
            replacement.verify()
        return True
    except (OSError, ValueError, SyntaxError):
        return False


def publish_png(image, destination):
    """Verify a completed sibling file before atomically replacing the final image."""
    destination = Path(destination)
    fd, name = tempfile.mkstemp(prefix=f".{destination.name}.", suffix=".tmp", dir=destination.parent)
    temporary = Path(name)
    try:
        with os.fdopen(fd, "wb") as stream:
            image.save(stream, format="PNG", optimize=False)
            stream.flush()
            os.fsync(stream.fileno())
        with Image.open(temporary) as check:
            if check.format != "PNG" or check.size != image.size:
                raise ValueError("Encoded replacement has an unexpected format or size")
            check.verify()
        os.replace(temporary, destination)
    finally:
        temporary.unlink(missing_ok=True)


def inspect_source(src):
    """Classify the original pixels once for both the GPU pipeline and the CPU coverage audit."""
    with Image.open(src) as original:
        im = original.convert("RGBA")
    w, h = im.size
    if min(w, h) < MIN_SIZE:
        return None, None, "tiny"
    a = np.asarray(im)
    if a[..., :3].std() < FLAT_STDDEV and a[..., 3].std() < FLAT_STDDEV:
        return None, None, "flat"
    s = target_scale(w, h)
    if not s:
        return None, None, "size_cap"
    p = min(pad_of(w, h), w - 1, h - 1)
    wrap_x, wrap_y = tiles(a[..., :3])
    job = {"size": (w, h), "pad": p, "scale": s, "alpha": a[..., 3],
           "wrap_x": bool(wrap_x), "wrap_y": bool(wrap_y)}
    return job, a[..., :3], None


def prepare(src, work):
    """The dumped texture's padded RGB for the network, and what to finish it with; None to skip."""
    job, rgb, _reason = inspect_source(src)
    if job:
        Image.fromarray(edge_pad(rgb, job["pad"], job["wrap_x"], job["wrap_y"])).save(work / src.name)
    return job


def finish(job, upscaled, dst):
    w, h = job["size"]
    p, s = job["pad"], job["scale"]
    with Image.open(upscaled) as network:
        rgb = network.convert("RGB")
    k = NET_SCALE
    if rgb.size != ((w + 2 * p) * k, (h + 2 * p) * k):
        raise ValueError(f"Unexpected network image size {rgb.size} for {w}x{h}")
    rgb = rgb.crop((p * k, p * k, (p + w) * k, (p + h) * k))
    out = (w * s, h * s)
    if rgb.size != out:
        rgb = rgb.resize(out, Image.LANCZOS)
    alpha = job["alpha"]
    if alpha.min() == 255:
        a = Image.new("L", out, 255)
    else:
        a = Image.fromarray(alpha, "L").resize(out, Image.LANCZOS)
    rgba = rgb.convert("RGBA")
    rgba.putalpha(a)
    publish_png(rgba, dst)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--textures", type=Path, default=TEXTURES, help="the game's textures folder (dump/ and replace/)")
    ap.add_argument("--force", action="store_true", help="redo textures already in replace/")
    ap.add_argument("--batch", type=int, default=64, help="textures per Real-ESRGAN run")
    ap.add_argument("--model", default=DEFAULT_MODEL, choices=sorted(MODELS),
                    help="redo every replacement not yet made with this model (tracked in upscale_state.json)")
    ap.add_argument("--pass-id", help="a fresh named pass from originals; reusing its name resumes verified completed textures")
    ap.add_argument("--stop-file", type=Path, help="stop after the current completed batch when this file exists")
    args = ap.parse_args()
    if args.batch < 1:
        ap.error("--batch must be at least 1")

    if not ESRGAN.exists():
        sys.exit(f"Real-ESRGAN not found: {ESRGAN}")
    for suffix in (".param", ".bin"):
        model_file = MODELS[args.model] / (args.model + suffix)
        if not model_file.is_file():
            sys.exit(f"Model file not found: {model_file}")
    replace = args.textures / "replace"
    replace.mkdir(parents=True, exist_ok=True)
    found = source_paths(args.textures)
    # vinyl masks (P8 in the car decal pools) are tinted by the game at run time: a replacement shows untinted or
    # not at all, so they are never upscaled (docs/notes/visual_fixes.md)
    skip = excluded_masks(args.textures)
    state_path = args.textures / "upscale_state.json"
    state = json.loads(state_path.read_text()) if state_path.exists() else {}
    passes_path = args.textures / "upscale_passes.json"
    passes = json.loads(passes_path.read_text()) if passes_path.exists() else {"version": 1, "passes": {}}
    current_pass = None
    if args.pass_id:
        context = pass_context(args.model)
        current_pass = passes["passes"].setdefault(args.pass_id, {"context": context, "completed": {}})
        if current_pass["context"] != context:
            ap.error("this --pass-id used different model weights or pipeline code; choose a new pass name")
    todo = [f for n, f in sorted(found.items()) if texture_name(n) not in skip and
            (args.force or state.get(n) != args.model or not replacement_valid(replace / n, f)
             or (current_pass is not None and not pass_complete(current_pass["completed"].get(n), f, replace / n)))]
    print(f"{len(todo)} textures to upscale with {args.model} ({len(found)} known, {len(skip)} vinyl masks skipped)")

    done = skipped = failed = 0
    for i in range(0, len(todo), args.batch):
        if args.stop_file and args.stop_file.exists():
            print(f"Stopped at batch boundary: remove {args.stop_file} to resume", flush=True)
            return 2
        batch = todo[i:i + args.batch]
        with tempfile.TemporaryDirectory() as tmp:
            src_dir, out_dir = Path(tmp) / "in", Path(tmp) / "out"
            src_dir.mkdir()
            out_dir.mkdir()
            jobs = {}
            for f in batch:
                try:
                    job = prepare(f, src_dir)
                except Exception as e:  # a damaged dump: leave it
                    print(f"  {f.name}: {e}")
                    failed += 1
                    continue
                if job:
                    if current_pass is not None:
                        job["source_sha256"] = file_digest(f)
                    jobs[f.name] = job
                else:
                    skipped += 1
            if not jobs:
                continue
            r = subprocess.run([str(ESRGAN), "-i", str(src_dir), "-o", str(out_dir), "-n", args.model,
                                "-m", str(MODELS[args.model]), "-f", "png"],
                               capture_output=True, text=True)
            for name, job in jobs.items():
                up = out_dir / name
                if not up.exists():
                    failed += 1
                    continue
                try:
                    finish(job, up, replace / name)
                    state[name] = args.model
                    if current_pass is not None:
                        current_pass["completed"][name] = {"source_sha256": job["source_sha256"],
                                                           "output_sha256": file_digest(replace / name)}
                    done += 1
                except (OSError, ValueError, SyntaxError) as e:
                    failed += 1
                    print(f"  {name}: {e}")
            if r.returncode and failed:
                print(r.stderr[-500:])
        publish_json(state, state_path)
        if current_pass is not None:
            publish_json(passes, passes_path)
        print(f"  {min(i + args.batch, len(todo))}/{len(todo)}: {done} upscaled, {skipped} skipped, {failed} failed")
    print(f"done: {done} replacements written to {replace}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
