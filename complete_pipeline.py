#!/usr/bin/env python3
"""
Complete Texture Upscaling Pipeline for Midnight Club 3
Processes all XPR files, upscales with Real-ESRGAN, repacks to XPR
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

# XPR files to process (scale: 2x for Xbox GPU compatibility)
XPR_FILES = {
    "media/dlcustom.xpr": {"scale": 2, "model": "realesrgan-x4plus", "category": "UI Custom"},
    "media/dlres.xpr": {"scale": 2, "model": "realesrgan-x4plus", "category": "UI Downloadable"},
}

# Future: XBCK files (Flash UI) - need different extractor
# XBCK_FILES = {
#     "flash/toplevel.xbck": {"scale": 2, "model": "realesrgan-x4plus", "category": "Main Menu"},
#     "flash/pause.xbck": {"scale": 2, "model": "realesrgan-x4plus", "category": "Pause Menu"},
# }


def check_dependencies():
    """Verify all required tools exist"""
    missing = []
    
    if not EXTRACT_XPR.exists():
        missing.append(f"Extract script: {EXTRACT_XPR}")
    if not REPACK_XPR.exists():
        missing.append(f"Repack script: {REPACK_XPR}")
    if not ESRGAN.exists():
        missing.append(f"Real-ESRGAN: {ESRGAN}")
    if not (MODEL_DIR / "realesrgan-x4plus.param").exists():
        missing.append(f"ESRGAN param: {MODEL_DIR / 'realesrgan-x4plus.param'}")
    if not (MODEL_DIR / "realesrgan-x4plus.bin").exists():
        missing.append(f"ESRGAN bin: {MODEL_DIR / 'realesrgan-x4plus.bin'}")
    
    if missing:
        print("Missing dependencies:")
        for m in missing:
            print(f"  ✗ {m}")
        return False
    
    print("All dependencies found:")
    print(f"  ✓ Extract script: {EXTRACT_XPR}")
    print(f"  ✓ Repack script: {REPACK_XPR}")
    print(f"  ✓ Real-ESRGAN: {ESRGAN}")
    print(f"  ✓ ESRGAN models: {MODEL_DIR}")
    return True


def extract_xpr(xpr_rel_path):
    """Extract XPR file to PNG"""
    xpr_path = ASSET_ROOT / xpr_rel_path
    name = Path(xpr_rel_path).stem
    out_dir = EXTRACTED_ROOT / name
    
    print(f"\n{'='*60}")
    print(f"EXTRACTING: {xpr_rel_path}")
    print(f"{'='*60}")
    
    if not xpr_path.exists():
        print(f"ERROR: Source not found: {xpr_path}")
        return False
    
    out_dir.mkdir(parents=True, exist_ok=True)
    
    result = subprocess.run([
        sys.executable, str(EXTRACT_XPR), str(xpr_path), str(out_dir)
    ], capture_output=True, text=True, timeout=300)
    
    if result.returncode != 0:
        print(f"ERROR: {result.stderr}")
        return False
    
    print(result.stdout)
    
    # Count extracted textures
    png_count = len(list(out_dir.glob("*.png")))
    print(f"Extracted {png_count} textures to {out_dir}")
    return True


def upscale_textures(xpr_rel_path, scale, model):
    """Upscale extracted textures"""
    name = Path(xpr_rel_path).stem
    in_dir = EXTRACTED_ROOT / name
    out_dir = UPSCALED_ROOT / f"{name}_{scale}x"
    
    print(f"\n{'='*60}")
    print(f"UPSCALING: {name} ({scale}x)")
    print(f"{'='*60}")
    
    if not in_dir.exists():
        print(f"ERROR: Input directory not found: {in_dir}")
        return False
    
    out_dir.mkdir(parents=True, exist_ok=True)
    
    png_files = list(in_dir.glob("*.png"))
    print(f"Found {len(png_files)} textures to upscale")
    
    success_count = 0
    for i, png_file in enumerate(png_files):
        out_file = out_dir / f"{png_file.stem}_{scale}x.png"
        print(f"  [{i+1}/{len(png_files)}] {png_file.name} -> {out_file.name}")
        
        result = subprocess.run([
            str(ESRGAN), "-i", str(png_file), "-o", str(out_file),
            "-n", model, "-s", str(scale), "-m", str(MODEL_DIR)
        ], capture_output=True, text=True, timeout=180)
        
        if result.returncode != 0:
            print(f"    ERROR: {result.stderr}")
        else:
            success_count += 1
            print(f"    OK")
    
    print(f"Upscaled {success_count}/{len(png_files)} textures to {out_dir}")
    return success_count > 0


def repack_xpr(xpr_rel_path, scale):
    """Repack upscaled textures to XPR"""
    name = Path(xpr_rel_path).stem
    png_dir = UPSCALED_ROOT / f"{name}_{scale}x"
    out_dir = REPACKED_ROOT / Path(xpr_rel_path).parent
    out_path = out_dir / f"{name}.xpr"
    
    print(f"\n{'='*60}")
    print(f"REPACKING: {xpr_rel_path}")
    print(f"{'='*60}")
    
    if not png_dir.exists():
        print(f"ERROR: Upscaled directory not found: {png_dir}")
        return False
    
    out_dir.mkdir(parents=True, exist_ok=True)
    xpr_path = ASSET_ROOT / xpr_rel_path
    
    result = subprocess.run([
        sys.executable, str(REPACK_XPR), str(xpr_path), str(png_dir), str(out_path)
    ], capture_output=True, text=True, timeout=300)
    
    if result.returncode != 0:
        print(f"ERROR: {result.stderr}")
        return False
    
    print(result.stdout)
    
    # Verify repacked file
    if out_path.exists():
        size_mb = out_path.stat().st_size / (1024*1024)
        print(f"Repacked XPR: {out_path} ({size_mb:.2f} MB)")
    
    return True


def verify_repacked(xpr_rel_path):
    """Verify repacked XPR by extracting and checking"""
    name = Path(xpr_rel_path).stem
    repacked_path = REPACKED_ROOT / Path(xpr_rel_path).parent / f"{name}.xpr"
    verify_dir = EXTRACTED_ROOT / f"verify_{name}"
    
    print(f"\n{'='*60}")
    print(f"VERIFYING: {xpr_rel_path}")
    print(f"{'='*60}")
    
    if not repacked_path.exists():
        print(f"ERROR: Repacked file not found: {repacked_path}")
        return False
    
    result = subprocess.run([
        sys.executable, str(EXTRACT_XPR), str(repacked_path), str(verify_dir)
    ], capture_output=True, text=True, timeout=120)
    
    if result.returncode != 0:
        print(f"ERROR: {result.stderr}")
        return False
    
    print(result.stdout)
    
    png_count = len(list(verify_dir.glob("*.png")))
    print(f"Verified {png_count} textures in repacked XPR")
    return True


def main():
    parser = argparse.ArgumentParser(description="Midnight Club 3 Texture Upscaling Pipeline")
    parser.add_argument("--extract", action="store_true", help="Extract XPR files")
    parser.add_argument("--upscale", action="store_true", help="Upscale extracted textures")
    parser.add_argument("--repack", action="store_true", help="Repack upscaled textures")
    parser.add_argument("--verify", action="store_true", help="Verify repacked XPR files")
    parser.add_argument("--all", action="store_true", help="Run full pipeline (extract -> upscale -> repack -> verify)")
    parser.add_argument("--xpr", help="Process specific XPR file (relative to assets/)")
    parser.add_argument("--scale", type=int, default=2, help="Upscale factor (default: 2)")
    args = parser.parse_args()
    
    if not any([args.extract, args.upscale, args.repack, args.verify, args.all]):
        parser.print_help()
        print("\nAvailable XPR files:")
        for xpr, config in XPR_FILES.items():
            print(f"  {xpr} - {config['category']}")
        return
    
    if not check_dependencies():
        sys.exit(1)
    
    files = {args.xpr: XPR_FILES.get(args.xpr, {"scale": args.scale, "model": "realesrgan-x4plus", "category": "Custom"})} if args.xpr else XPR_FILES
    
    if args.extract or args.all:
        for xpr_rel_path in files:
            if not extract_xpr(xpr_rel_path):
                print(f"Failed to extract {xpr_rel_path}")
                if not args.all:
                    sys.exit(1)
    
    if args.upscale or args.all:
        for xpr_rel_path, config in files.items():
            scale = args.scale if args.scale else config["scale"]
            if not upscale_textures(xpr_rel_path, scale, config["model"]):
                print(f"Failed to upscale {xpr_rel_path}")
                if not args.all:
                    sys.exit(1)
    
    if args.repack or args.all:
        for xpr_rel_path, config in files.items():
            scale = args.scale if args.scale else config["scale"]
            if not repack_xpr(xpr_rel_path, scale):
                print(f"Failed to repack {xpr_rel_path}")
                if not args.all:
                    sys.exit(1)
    
    if args.verify or args.all:
        for xpr_rel_path in files:
            if not verify_repacked(xpr_rel_path):
                print(f"Failed to verify {xpr_rel_path}")
                if not args.all:
                    sys.exit(1)
    
    print(f"\n{'='*60}")
    print("PIPELINE COMPLETE!")
    print(f"{'='*60}")
    print(f"Extracted textures: {EXTRACTED_ROOT}")
    print(f"Upscaled textures:  {UPSCALED_ROOT}")
    print(f"Repacked XPR files: {REPACKED_ROOT}")
    
    # Summary
    print("\nSummary:")
    for xpr_rel_path in files:
        name = Path(xpr_rel_path).stem
        repacked = REPACKED_ROOT / Path(xpr_rel_path).parent / f"{name}.xpr"
        if repacked.exists():
            size_mb = repacked.stat().st_size / (1024*1024)
            print(f"  ✓ {xpr_rel_path} -> {repacked} ({size_mb:.2f} MB)")
        else:
            print(f"  ✗ {xpr_rel_path} -> NOT REPACKED")


if __name__ == "__main__":
    main()