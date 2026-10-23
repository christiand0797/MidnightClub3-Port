#!/usr/bin/env python3
"""Generates PBR material maps for the game's textures -> templates/game/out/native/textures/pbr/.

  py -3.12 tools/pbr/generate.py --set tools/pbr/sets/starter50.json        # the curated starter set
  py -3.12 tools/pbr/generate.py --hash D4B3A26FD275FFE4 --class brick      # one texture
  py -3.12 tools/pbr/generate.py --classes --only road_asphalt --limit 200   # from classify.py's classes.json
  options: --method ensemble|hybrid|pbrify|deep|classic   --device cpu|dml   --max-size 1024
           --input source|replace   --work-scale 4   --force (redo textures already done with the same settings)

Per texture it reads the console's own texture (textures/source/<hash>.png) and enlarges it --work-scale times
(Lanczos; 4 = the HD pack's size), or with --input replace the HD pack image (textures/replace/<hash>.png); the source is
the default so the maps follow the original artwork rather than an upscaler's grain. It works at up to --max-size on
the long side and writes <hash>_n/_h/_r/_ao
(+ _m for classes that can be bare metal) next to a manifest, pbr/manifest.json, recording class, inputs and settings.
Writes go to a .part file first and are renamed, so an interrupted run never leaves a half-written map.

The runtime does not read any of this yet (docs/PBR_PLAN.md, phase R1+); the game renders exactly as before.
Models (fetch with --download-models, SHA-256 checked): DeepBump tools/pbr/models/deepbump256.onnx (onnxruntime) and
PBRify_Remix tools/pbr/models/pbrify/*.pth (torch + spandrel). A method whose model is missing falls back to a simpler
one; the manifest records the method each texture actually got.
"""
import argparse
import hashlib
import json
import sys
import time
import urllib.request
from pathlib import Path


sys.path.insert(0, str(Path(__file__).resolve().parent))
import pbrlib as P  # noqa: E402

MANIFEST = P.PBR / "manifest.json"


def download_models():
    """DeepBump's ONNX file and PBRify_Remix's three SPAN .pth files (only those are taken from the release zip),
    each checked against the SHA-256 recorded in pbrlib."""
    P.MODEL_DIR.mkdir(parents=True, exist_ok=True)
    if not P.DEEPBUMP.exists():
        tmp = P.DEEPBUMP.with_suffix(".part")
        print(f"downloading {P.DEEPBUMP_URL}")
        urllib.request.urlretrieve(P.DEEPBUMP_URL, tmp)
        digest = hashlib.sha256(tmp.read_bytes()).hexdigest()
        if digest != P.DEEPBUMP_SHA256:
            tmp.unlink()
            raise SystemExit(f"DeepBump hash mismatch: {digest}")
        tmp.replace(P.DEEPBUMP)
    if not all((P.PBRIFY_DIR / f).exists() for f in P.PBRIFY_SHA256):
        import tempfile
        import zipfile
        P.PBRIFY_DIR.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory() as td:
            z = Path(td) / "pbrify.zip"
            print(f"downloading {P.PBRIFY_URL}")
            urllib.request.urlretrieve(P.PBRIFY_URL, z)
            with zipfile.ZipFile(z) as zf:
                for info in zf.infolist():
                    name = Path(info.filename).name
                    if name in P.PBRIFY_SHA256 or (name in ("LICENSE", "README.md") and "/Extras/" not in info.filename):
                        data = zf.read(info)
                        if name in P.PBRIFY_SHA256 and hashlib.sha256(data).hexdigest() != P.PBRIFY_SHA256[name]:
                            raise SystemExit(f"PBRify {name}: hash mismatch")
                        (P.PBRIFY_DIR / name).write_bytes(data)
    print("models ok:", P.DEEPBUMP.exists(), P.PBRify.available())


def load_manifest():
    if MANIFEST.exists():
        return json.load(open(MANIFEST, encoding="utf-8"))
    return {"version": P.MAPS_VERSION, "normal_convention": "opengl_y_up", "textures": {}}


def save_manifest(m):
    P.PBR.mkdir(parents=True, exist_ok=True)
    tmp = MANIFEST.with_suffix(".part")
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(m, f, indent=1, sort_keys=True)
    tmp.replace(MANIFEST)


def jobs_from_args(args, index):
    jobs = []
    if args.set:
        s = json.load(open(args.set, encoding="utf-8"))
        jobs += [(t["hash"].upper(), t["class"]) for t in s["textures"]]
    if args.hash:
        jobs += [(h.upper(), args.cls or "generic") for h in args.hash]
    if args.classes:
        c = json.load(open(P.PBR / "classes.json"))["textures"]
        for h, v in c.items():
            if v["class"] in ("skip", "ui"):
                continue
            if args.only and v["class"] not in args.only:
                continue
            jobs.append((h, v["class"]))
    seen, out = set(), []
    for h, cls in jobs:
        if h in seen:
            continue
        if h not in index:
            print(f"{h}: not in source_index.json, skipped")
            continue
        seen.add(h)
        out.append((h, cls))
    return out[: args.limit] if args.limit else out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--set", help="set file (tools/pbr/sets/*.json)")
    ap.add_argument("--hash", nargs="*", help="texture hashes")
    ap.add_argument("--class", dest="cls", help="material class for --hash")
    ap.add_argument("--classes", action="store_true", help="take textures and classes from pbr/classes.json")
    ap.add_argument("--only", nargs="*", help="with --classes: these classes only")
    ap.add_argument("--limit", type=int, default=0)
    ap.add_argument("--method", choices=list(P.METHODS), default="ensemble")
    ap.add_argument("--device", choices=["cpu", "dml"], default="cpu",
                    help="DeepBump on the CPU (default, leaves the GPU to the upscalers) or DirectML")
    ap.add_argument("--max-size", type=int, default=1024)
    ap.add_argument("--input", choices=["source", "replace"], default="source",
                    help="source (default): the console texture enlarged by --work-scale; replace: the HD pack image")
    ap.add_argument("--work-scale", type=float, default=4.0, help="enlargement of source textures (4 = HD pack size)")
    ap.add_argument("--force", action="store_true")
    ap.add_argument("--threads", type=int, default=6, help="CPU threads for the PBRify networks")
    ap.add_argument("--download-models", action="store_true")
    args = ap.parse_args()

    if args.download_models:
        download_models()
        if not (args.set or args.hash or args.classes):
            return
    index = P.load_index()
    jobs = jobs_from_args(args, index)
    if not jobs:
        ap.error("nothing to do: give --set, --hash or --classes")

    models = {}
    method = args.method
    if method in ("ensemble", "hybrid", "deep"):
        if P.DeepBump.available():
            models["deep"] = P.DeepBump(args.device)
            print("DeepBump on", models["deep"].providers[0])
        else:
            print("DeepBump unavailable (tools/pbr/models/deepbump256.onnx or onnxruntime missing)")
    if method in ("ensemble", "pbrify"):
        if P.PBRify.available():
            models["pbrify"] = P.PBRify(args.threads)
            print("PBRify on the CPU")
        else:
            print("PBRify unavailable (tools/pbr/models/pbrify/*.pth, torch or spandrel missing)")

    man = load_manifest()
    settings = {"method": method, "max_size": args.max_size, "input": args.input, "work_scale": args.work_scale,
                "version": P.MAPS_VERSION}
    t_all = time.time()
    done = 0
    for i, (h, cls) in enumerate(jobs):
        prev = man["textures"].get(h)
        if prev and not args.force and prev.get("settings") == settings and prev.get("class") == cls and \
                all((P.PBR / f"{h}_{k}.png").exists() for k in prev.get("maps", [])):
            continue
        e = P.parse_index_entry(index[h])
        if not (P.SOURCE / f"{h}.png").exists():
            print(f"{h}: no image")
            continue
        t = time.time()
        rgba, src, scale = P.load_working(h, e, args.input, args.work_scale, args.max_size)
        H, W = rgba.shape[:2]
        res = P.generate(rgba, cls, scale, models, method)
        overridden = P.apply_overrides(h, res)
        if overridden:
            res.info["overrides"] = overridden
        maps = P.write(h, res)
        man["textures"][h] = {
            "class": cls, "input": src, "size": [W, H], "orig": [e["w"], e["h"]], "fmt": e["fmt"],
            "from": e["loc"], "maps": maps, "settings": settings, "stats": res.info,
            "seconds": round(time.time() - t, 2),
        }
        done += 1
        print(f"[{i + 1}/{len(jobs)}] {h} {cls:14s} {src:7s} {W}x{H} maps={','.join(maps)} "
              f"r={res.info['rough_mean']:.2f} {time.time() - t:.1f}s", flush=True)
        if done % 10 == 0:
            save_manifest(man)
    man["materials"] = P.material_dict()
    save_manifest(man)
    print(f"{done} textures in {time.time() - t_all:.0f}s -> {P.PBR}")


if __name__ == "__main__":
    main()
