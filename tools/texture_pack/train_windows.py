#!/usr/bin/env python3
"""Fill the tram/subway windows in the HD replacements with passengers behind glass.

The trains (va_tram_sd, va_train_detroit, va_train_tokyo) have one texture each. Its window texels are alpha 0: the
shell's alpha test cuts them open onto the interior and passenger meshes, which the game draws only when the camera
is right beside a train, so everywhere else the windows showed the street straight through the car. The art stored in
those texels is the interior's ads, so turning them opaque shows brand logos instead.

This rewrites textures/replace/<hash>.png for the three: every alpha-0 window area outside the passenger block
(u 0.51..1, v 0..0.5, the art people.mesh samples, whose own cut-outs must stay) gets the texture's own passengers
over a dim interior, behind a little glass tint, and becomes opaque. Run after upscale.py (it calls this).

  py -3.12 tools/texture_pack/train_windows.py [--textures DIR]
"""
import argparse
import os
import sys
from pathlib import Path

import numpy as np
from PIL import Image
from scipy import ndimage

ROOT = Path(__file__).resolve().parents[2]
TRAINS = ("CBD7BFB78140A4B4", "C2FD2BE826DD6218", "463ED3177FE07761")  # SD tram, Detroit people mover, Tokyo train
CROWD = (0.512, 0.0, 1.0, 0.39)  # u0 v0 u1 v1: the passenger art, its alpha > 5 where a person is (train_people.fx)
KEEP = (0.5, 0.0, 1.0, 0.5)      # the passenger block, whose alpha-0 background the people pass still needs
MARK = b"mc3-train-windows-v1"


def crowd_layer(rep, scale_h):
    """The passengers as RGBA, scaled to scale_h pixels tall."""
    h, w = rep.shape[:2]
    u0, v0, u1, v1 = CROWD
    crop = rep[int(v0 * h):int(v1 * h), int(u0 * w):int(u1 * w)].astype(np.float32)
    person = (crop[..., 3] > 5.5).astype(np.float32)
    rgba = np.dstack([crop[..., :3], person * 255])
    img = Image.fromarray(rgba.astype(np.uint8), "RGBA")
    nw = max(1, round(img.width * scale_h / img.height))
    return np.asarray(img.resize((nw, scale_h), Image.LANCZOS)).astype(np.float32) / 255


def fill(rep, src_alpha):
    """rep: HxWx4 uint8 replacement; src_alpha: the console texture's alpha (the exact window texels)."""
    h, w = rep.shape[:2]
    sh, sw = src_alpha.shape
    windows = src_alpha == 0
    k0, k1 = int(KEEP[1] * sh), int(KEEP[3] * sh)
    windows[k0:k1, int(KEEP[0] * sw):int(KEEP[2] * sw)] = False
    labels, count = ndimage.label(windows)
    out = rep.astype(np.float32) / 255
    opaque = out[..., 3] > 0.5
    interior = out[opaque][:, :3].mean(0) * 0.32 if opaque.any() else np.array([0.08, 0.07, 0.06])
    glass = np.array([0.10, 0.13, 0.15])
    sy, sx = h / sh, w / sw
    for n, box in enumerate(ndimage.find_objects(labels), 1):
        if box is None or (box[0].stop - box[0].start) < 2 or (box[1].stop - box[1].start) < 2:
            continue
        y0, y1 = int(box[0].start * sy), int(box[0].stop * sy)
        x0, x1 = int(box[1].start * sx), int(box[1].stop * sx)
        bh, bw = y1 - y0, x1 - x0
        # interior: darker at the floor, a little light from the ceiling
        grad = np.linspace(1.25, 0.7, bh)[:, None, None]
        pane = np.broadcast_to(interior * grad, (bh, bw, 3)).copy()
        people = crowd_layer(rep, max(8, round(bh * 1.15)))
        shift = (n * 37) % people.shape[1]
        cols = (np.arange(bw) + shift) % people.shape[1]
        layer = people[bh // 10: bh // 10 + bh][:, cols]
        if layer.shape[0] < bh:
            layer = np.pad(layer, ((0, bh - layer.shape[0]), (0, 0), (0, 0)))
        a = layer[..., 3:4] * 0.9
        pane = pane * (1 - a) + layer[..., :3] * 0.8 * a
        # glass: a cool tint and a soft reflection band across the top
        refl = np.clip(1 - np.linspace(0, 3, bh), 0, 1)[:, None, None] * 0.12
        pane = pane * 0.82 + glass * 0.18 + refl
        # only where the window is open: soft upscaled edges blend into the frame
        cover = (labels[np.clip((np.arange(y0, y1) / sy).astype(int), 0, sh - 1)][:, np.clip(
            (np.arange(x0, x1) / sx).astype(int), 0, sw - 1)] == n)
        open_ = (1 - out[y0:y1, x0:x1, 3:4]) * cover[..., None]
        out[y0:y1, x0:x1, :3] = out[y0:y1, x0:x1, :3] * (1 - open_) + np.clip(pane, 0, 1) * open_
        out[y0:y1, x0:x1, 3:4] = np.maximum(out[y0:y1, x0:x1, 3:4], cover[..., None])
    return (np.clip(out, 0, 1) * 255 + 0.5).astype(np.uint8)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--textures", type=Path, default=Path(os.environ.get("MC3_NATIVE") or ROOT / "templates" / "game" /
                                                        "out" / "native") / "textures")
    a = ap.parse_args(argv)
    for h in TRAINS:
        src, rep = a.textures / "source" / f"{h}.png", a.textures / "replace" / f"{h}.png"
        if not src.exists() or not rep.exists():
            print(f"  {h}: no source or replacement yet, skipped")
            continue
        with Image.open(rep) as im:
            if im.info.get("mc3") == MARK.decode():
                print(f"  {h}: windows already filled")
                continue
            pixels = np.asarray(im.convert("RGBA"))
        with Image.open(src) as im:
            src_alpha = np.asarray(im.convert("RGBA"))[..., 3]
        from PIL.PngImagePlugin import PngInfo
        info = PngInfo(); info.add_text("mc3", MARK.decode())
        tmp = rep.with_suffix(".tmp.png")
        Image.fromarray(fill(pixels, src_alpha), "RGBA").save(tmp, pnginfo=info)
        os.replace(tmp, rep)
        print(f"  {h}: windows filled with passengers")
    return 0


if __name__ == "__main__":
    sys.exit(main())
