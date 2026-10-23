#!/usr/bin/env python3
"""
Complete Texture Upscaling Pipeline for Midnight Club 3
Extracts XPR textures, upscales with Real-ESRGAN, repacks to XPR
"""

import os
import sys
import subprocess
import argparse
from pathlib import Path

# Paths
EXTRACT_XPR = Path(__file__).parent / "extract_xpr.py"
REPACK_XPR = Path(__file__).parent / "repack_xpr.py"
ESRGAN = Path(__file__).parent / "upscale_tools" / "realesrgan-ncnn-vulkan-v0.2.0-windows" / "realesrgan-ncnn-vulkan.exe"
MODEL_DIR = Path(__file__).parent / "upscale_tools" / "realesrgan-ncnn-vulkan-v0.2.0-windows" / "models"

ASSET_ROOT = Path(__file__).parent / "templates" / "game" / "assets"
EXTRACTED_ROOT = Path(__file__).parent / "extracted"
REPACKED_ROOT = Path(__file__).parent / "repacked"
UPSCALED_ROOT = Path(__file__).parent / "upscaled"

# XPR files to process
# Scale: 2x for 3D textures (Xbox GPU max 2048), 4x for UI only
XPR_FILES = {
    "media/dlcustom.xpr": {"scale": 2, "model": "realesrgan-x4plus"},  # UI backgrounds
    "media/dlres.xpr": {"scale": 2, "model": "realesrgan-x4plus"},      # Downloadable resources
    # "resources/vehicle/rim.xbpf": {"scale": 2, "model": "realesrgan-x4plus"},  # Rim textures (XBPF)
    # "resources/vehicle/decal.xbpf": {"scale": 2, "model": "realesrgan-x4plus"}, # Decals (XBPF)
}


def extract_xpr(xpr_rel_path):
    """Extract XPR file to PNG"""
    xpr_path = ASSET_ROOT / xpr_rel_path
    name = Path(xpr_rel_path).stem
    out_dir = EXTRACTED_ROOT / name
    
    print(f"Extracting {xpr_rel_path}...")
    result = subprocess.run([
        sys.executable, str(EXTRACT_XPR), str(xpr_path), str(out_dir)
    ], capture_output=True, text=True)
    
    if result.returncode != 0:
        print(f"Error: {result.stderr}")
        return False
    
    print(result.stdout)
    return True


def upscale_textures(xpr_rel_path, scale, model):
    """Upscale extracted textures"""
    name = Path(xpr_rel_path).stem
    in_dir = EXTRACTED_ROOT / name
    out_dir = UPSCALED_ROOT / f"{name}_{scale}x"
    
    if not in_dir.exists():
        print(f"Input directory not found: {in_dir}")
        return False
    
    out_dir.mkdir(parents=True, exist_ok=True)
    
    png_files = list(in_dir.glob("*.png"))
    print(f"Upscaling {len(png_files)} textures from {in_dir} ({scale}x)...")
    
    for png_file in png_files:
        out_file = out_dir / f"{png_file.stem}_{scale}x.png"
        print(f"  {png_file.name} -> {out_file.name}")
        
        result = subprocess.run([
            str(ESRGAN), "-i", str(png_file), "-o", str(out_file),
            "-n", model, "-s", str(scale), "-m", str(MODEL_DIR)
        ], capture_output=True, text=True, timeout=120)
        
        if result.returncode != 0:
            print(f"    Error: {result.stderr}")
        else:
            print(f"    Done")
    
    return True


def repack_xpr(xpr_rel_path, scale):
    """Repack upscaled textures to XPR"""
    name = Path(xpr_rel_path).stem
    png_dir = UPSCALED_ROOT / f"{name}_{scale}x"
    out_dir = REPACKED_ROOT / Path(xpr_rel_path).parent
    out_path = out_dir / f"{name}.xpr"
    
    if not png_dir.exists():
        print(f"Upscaled directory not found: {png_dir}")
        return False
    
    out_dir.mkdir(parents=True, exist_ok=True)
    xpr_path = ASSET_ROOT / xpr_rel_path
    
    print(f"Repacking {xpr_rel_path}...")
    result = subprocess.run([
        sys.executable, str(REPACK_XPR), str(xpr_path), str(png_dir), str(out_path)
    ], capture_output=True, text=True)
    
    if result.returncode != 0:
        print(f"Error: {result.stderr}")
        return False
    
    print(result.stdout)
    return True


def main():
    parser = argparse.ArgumentParser(description="Midnight Club 3 Texture Upscaling Pipeline")
    parser.add_argument("--extract", action="store_true", help="Extract XPR files")
    parser.add_argument("--upscale", action="store_true", help="Upscale extracted textures")
    parser.add_argument("--repack", action="store_true", help="Repack upscaled textures")
    parser.add_argument("--all", action="store_true", help="Run full pipeline")
    parser.add_argument("--xpr", help="Process specific XPR file")
    args = parser.parse_args()
    
    if not any([args.extract, args.upscale, args.repack, args.all]):
        parser.print_help()
        return
    
    files = {args.xpr: XPR_FILES.get(args.xpr, {"scale": 2, "model": "realesrgan-x4plus"})} if args.xpr else XPR_FILES
    
    if args.extract or args.all:
        for xpr_rel_path in files:
            extract_xpr(xpr_rel_path)
    
    if args.upscale or args.all:
        for xpr_rel_path, config in files.items():
            upscale_textures(xpr_rel_path, config["scale"], config["model"])
    
    if args.repack or args.all:
        for xpr_rel_path, config in files.items():
            repack_xpr(xpr_rel_path, config["scale"])
    
    print("Pipeline complete!")


if __name__ == "__main__":
    main()