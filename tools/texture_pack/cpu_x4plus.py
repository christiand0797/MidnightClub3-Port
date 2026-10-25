"""Bounded, CPU-only x4plus inference for original-derived, isolated loading-art candidates.

Architecture follows official BasicSR RRDBNet (x4, 64 features, 23 RRDBs, 32 growth channels):
https://github.com/XPixelGroup/BasicSR/blob/master/basicsr/archs/rrdbnet_arch.py
Tiling follows the padded crop/stitch convention of the official Real-ESRGAN helper:
https://github.com/xinntao/Real-ESRGAN/blob/master/realesrgan/utils.py
This backend has its own PyTorch/model provenance; it makes no claim of NCNN output equivalence.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import tempfile
import time

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[2]
DEFAULT_WEIGHTS = ROOT/"upscale_tools/realesrgan-ncnn-vulkan-v0.2.0-windows/RealESRGAN_x4plus.pth"
ARCHITECTURE = {"name": "RRDBNet", "scale": 4, "num_in_ch": 3, "num_out_ch": 3,
                "num_feat": 64, "num_block": 23, "num_grow_ch": 32,
                "residual_scale": 0.2, "upsampling": "nearest-2x-twice", "activation": "LeakyReLU-0.2"}


def digest(path: Path) -> str:
    h = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024*1024), b""):
            h.update(chunk)
    return h.hexdigest()


def configure_cpu(torch, threads: int = 2) -> None:
    if not 1 <= threads <= 2:
        raise ValueError("CPU inference is limited to one or two threads")
    torch.set_num_threads(threads)
    if torch.get_num_interop_threads() > threads:
        try:
            torch.set_num_interop_threads(threads)
        except RuntimeError as error:
            raise RuntimeError("CPU inter-op thread limit could not be applied; use a fresh Python process") from error
    if torch.get_num_threads() > 2 or torch.get_num_interop_threads() > 2:
        raise RuntimeError("PyTorch CPU thread limits were not applied")


def build_rrdbnet(torch):
    """Exact state-key/shape-compatible official x4 architecture; all parameters must be replaced strictly."""
    nn, functional = torch.nn, torch.nn.functional

    class ResidualDenseBlock(nn.Module):
        def __init__(self):
            super().__init__()
            for index in range(1, 6):
                self.add_module(f"conv{index}", nn.Conv2d(64+32*(index-1), 32 if index<5 else 64, 3, 1, 1))
            self.lrelu = nn.LeakyReLU(0.2, inplace=True)

        def forward(self, value):
            features = [value]
            for index in range(1, 5):
                features.append(self.lrelu(getattr(self, f"conv{index}")(torch.cat(features, 1))))
            return value + getattr(self, "conv5")(torch.cat(features, 1))*0.2

    class RRDB(nn.Module):
        def __init__(self):
            super().__init__()
            self.rdb1, self.rdb2, self.rdb3 = (ResidualDenseBlock() for _ in range(3))

        def forward(self, value):
            return value + self.rdb3(self.rdb2(self.rdb1(value)))*0.2

    class RRDBNet(nn.Module):
        def __init__(self):
            super().__init__()
            self.conv_first = nn.Conv2d(3, 64, 3, 1, 1)
            self.body = nn.Sequential(*(RRDB() for _ in range(23)))
            for name in ("conv_body", "conv_up1", "conv_up2", "conv_hr"):
                self.add_module(name, nn.Conv2d(64, 64, 3, 1, 1))
            self.conv_last, self.lrelu = nn.Conv2d(64, 3, 3, 1, 1), nn.LeakyReLU(0.2, inplace=True)

        def forward(self, value):
            initial = self.conv_first(value)
            features = initial + self.conv_body(self.body(initial))
            for conv in (self.conv_up1, self.conv_up2):
                features = self.lrelu(conv(functional.interpolate(features, scale_factor=2, mode="nearest")))
            return self.conv_last(self.lrelu(self.conv_hr(features)))

    return RRDBNet()


def validate_state(torch, model, checkpoint) -> tuple[dict, str]:
    if not isinstance(checkpoint, dict):
        raise ValueError("x4plus checkpoint must contain a parameter dictionary")
    key = "params_ema" if "params_ema" in checkpoint else "params"
    state = checkpoint.get(key)
    if not isinstance(state, dict):
        raise ValueError("x4plus checkpoint lacks params_ema/params")
    expected = model.state_dict()
    if set(state) != set(expected):
        raise ValueError(f"x4plus state keys differ: missing={sorted(set(expected)-set(state))[:3]}, "
                         f"unexpected={sorted(set(state)-set(expected))[:3]}")
    for name, tensor in state.items():
        if (not isinstance(tensor, torch.Tensor) or tensor.device.type != "cpu" or tensor.dtype != torch.float32
                or tensor.shape != expected[name].shape or not bool(torch.isfinite(tensor).all())):
            raise ValueError(f"invalid x4plus tensor type/device/shape/values: {name}")
    return state, key


class CpuX4Plus:
    def __init__(self, weights_path=DEFAULT_WEIGHTS, *, threads=2):
        try:
            import torch
        except ImportError as error:
            raise RuntimeError("CPU x4plus requires PyTorch; use the configured Python3.12 runtime") from error
        configure_cpu(torch, threads)
        self.torch, self.weights_path = torch, Path(weights_path).resolve()
        if not self.weights_path.is_file():
            raise ValueError("x4plus weights are missing")
        model_hash = digest(self.weights_path)
        # Explicit CPU mapping and weights-only unpickling; never ask CUDA/Vulkan to select a device.
        checkpoint = torch.load(self.weights_path, map_location="cpu", weights_only=True)
        self.model = build_rrdbnet(torch).to(device="cpu", dtype=torch.float32)
        state, key = validate_state(torch, self.model, checkpoint)
        self.model.load_state_dict(state, strict=True)
        self.model.eval().requires_grad_(False)
        if digest(self.weights_path) != model_hash:
            raise ValueError("x4plus weights changed while loading")
        self.metadata = {"backend": "pytorch-cpu-x4plus-v1", "device": "cpu", "precision": "float32",
                         "architecture": dict(ARCHITECTURE), "torch_version": str(torch.__version__),
                         "model_sha256": model_hash, "model_path": str(self.weights_path),
                         "state_key": key, "validated_state_keys": len(state),
                         "weight_key_validation": {"strict": True, "tensor_keys": len(state), "dtype": "float32", "device": "cpu"},
                         "parameters": sum(t.numel() for t in self.model.parameters()),
                         "intra_op_threads": torch.get_num_threads(), "inter_op_threads": torch.get_num_interop_threads(),
                         "worker_sha256": digest(Path(__file__)), "module_sha256": digest(Path(__file__)),
                         "module_path": str(Path(__file__).resolve()),
                         "ncnn_equivalence_claimed": False}

    def upscale_rgb(self, rgb, *, tile=64, tile_pad=10, max_pixels=1_048_576, stop_file=None, progress=None):
        if not isinstance(rgb, np.ndarray) or rgb.dtype != np.uint8 or rgb.ndim != 3 or rgb.shape[2] != 3:
            raise ValueError("CPU x4plus input must be an HWC uint8 RGB array")
        height, width = rgb.shape[:2]
        if not 1 <= max_pixels <= 1_048_576 or min(height, width) < 1 or height*width > max_pixels:
            raise ValueError("CPU candidate source exceeds the configured pixel bound")
        if not 8 <= tile <= 96 or not 0 <= tile_pad <= 32:
            raise ValueError("CPU tile must be8..96 and tile padding0..32")
        tiles_x, tiles_y = math.ceil(width/tile), math.ceil(height/tile)
        if tiles_x*tiles_y > 1024:
            raise ValueError("CPU candidate exceeds the1024-tile bound")
        torch = self.torch
        configure_cpu(torch, self.metadata["intra_op_threads"])
        output = np.empty((height*4, width*4, 3), dtype=np.uint8)
        with torch.inference_mode():
            for y in range(tiles_y):
                for x in range(tiles_x):
                    if stop_file and Path(stop_file).exists():
                        raise InterruptedError("CPU candidate stopped at a tile boundary")
                    x0, y0, x1, y1 = x*tile, y*tile, min((x+1)*tile,width), min((y+1)*tile,height)
                    px0, py0 = max(x0-tile_pad,0), max(y0-tile_pad,0)
                    px1, py1 = min(x1+tile_pad,width), min(y1+tile_pad,height)
                    value = torch.from_numpy(np.ascontiguousarray(rgb[py0:py1,px0:px1].transpose(2,0,1)))
                    value = value.to(device="cpu", dtype=torch.float32).unsqueeze(0)/255.0
                    prediction = self.model(value)
                    expected = (1,3,(py1-py0)*4,(px1-px0)*4)
                    if tuple(prediction.shape) != expected or prediction.device.type != "cpu" or not bool(torch.isfinite(prediction).all()):
                        raise ValueError("CPU model returned invalid dimensions/device/nonfinite output")
                    cropped = prediction[0,:,4*(y0-py0):4*(y1-py0),4*(x0-px0):4*(x1-px0)]
                    pixels = cropped.clamp(0,1).mul(255).round().to(dtype=torch.uint8).permute(1,2,0).numpy()
                    output[y0*4:y1*4,x0*4:x1*4] = pixels
                    if progress:
                        progress(y*tiles_x+x+1, tiles_x*tiles_y)
        return output


def validate_paths(source_path, output_path, original_root):
    source, destination, original_root = Path(source_path).resolve(), Path(output_path).resolve(), Path(original_root).resolve()
    if not source.is_file() or source.suffix.lower() != ".png" or original_root not in source.parents:
        raise ValueError("input must be a PNG under the pinned original-derived source root")
    active = (ROOT/"templates/game/out/native/textures/replace").resolve()
    if source == active or active in source.parents:
        raise ValueError("an existing active upscale cannot be used as original input")
    local = (ROOT/".local").resolve()
    if local not in destination.parents or destination == source or original_root in destination.parents:
        raise ValueError("output must be an isolated .local candidate outside the source root")
    if destination.suffix.lower() != ".png" or destination.exists():
        raise ValueError("candidate output must be a new PNG; existing outputs are preserved")
    return source, destination


def upscale_original(source_path, output_path, weights_path=DEFAULT_WEIGHTS, *, original_root,
                     expected_source_sha256, tile=64, tile_pad=10, engine=None, max_pixels=1_048_576,
                     stop_file=None, progress=None):
    source, destination = validate_paths(source_path, output_path, original_root)
    if not isinstance(expected_source_sha256, str) or len(expected_source_sha256) != 64 or digest(source) != expected_source_sha256:
        raise ValueError("original-derived input hash does not match its pinned plan")
    with Image.open(source) as image:
        image.load()
        if image.mode not in ("RGB","RGBA","L","LA","P"):
            raise ValueError("unsupported original PNG mode")
        if image.width*image.height > max_pixels:
            raise ValueError("CPU candidate source exceeds the configured pixel bound")
        rgb = np.asarray(image.convert("RGB"))
        alpha = image.convert("RGBA").getchannel("A") if image.mode in ("RGBA","LA") or "transparency" in image.info else None
    if engine is None:
        engine = CpuX4Plus(weights_path)
    elif engine.metadata.get("model_sha256") != digest(Path(weights_path)):
        raise ValueError("reused CPU engine does not match the requested weights")
    if (engine.metadata.get("device") != "cpu" or engine.metadata.get("intra_op_threads", 99) > 2
            or engine.metadata.get("inter_op_threads", 99) > 2
            or engine.metadata.get("module_sha256") != digest(Path(__file__))):
        raise ValueError("CPU engine device/thread/module provenance does not match")
    started = time.monotonic()
    pixels = engine.upscale_rgb(rgb, tile=tile, tile_pad=tile_pad, max_pixels=max_pixels, stop_file=stop_file, progress=progress)
    result = Image.fromarray(pixels)
    if alpha is not None:
        result.putalpha(alpha.resize(result.size, Image.Resampling.LANCZOS))
    if result.size != (rgb.shape[1]*4,rgb.shape[0]*4):
        raise ValueError("CPU candidate dimensions are not exact4x")
    if digest(source) != expected_source_sha256:
        raise ValueError("original-derived input changed during inference")
    if (digest(Path(weights_path)) != engine.metadata["model_sha256"]
            or digest(Path(__file__)) != engine.metadata["module_sha256"]):
        raise ValueError("CPU model/module changed during inference")
    destination.parent.mkdir(parents=True,exist_ok=True)
    fd, name = tempfile.mkstemp(prefix=".cpu-x4plus-",suffix=".png",dir=destination.parent)
    os.close(fd)
    temporary = Path(name)
    try:
        result.save(temporary,format="PNG")
        with Image.open(temporary) as verified:
            verified.load()
            if verified.size != result.size or not np.array_equal(np.asarray(verified),np.asarray(result)):
                raise ValueError("CPU candidate PNG failed its decode/pixel audit")
        # Same-directory hard-link publication is atomic and cannot replace a concurrently created output.
        os.link(temporary,destination)
    finally:
        temporary.unlink(missing_ok=True)
    return {**engine.metadata, "source": str(source), "source_sha256": expected_source_sha256,
            "output": str(destination), "output_sha256": digest(destination), "source_size": [rgb.shape[1],rgb.shape[0]],
            "output_size": list(result.size), "tile": tile, "tile_pad": tile_pad,
            "config": {"scale": 4, "tile": tile, "tile_pad": tile_pad, "max_source_pixels": max_pixels,
                       "intra_op_threads": engine.metadata["intra_op_threads"], "inter_op_threads": engine.metadata["inter_op_threads"]},
            "outer_padding": "as supplied by pinned original-derived plan", "alpha": "original-Lanczos" if alpha else "opaque-RGB",
            "elapsed_seconds": time.monotonic()-started, "active_replacements_modified": False}


def benchmark(weights_path=DEFAULT_WEIGHTS):
    engine = CpuX4Plus(weights_path)
    rgb = np.random.default_rng(20261024).integers(0,256,(16,16,3),dtype=np.uint8)
    started = time.monotonic()
    whole = engine.upscale_rgb(rgb,tile=32,tile_pad=16)
    whole_seconds = time.monotonic()-started
    started = time.monotonic()
    tiled = engine.upscale_rgb(rgb,tile=8,tile_pad=16)
    tiled_seconds = time.monotonic()-started
    if not np.array_equal(whole,tiled):
        raise ValueError("whole/tiled synthetic inference differs despite every tile receiving the complete input")
    return {**engine.metadata,"synthetic_source_size": [16,16],"output_size": [64,64],
            "whole_seconds": whole_seconds,"tiled_seconds": tiled_seconds,"tile_layout_pixel_identical": True,
            "output_pixels_sha256": hashlib.sha256(whole.tobytes()).hexdigest(),"game_assets_processed": 0}


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--weights",type=Path,default=DEFAULT_WEIGHTS)
    parser.add_argument("--benchmark",action="store_true")
    parser.add_argument("--input",type=Path)
    parser.add_argument("--output",type=Path)
    parser.add_argument("--original-root",type=Path)
    parser.add_argument("--source-sha256")
    parser.add_argument("--tile",type=int,default=64)
    parser.add_argument("--tile-pad",type=int,default=10)
    args=parser.parse_args()
    if args.benchmark:
        result=benchmark(args.weights)
    else:
        if not all((args.input,args.output,args.original_root,args.source_sha256)):
            parser.error("candidate inference requires input/output/original-root/source-sha256")
        result=upscale_original(args.input,args.output,args.weights,original_root=args.original_root,
                                expected_source_sha256=args.source_sha256,tile=args.tile,tile_pad=args.tile_pad)
    print(json.dumps(result,indent=2,allow_nan=False))
    return 0


if __name__=="__main__":raise SystemExit(main())
