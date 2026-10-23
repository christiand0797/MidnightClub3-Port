"""Material map generation for the MC3 HD texture pack (offline; nothing here touches the game or its renderer).

For a texture named by its runtime content hash (textures/source/<hash>.png, textures/replace/<hash>.png) this module
derives a set of PBR material maps:

  <hash>_n.png   tangent-space normal, RGB8, OpenGL / glTF convention (+X right, +Y up the image, +Z out)
  <hash>_h.png   height, 16-bit grey, 0.5 = the texture's mean surface (integrated from the normal map)
  <hash>_r.png   perceptual roughness, 8-bit grey (0 mirror .. 255 fully rough)
  <hash>_m.png   metalness, 8-bit grey; only written for classes that can hold bare metal
  <hash>_ao.png  ambient occlusion from the height field, 8-bit grey (255 = unoccluded)

Normals come from DeepBump (a small learned colour -> normal U-Net, run with onnxruntime, CPU or DirectML) blended with a
classical band-pass height + Sobel estimate, or from the classical estimate alone when the model is not available.
Roughness and metalness are rules: a per-class prior (MATERIALS) modulated by local image statistics and, for the
world textures, by the artists' own reflection mask that MC3 stores in the DXT5 alpha channel (see docs/PBR_PLAN.md).

Every filter wraps at the edges, because most world textures tile.
"""
from __future__ import annotations

import json
import math
import os
from dataclasses import dataclass, field, asdict
from pathlib import Path

import numpy as np
from PIL import Image
from scipy import ndimage

ROOT = Path(__file__).resolve().parents[2]
TEXTURES = ROOT / "templates" / "game" / "out" / "native" / "textures"
SOURCE = TEXTURES / "source"
REPLACE = TEXTURES / "replace"
PBR = TEXTURES / "pbr"
INDEX = TEXTURES / "source_index.json"
MODEL_DIR = Path(__file__).resolve().parent / "models"
DEEPBUMP = MODEL_DIR / "deepbump256.onnx"
DEEPBUMP_URL = "https://raw.githubusercontent.com/HugoTini/DeepBump/master/deepbump256.onnx"
DEEPBUMP_SHA256 = "3a2ababc5fa652b19040a0f3d639b71826368e52ad805cf70ffb0e7c96a54d67"
PBRIFY_DIR = MODEL_DIR / "pbrify"
PBRIFY_URL = "https://github.com/Kim2091/PBRify_Remix/releases/download/v1.7.2/PBRify_Remix-1.7.2.zip"
PBRIFY_SHA256 = {
    "1x-PBRify_Height.pth": "5b973ecb8bae9d96d14d77b8a8f1d88fb6a8580bcc1bb55d2872811acdc4277d",
    "1x-PBRify_NormalV3.pth": "b0a18270da765f02eaae3c228203bee0677fc28b2854a562515e4aae9c61223b",
    "1x-PBRify_RoughnessV2.pth": "7003c39041af64cdb77d5120bd560a2878538fbb14a07657d8fd12aac5773679",
}

MAPS_VERSION = 2  # bump when the algorithms change: generate.py redoes textures made with an older version


# ----------------------------------------------------------------------------------------------------------------------
# material classes


@dataclass(frozen=True)
class Material:
    rough: float              # base perceptual roughness
    rough_var: float          # how far local texture detail moves it (+-)
    metal: str = "none"       # none | chrome (detect bare bright unsaturated metal per pixel) | full
    normal: float = 1.0       # normal strength (multiplies the classical slope; DeepBump output is rescaled to it)
    deep: float = 0.7         # weight of DeepBump against the classical normal (0 = classical only)
    ao: float = 0.6           # AO strength
    alpha: str = "none"       # none | gloss (alpha = MC3 reflection mask) | cutout (alpha = coverage)
    gloss_rough: float = 0.1  # roughness where the reflection mask is fully on
    invert_height: bool = False


# Priors. Values are deliberately conservative: the runtime multiplies everything by user-facing strengths.
MATERIALS: dict[str, Material] = {
    # ground
    "road_asphalt": Material(0.88, 0.10, normal=1.4, deep=0.6, ao=0.7, alpha="gloss", gloss_rough=0.35),
    "road_marking": Material(0.85, 0.10, normal=1.2, deep=0.6, ao=0.6, alpha="gloss", gloss_rough=0.35),
    "road_concrete": Material(0.85, 0.10, normal=1.2, deep=0.6, ao=0.6, alpha="gloss", gloss_rough=0.40),
    "pavement": Material(0.82, 0.12, normal=1.6, deep=0.7, ao=0.8, alpha="gloss", gloss_rough=0.40),
    "curb": Material(0.80, 0.10, normal=1.2, deep=0.6, ao=0.6, alpha="gloss", gloss_rough=0.40),
    "terrain": Material(0.92, 0.06, normal=1.6, deep=0.7, ao=0.8, alpha="none"),
    "foliage": Material(0.75, 0.10, normal=1.0, deep=0.6, ao=0.7, alpha="cutout"),
    # buildings
    "brick": Material(0.88, 0.08, normal=1.8, deep=0.75, ao=0.9, alpha="gloss", gloss_rough=0.15),
    "concrete": Material(0.86, 0.10, normal=1.3, deep=0.7, ao=0.7, alpha="gloss", gloss_rough=0.15),
    "facade": Material(0.80, 0.12, normal=1.3, deep=0.7, ao=0.8, alpha="gloss", gloss_rough=0.08),
    "storefront": Material(0.65, 0.15, normal=1.0, deep=0.6, ao=0.7, alpha="gloss", gloss_rough=0.08),
    "windows": Material(0.60, 0.15, normal=1.0, deep=0.6, ao=0.7, alpha="gloss", gloss_rough=0.05),
    "windows_night": Material(0.30, 0.10, normal=0.6, deep=0.5, ao=0.3, alpha="none"),
    "metal_painted": Material(0.55, 0.15, normal=1.2, deep=0.7, ao=0.7, alpha="gloss", gloss_rough=0.15),
    "metal_bare": Material(0.40, 0.15, metal="full", normal=1.2, deep=0.7, ao=0.7, alpha="gloss", gloss_rough=0.15),
    "signage": Material(0.55, 0.12, normal=0.7, deep=0.5, ao=0.4, alpha="gloss", gloss_rough=0.10),
    # vehicles
    "car_atlas": Material(0.45, 0.15, metal="chrome", normal=0.8, deep=0.5, ao=0.5),
    "traffic_atlas": Material(0.45, 0.15, metal="chrome", normal=0.8, deep=0.5, ao=0.5),
    "car_light": Material(0.12, 0.06, normal=0.8, deep=0.5, ao=0.4),
    "car_glass": Material(0.06, 0.05, normal=0.5, deep=0.4, ao=0.2),
    "carbon": Material(0.30, 0.08, normal=0.8, deep=0.5, ao=0.4),
    "grill": Material(0.40, 0.12, metal="chrome", normal=1.4, deep=0.6, ao=0.9),
    "exhaust": Material(0.30, 0.12, metal="chrome", normal=1.0, deep=0.6, ao=0.7),
    "rim": Material(0.22, 0.10, metal="chrome", normal=0.9, deep=0.5, ao=0.6, alpha="cutout"),
    "chrome": Material(0.12, 0.06, metal="chrome", normal=0.8, deep=0.5, ao=0.5, alpha="cutout"),
    "brake": Material(0.45, 0.12, metal="chrome", normal=1.0, deep=0.6, ao=0.7, alpha="cutout"),
    "tire": Material(0.85, 0.08, normal=1.6, deep=0.7, ao=0.8),
    "rider": Material(0.70, 0.12, normal=1.0, deep=0.7, ao=0.6),
    # fallbacks
    "generic": Material(0.70, 0.12, normal=1.0, deep=0.6, ao=0.6),
    "ui": Material(0.50, 0.0, normal=0.0, deep=0.0, ao=0.0),
}

METAL_CLASSES = {k for k, m in MATERIALS.items() if m.metal != "none"}


# ----------------------------------------------------------------------------------------------------------------------
# io


def load_index() -> dict:
    with open(INDEX, encoding="utf-8") as f:
        return json.load(f)


def parse_index_entry(v: str) -> dict:
    loc, fmt, size, levels = v.rsplit(" ", 3)
    w, h = (int(x) for x in size.split("x"))
    return {"loc": loc.replace("\\", "/"), "fmt": fmt, "w": w, "h": h, "levels": int(levels[1:])}


def albedo_path(h: str, prefer_hd: bool = True) -> tuple[Path, str]:
    r = REPLACE / f"{h}.png"
    if prefer_hd and r.exists():
        return r, "replace"
    return SOURCE / f"{h}.png", "source"


def load_working(h: str, entry: dict, source: str = "source", work_scale: float = 4.0, max_size: int = 1024):
    """The image maps are computed from, at the working resolution. Returns (rgba, input used, texels scale).

    source="source" (default): the console's own texture (textures/source), Lanczos-enlarged by work_scale (4 = the
    HD pack's size) and capped at max_size. The maps then follow the original artwork, not the grain or invented detail
    of whichever upscaler made textures/replace (it is regenerated from time to time), and still line up with it: maps
    and albedo share UVs, so their resolutions need not match. source="replace": the HD pack image when present."""
    path, used = albedo_path(h, prefer_hd=(source == "replace"))
    rgba = load_rgba(path)
    H, W = rgba.shape[:2]
    f = work_scale if used == "source" else 1.0
    if max(H, W) * f > max_size:
        f = max_size / max(H, W)
    if f != 1.0:
        rgba = np.clip(resize(rgba, max(1, round(W * f)), max(1, round(H * f))), 0, 1)
    return rgba, used, rgba.shape[1] / entry["w"]


def load_rgba(path: Path) -> np.ndarray:
    with Image.open(path) as im:
        return np.asarray(im.convert("RGBA"), dtype=np.float32) / 255.0


def resize(a: np.ndarray, w: int, h: int) -> np.ndarray:
    """Float image (H,W) or (H,W,C) resized with Lanczos per channel."""
    if a.shape[1] == w and a.shape[0] == h:
        return a
    chans = [a] if a.ndim == 2 else [a[..., c] for c in range(a.shape[2])]
    out = [np.asarray(Image.fromarray(c.astype(np.float32), "F").resize((w, h), Image.LANCZOS)) for c in chans]
    return out[0] if a.ndim == 2 else np.stack(out, -1)


def _atomic_save(im: Image.Image, path: Path):
    tmp = path.with_name(path.stem + ".part.png")
    im.save(tmp, optimize=False, compress_level=6)
    os.replace(tmp, path)


def save_grey8(a: np.ndarray, path: Path):
    _atomic_save(Image.fromarray(np.clip(a * 255.0 + 0.5, 0, 255).astype(np.uint8), "L"), path)


def save_grey16(a: np.ndarray, path: Path):
    _atomic_save(Image.fromarray(np.clip(a * 65535.0 + 0.5, 0, 65535).astype(np.uint16), "I;16"), path)


def save_normal(n: np.ndarray, path: Path):
    _atomic_save(Image.fromarray(np.clip((n * 0.5 + 0.5) * 255.0 + 0.5, 0, 255).astype(np.uint8), "RGB"), path)


# ----------------------------------------------------------------------------------------------------------------------
# image statistics


def srgb_to_linear(c):
    return np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)


def luminance(rgb: np.ndarray) -> np.ndarray:
    return rgb[..., 0] * 0.2126 + rgb[..., 1] * 0.7152 + rgb[..., 2] * 0.0722


def saturation(rgb: np.ndarray) -> np.ndarray:
    mx, mn = rgb.max(-1), rgb.min(-1)
    return np.where(mx > 1e-4, (mx - mn) / np.maximum(mx, 1e-4), 0.0)


# edge handling of every filter: "wrap" for tiling textures, "reflect" for atlases (car and traffic textures are
# packed UV islands; wrapping would pull the opposite edge's parts into the border). Set per texture by generate().
_EDGE = ["wrap"]
ATLAS_CLASSES = {"car_atlas", "traffic_atlas", "car_light", "rider", "signage", "storefront", "rim", "brake", "chrome"}


def blur(a: np.ndarray, sigma: float) -> np.ndarray:
    if sigma <= 0:
        return a
    return ndimage.gaussian_filter(a, sigma, mode=_EDGE[0], truncate=3.0)


def local_std(a: np.ndarray, sigma: float) -> np.ndarray:
    m = blur(a, sigma)
    return np.sqrt(np.maximum(blur(a * a, sigma) - m * m, 0.0))


def smoothstep(e0, e1, x):
    t = np.clip((x - e0) / (e1 - e0), 0.0, 1.0)
    return t * t * (3 - 2 * t)


def robust_norm(a: np.ndarray, lo=2.0, hi=98.0, mask=None) -> np.ndarray:
    v = a[mask] if mask is not None and mask.any() else a
    p0, p1 = np.percentile(v, [lo, hi])
    return np.clip((a - p0) / max(p1 - p0, 1e-6), 0.0, 1.0)


# ----------------------------------------------------------------------------------------------------------------------
# height and normals


def classical_height(lum: np.ndarray, scale: float, fine_only: bool = False) -> np.ndarray:
    """Band-pass height from luminance: a sum of difference-of-Gaussian bands, so baked large-scale shading and the
    image's overall brightness do not become slopes, and DXT block noise (removed by a pre-blur) does not become bumps.
    scale: working pixels per original texel (the HD pack is 4x), so the bands sit at the same texel frequencies at
    any resolution. Returns zero-mean, unit-std height."""
    s = max(scale, 1.0)
    base = blur(lum, 0.35 * s)
    sig = [0.5, 1.0, 2.0, 4.0, 8.0, 16.0]
    w = [0.6, 1.0, 1.0, 0.8, 0.5, 0.3]
    if fine_only:
        sig, w = sig[:3], [0.8, 1.0, 0.6]
    levels = [blur(base, x * s) for x in sig]
    h = np.zeros_like(lum)
    prev = base
    for lv, wt in zip(levels, w):
        h += wt * (prev - lv)
        prev = lv
    h -= h.mean()
    return h / (h.std() + 1e-6)


def height_to_normal(h: np.ndarray, strength: float) -> np.ndarray:
    """OpenGL convention normal from a height field whose rows run down the image. Sobel on a wrapped field."""
    gx = ndimage.sobel(h, axis=1, mode=_EDGE[0]) / 8.0
    gy = ndimage.sobel(h, axis=0, mode=_EDGE[0]) / 8.0
    n = np.stack([-gx * strength, gy * strength, np.ones_like(h)], -1)  # +Y up: a rise down the image tilts to -Y
    return n / np.linalg.norm(n, axis=-1, keepdims=True)


def normalize(n: np.ndarray) -> np.ndarray:
    return n / np.maximum(np.linalg.norm(n, axis=-1, keepdims=True), 1e-6)


def blend_normals(a: np.ndarray, b: np.ndarray, wa: float, wb: float) -> np.ndarray:
    """Weighted slope blend (partial-derivative blending): adds the two surfaces' slopes."""
    sa = a[..., :2] / np.maximum(a[..., 2:3], 1e-3)
    sb = b[..., :2] / np.maximum(b[..., 2:3], 1e-3)
    s = sa * wa + sb * wb
    return normalize(np.concatenate([s, np.ones_like(s[..., :1])], -1))


def scale_normal(n: np.ndarray, k: float) -> np.ndarray:
    s = n[..., :2] / np.maximum(n[..., 2:3], 1e-3) * k
    return normalize(np.concatenate([s, np.ones_like(s[..., :1])], -1))


def highpass_normal(n: np.ndarray, sigma: float) -> np.ndarray:
    """Removes slope variation broader than sigma pixels (learned models read large dirt and shading patches as tilt;
    on a tiling texture that broad tilt is never real geometry)."""
    s = n[..., :2] / np.maximum(n[..., 2:3], 1e-3)
    s = s - np.stack([blur(s[..., 0], sigma), blur(s[..., 1], sigma)], -1)
    return normalize(np.concatenate([s, np.ones_like(s[..., :1])], -1))


def integrate_normals(n: np.ndarray) -> np.ndarray:
    """Frankot-Chellappa: the periodic height whose gradient is closest (least squares) to the normal map's slopes.
    Returns zero-mean height in the slope units of the map (one pixel = one unit)."""
    p = -n[..., 0] / np.maximum(n[..., 2], 1e-3)      # dh/dx
    q = n[..., 1] / np.maximum(n[..., 2], 1e-3)       # dh/dy (rows down), OpenGL +Y up
    H, W = p.shape
    wx = np.fft.fftfreq(W) * 2 * np.pi
    wy = np.fft.fftfreq(H) * 2 * np.pi
    WX, WY = np.meshgrid(wx, wy)
    P, Q = np.fft.fft2(p), np.fft.fft2(q)
    den = WX ** 2 + WY ** 2
    den[0, 0] = 1.0
    Z = (-1j * WX * P - 1j * WY * Q) / den
    Z[0, 0] = 0
    h = np.real(np.fft.ifft2(Z))
    return h - h.mean()


class DeepBump:
    """DeepBump colour -> normals (HugoTini/DeepBump, deepbump256.onnx, GPL-3; model only, run locally). Input is the
    grey mean of RGB in 0..1 as 256x256 tiles, output RGB 0..1 normals. Tiles overlap by a quarter and are blended with
    a separable raised-cosine window; the image is wrap-padded first so tiling textures stay seamless."""

    TILE = 256

    def __init__(self, device: str = "cpu"):
        import onnxruntime as ort
        ort.set_default_logger_severity(3)
        providers = ["CPUExecutionProvider"]
        if device == "dml" and "DmlExecutionProvider" in ort.get_available_providers():
            providers = ["DmlExecutionProvider", "CPUExecutionProvider"]
        so = ort.SessionOptions()
        if device == "cpu":
            so.intra_op_num_threads = max(1, (os.cpu_count() or 4) // 2)
        self.session = ort.InferenceSession(str(DEEPBUMP), so, providers=providers)
        self.providers = self.session.get_providers()

    @staticmethod
    def available() -> bool:
        try:
            import onnxruntime  # noqa: F401
        except ImportError:
            return False
        return DEEPBUMP.exists()

    def __call__(self, grey: np.ndarray) -> np.ndarray:
        T = self.TILE
        stride = T * 3 // 4
        H, W = grey.shape
        pad = T // 4
        # pad so that (padded - T) is a multiple of stride, wrapping the texture's own edges
        ph = max(T, H + 2 * pad)
        pw = max(T, W + 2 * pad)
        ph += (-(ph - T)) % stride
        pw += (-(pw - T)) % stride
        top, left = (ph - H) // 2, (pw - W) // 2
        img = np.pad(grey, ((top, ph - H - top), (left, pw - W - left)),
                     mode="wrap" if _EDGE[0] == "wrap" else "symmetric").astype(np.float32)
        win1 = np.sin(np.linspace(0.5, T - 0.5, T) / T * np.pi) ** 2 + 1e-3
        win = np.outer(win1, win1).astype(np.float32)
        acc = np.zeros((3, ph, pw), np.float32)
        wsum = np.zeros((ph, pw), np.float32)
        tiles, pos = [], []
        for y in range(0, ph - T + 1, stride):
            for x in range(0, pw - T + 1, stride):
                tiles.append(img[y:y + T, x:x + T])
                pos.append((y, x))
        for (y, x), t in zip(pos, tiles):
            out = self.session.run(None, {"input": t[None, None]})[0][0]
            acc[:, y:y + T, x:x + T] += out * win
            wsum[y:y + T, x:x + T] += win
        out = acc / wsum
        out = out[:, top:top + H, left:left + W]
        n = np.transpose(out, (1, 2, 0)) * 2.0 - 1.0
        return normalize(n)


class PBRify:
    """PBRify_Remix (Kim2091, CC0, trained on ambientCG/PolyHaven materials for RTX Remix): three 1x SPAN networks,
    albedo RGB 0..1 -> normal (DirectX convention: green flipped here to OpenGL), roughness, height. Loaded through
    spandrel from .pth state dicts with torch.load(weights_only=True) (no pickled code runs). CPU torch is enough:
    about 1.2 s per 512x512 per network on this machine.
    Measured on the starter set: its normals agree with the classical estimate (corr ~0.8), but its roughness is not
    reliable in absolute terms on MC3 textures (a black rubber tyre comes out 0.2), so only its variation is used."""

    FILES = {"normal": "1x-PBRify_NormalV3.pth", "rough": "1x-PBRify_RoughnessV2.pth", "height": "1x-PBRify_Height.pth"}

    def __init__(self, threads: int = 0):
        import torch
        import spandrel
        self.torch = torch
        if threads:
            torch.set_num_threads(threads)
        self.nets = {}
        for k, f in self.FILES.items():
            sd = torch.load(str(PBRIFY_DIR / f), map_location="cpu", weights_only=True)
            for key in ("params_ema", "params"):
                if isinstance(sd, dict) and key in sd:
                    sd = sd[key]
            self.nets[k] = spandrel.ModelLoader().load_from_state_dict(sd).model.eval()

    @staticmethod
    def available() -> bool:
        try:
            import torch  # noqa: F401
            import spandrel  # noqa: F401
        except ImportError:
            return False
        return all((PBRIFY_DIR / f).exists() for f in PBRify.FILES.values())

    def __call__(self, rgb: np.ndarray, which=("normal", "rough")) -> dict:
        pad = 16
        H, W = rgb.shape[:2]
        x = np.pad(rgb, ((pad, pad), (pad, pad), (0, 0)), mode="wrap" if _EDGE[0] == "wrap" else "symmetric")
        x = x.transpose(2, 0, 1)
        x = self.torch.from_numpy(np.ascontiguousarray(x, dtype=np.float32))[None]
        out = {}
        with self.torch.no_grad():
            for k in which:
                y = self.nets[k](x)[0].numpy().transpose(1, 2, 0)[pad:pad + H, pad:pad + W]
                if k == "normal":
                    n = y * 2 - 1
                    n[..., 1] = -n[..., 1]  # DirectX -> OpenGL
                    out[k] = normalize(n)
                else:
                    out[k] = np.clip(y.mean(-1), 0, 1)
        return out


# ----------------------------------------------------------------------------------------------------------------------
# the maps


@dataclass
class Result:
    normal: np.ndarray
    height: np.ndarray
    rough: np.ndarray
    metal: np.ndarray | None
    ao: np.ndarray
    info: dict = field(default_factory=dict)


METHODS = ("ensemble", "hybrid", "pbrify", "deep", "classic")


def _slope(n):
    return np.median(np.linalg.norm(n[..., :2] / np.maximum(n[..., 2:3], 1e-3), axis=-1)) + 1e-4


def generate(rgba: np.ndarray, cls: str, scale: float, models: dict | None = None, method: str = "ensemble") -> Result:
    _EDGE[0] = "reflect" if cls in ATLAS_CLASSES else "wrap"
    try:
        return _generate(rgba, cls, scale, models, method)
    finally:
        _EDGE[0] = "wrap"


def _generate(rgba, cls, scale, models, method) -> Result:
    """rgba: float (H,W,4) sRGB 0..1 at the working resolution; scale: working pixels per original texel.
    models: {"deep": DeepBump, "pbrify": PBRify}, either may be missing. Methods:
      classic   band-pass height + Sobel
      deep      DeepBump only;  pbrify: PBRify only
      hybrid    DeepBump, plus the classical estimate's fine bands for texel-scale detail
      ensemble  DeepBump and PBRify slopes averaged, plus the classical fine bands (default)
    Learned normals are rescaled to the classical estimate's median slope, so the class strength governs all methods.
    A method whose model is missing falls back to the next simpler one (recorded in info["method"])."""
    models = models or {}
    deep, pbr = models.get("deep"), models.get("pbrify")
    if method == "ensemble" and not (deep and pbr):
        method = "hybrid" if deep else ("pbrify" if pbr else "classic")
    if method in ("hybrid", "deep") and not deep:
        method = "pbrify" if pbr else "classic"
    if method == "pbrify" and not pbr:
        method = "classic"
    m = MATERIALS.get(cls, MATERIALS["generic"])
    rgb, alpha = rgba[..., :3], rgba[..., 3]
    lum = luminance(rgb)
    sat = saturation(rgb)
    info = {"class": cls, "method": method, "edges": _EDGE[0]}

    cover = np.ones_like(lum, dtype=bool)
    if m.alpha == "cutout":
        cover = alpha > 0.5

    # --- height / normal
    h_cl = classical_height(lum, scale)
    if m.invert_height:
        h_cl = -h_cl
    # slope strength: unit-std height over ~2 texels of rise per texel at the chosen strength
    k = m.normal * 0.9 / max(scale, 1.0)
    n_cl = height_to_normal(h_cl, k * 2.0)
    s_cl = _slope(n_cl)
    learned = []
    pb_out = {}
    if m.normal > 0 and m.deep > 0:
        if method in ("hybrid", "deep", "ensemble"):
            n_dp = highpass_normal(deep(np.mean(rgb, -1)), 16.0 * scale)
            learned.append(scale_normal(n_dp, s_cl / _slope(n_dp)))
        if method in ("pbrify", "ensemble"):
            pb_out = pbr(rgb, ("normal", "rough"))
            n_pb = highpass_normal(pb_out["normal"], 16.0 * scale)
            learned.append(scale_normal(n_pb, s_cl / _slope(n_pb)))
    if learned:
        if m.invert_height:
            for x in learned:
                x[..., :2] *= -1
        # ensemble: PBRify leads (clean, architecture-aware on the starter set); DeepBump adds its broader shapes
        n_l = learned[0] if len(learned) == 1 else blend_normals(learned[0], learned[1], 0.3, 0.7)
        if method in ("deep", "pbrify"):
            n = n_l
        else:
            n_fine = height_to_normal(classical_height(lum, scale, fine_only=True), k * 2.0)
            n = blend_normals(n_l, n_fine, m.deep, 1.0 - m.deep)
        if len(learned) == 2:
            info["model_agreement"] = round(float(np.corrcoef(learned[0][..., :2].ravel(), learned[1][..., :2].ravel())[0, 1]), 3)
    else:
        n = n_cl
    if m.normal <= 0:
        n = np.zeros_like(n_cl)
        n[..., 2] = 1
    if not cover.all():
        flat = np.zeros_like(n)
        flat[..., 2] = 1
        soft = blur(cover.astype(np.float32), 0.75 * scale)[..., None]
        n = normalize(n * soft + flat * (1 - soft))

    # height consistent with the final normals
    h = integrate_normals(n)
    # integration lets small slope biases pile up into broad hills; keep relief below ~24 texels across
    h = h - blur(h, 24.0 * scale)
    hs = h.std() + 1e-6
    height = np.clip(0.5 + h / hs * 0.12, 0, 1)

    # --- ambient occlusion: how far each point sits below its neighbourhood, at a few radii (in original texels)
    ao = np.ones_like(lum)
    if m.ao > 0:
        hn = h / hs
        cav = np.zeros_like(lum)
        for r, w in ((1.0, 0.45), (3.0, 0.35), (8.0, 0.2)):
            cav += w * np.maximum(blur(hn, r * scale) - hn, 0)
        ao = np.clip(1.0 - m.ao * 0.35 * cav, 0.25, 1.0)
        ao = np.where(cover, ao, 1.0)

    # --- metalness
    metal = None
    if m.metal == "full":
        metal = np.where(cover, 1.0, 0.0).astype(np.float32)
    elif m.metal == "chrome":
        # bare metal in these atlases is bright, grey and contrasty (chrome trim, rims, exhaust tips); paint, rubber,
        # lens plastics and printed logos are saturated, dark or flat
        contrast = robust_norm(local_std(lum, 1.5 * scale))
        metal = smoothstep(0.18, 0.40, lum) * (1 - smoothstep(0.10, 0.28, sat)) * smoothstep(0.05, 0.35, contrast)
        if cls in ("car_atlas", "traffic_atlas"):
            # atlases hold large near-white areas (the body base the paint colour multiplies, printed decals): their
            # edges are contrasty and unsaturated too, but they are paint, not chrome
            metal = metal * (1 - smoothstep(0.78, 0.92, blur(lum, 2.0 * scale)))
        if cls in ("rim", "chrome"):
            metal = np.maximum(metal, smoothstep(0.12, 0.30, lum) * (1 - smoothstep(0.15, 0.35, sat)))
        metal = blur(metal, 0.5 * scale)
        metal = np.where(cover, metal, 0.0)
        info["metal_mean"] = round(float(metal.mean()), 4)

    # --- roughness
    detail = robust_norm(local_std(lum, 1.0 * scale), mask=cover)            # busy, grainy -> rougher
    cavity = robust_norm(-blur(h / hs, 1.0 * scale), mask=cover)              # pits and grooves -> rougher
    mod = (detail - 0.5) * 1.2 + (cavity - 0.5) * 0.6
    if "rough" in pb_out:
        # the network's roughness variation (not its level), in units of its own spread, averaged with the rules
        rp = pb_out["rough"]
        sel = rp[cover] if cover.any() else rp
        rz = np.clip((rp - np.median(sel)) / (sel.std() + 0.02), -2, 2) * 0.5
        mod = 0.5 * mod + 0.5 * rz
    rough = m.rough + m.rough_var * mod
    if metal is not None:
        rough = rough * (1 - 0.45 * metal)                                     # polished where metal
    if m.alpha == "gloss":
        # MC3 adds its reflection texture times this alpha (combiner: albedo*light + refl*tfactor*t0.a): the artists'
        # reflectivity mask - but only in the draws that use that two-stage combiner, which offline we cannot see.
        # Applied only when the alpha looks like a mask: structured (not one flat value) and covering a minority of the
        # texture (window panes, wet-asphalt speckle, grates). Dense or uniform alpha is left alone until the runtime
        # material census (docs/PBR_PLAN.md) says which combiner draws the texture.
        on = float((alpha > 0.5).mean())
        if alpha.std() > 0.06 and on < 0.7 and alpha.min() < 0.5:
            g = blur(alpha, 0.5 * scale)
            rough = rough + (m.gloss_rough - rough) * smoothstep(0.05, 0.9, g)
            info["alpha_role"] = "gloss"
            info["gloss_mask_mean"] = round(float(alpha.mean()), 4)
        elif alpha.min() < 0.98:
            info["alpha_role"] = "ignored (dense or uniform)"
    rough = np.where(cover, np.clip(rough, 0.03, 1.0), 1.0)

    info.update({
        "rough_mean": round(float(rough[cover].mean()) if cover.any() else 1.0, 4),
        "ao_mean": round(float(ao.mean()), 4),
        "slope_mean": round(float(np.mean(np.linalg.norm(n[..., :2], axis=-1))), 4),
    })
    return Result(n.astype(np.float32), height.astype(np.float32), rough.astype(np.float32),
                  None if metal is None else metal.astype(np.float32), ao.astype(np.float32), info)


OVERRIDES = Path(__file__).resolve().parent / "overrides"


def apply_overrides(h: str, res: Result) -> list[str]:
    """Hand-made maps win: tools/pbr/overrides/<hash>_<m|r|ao>.png (grey, any size; resized to the maps) replace the
    generated ones. For what rules cannot know, e.g. which white strokes of a car atlas are chrome trim and which are
    paint. Returns the maps overridden."""
    used = []
    H, W = res.rough.shape
    for key, attr in (("m", "metal"), ("r", "rough"), ("ao", "ao")):
        p = OVERRIDES / f"{h}_{key}.png"
        if p.exists():
            with Image.open(p) as im:
                a = np.asarray(im.convert("L"), dtype=np.float32) / 255.0
            setattr(res, attr, np.clip(resize(a, W, H), 0, 1))
            used.append(key)
    return used


def write(h: str, res: Result, out_dir: Path = PBR) -> list[str]:
    out_dir.mkdir(parents=True, exist_ok=True)
    written = []
    save_normal(res.normal, out_dir / f"{h}_n.png"); written.append("n")
    save_grey16(res.height, out_dir / f"{h}_h.png"); written.append("h")
    save_grey8(res.rough, out_dir / f"{h}_r.png"); written.append("r")
    save_grey8(res.ao, out_dir / f"{h}_ao.png"); written.append("ao")
    if res.metal is not None:
        save_grey8(res.metal, out_dir / f"{h}_m.png"); written.append("m")
    else:
        stale = out_dir / f"{h}_m.png"
        if stale.exists():
            stale.unlink()
    return written


# ----------------------------------------------------------------------------------------------------------------------
# preview shading (contact sheets): GGX under one directional light plus a constant ambient, linear space


def shade_preview(rgb: np.ndarray, n: np.ndarray, rough: np.ndarray, metal: np.ndarray | None, ao: np.ndarray,
                  light=(-0.55, 0.55, 0.63), view=(0.0, 0.0, 1.0)) -> np.ndarray:
    base = srgb_to_linear(rgb)
    mtl = np.zeros_like(rough) if metal is None else metal
    L = np.array(light, np.float32); L /= np.linalg.norm(L)
    V = np.array(view, np.float32); V /= np.linalg.norm(V)
    Hv = (L + V); Hv /= np.linalg.norm(Hv)
    nl = np.clip(n @ L, 0, 1)
    nv = np.clip(n @ V, 1e-3, 1)
    nh = np.clip(n @ Hv, 0, 1)
    vh = float(np.clip(V @ Hv, 0, 1))
    a = np.maximum(rough, 0.03) ** 2
    a2 = a * a
    D = a2 / (np.pi * (nh * nh * (a2 - 1) + 1) ** 2)
    kk = (rough + 1) ** 2 / 8
    G = (nl / (nl * (1 - kk) + kk)) * (nv / (nv * (1 - kk) + kk))
    F0 = 0.04 * (1 - mtl[..., None]) + base * mtl[..., None]
    F = F0 + (1 - F0) * (1 - vh) ** 5
    spec = D[..., None] * G[..., None] * F / np.maximum(4 * nl * nv, 1e-3)[..., None]
    diff = base * (1 - mtl[..., None]) / np.pi
    radiance = 3.0
    col = (diff + spec) * nl[..., None] * radiance + base * 0.18 * ao[..., None] * (1 - 0.7 * mtl[..., None])
    col += F0 * 0.15 * ao[..., None]  # a flat grey environment for metals
    col = col / (1 + col)  # Reinhard
    return np.where(col <= 0.0031308, col * 12.92, 1.055 * np.power(np.maximum(col, 0), 1 / 2.4) - 0.055)


def material_dict() -> dict:
    return {k: asdict(v) for k, v in MATERIALS.items()}
