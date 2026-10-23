#!/usr/bin/env python3
"""
Texture Upscaling Script for Midnight Club 3
Uses Real-ESRGAN to upscale game textures
"""

import os
import sys
import subprocess
import argparse
from pathlib import Path

# Configuration
ASSET_ROOT = Path(__file__).parent / "templates" / "game" / "assets"
OUTPUT_ROOT = Path(__file__).parent / "upscaled_textures"

# Texture categories and their upscaling settings
TEXTURE_CATEGORIES = {
    "ui_backgrounds": {
        "path": "flash",
        "pattern": "bg_*.xbck",
        "scale": 4,
        "model": "realesrgan-x4plus",
        "note": "UI backgrounds - high scale for text clarity"
    },
    "vehicle_textures": {
        "path": "flash",
        "pattern": "ds_vp_*.xbck",
        "scale": 2,
        "model": "realesrgan-x4plus",
        "note": "Vehicle textures - moderate scale"
    },
    "ui_elements": {
        "path": "media",
        "pattern": "dlcustom.xpr",
        "scale": 4,
        "model": "realesrgan-x4plus",
        "note": "Custom UI elements - high scale for text"
    },
    "dl_resources": {
        "path": "media",
        "pattern": "dlres.xpr",
        "scale": 2,
        "model": "realesrgan-x4plus",
        "note": "Downloadable resources"
    },
}

def check_dependencies():
    """Check if required tools are installed"""
    tools = {
        "realesrgan-ncnn-vulkan": "Real-ESRGAN (recommended)",
        "texconv": "DirectX Texture Converter",
        "xpr2": "XPR extractor (custom)",
    }
    
    missing = []
    for tool, desc in tools.items():
        try:
            subprocess.run([tool, "--version"], capture_output=True, check=False)
            print(f"  ✓ {tool} - {desc}")
        except FileNotFoundError:
            missing.append(f"  ✗ {tool} - {desc}")
    
    if missing:
        print("\nMissing tools:")
        for m in missing:
            print(m)
        print("\nInstall with:")
        print("  Real-ESRGAN: https://github.com/xinntao/Real-ESRGAN-ncnn-vulkan/releases")
        print("  texconv: https://github.com/microsoft/DirectXTex/wiki/Texconv")
        return False
    return True

def extract_xpr(xpr_path, output_dir):
    """Extract .xpr file using xpr2 or similar tool"""
    # TODO: Implement XPR extraction
    # This requires a custom XPR extractor
    print(f"  Extracting {xpr_path} -> {output_dir}")
    # subprocess.run(["xpr2", "extract", str(xpr_path), "-o", str(output_dir)])
    pass

def extract_xbck(xbck_path, output_dir):
    """Extract .xbck file"""
    # TODO: Implement XBCK extraction
    print(f"  Extracting {xbck_path} -> {output_dir}")
    pass

def upscale_texture(input_path, output_path, scale, model):
    """Upscale a single texture using Real-ESRGAN"""
    cmd = [
        "realesrgan-ncnn-vulkan",
        "-i", str(input_path),
        "-o", str(output_path),
        "-n", model,
        "-s", str(scale)
    ]
    print(f"  Upscaling {input_path.name} -> {output_path.name} ({scale}x)")
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0:
        print(f"  Error: {result.stderr}")
        return False
    return True

def process_category(category_name, config):
    """Process all textures in a category"""
    source_dir = ASSET_ROOT / config["path"]
    output_dir = OUTPUT_ROOT / category_name
    
    if not source_dir.exists():
        print(f"Source directory not found: {source_dir}")
        return
    
    output_dir.mkdir(parents=True, exist_ok=True)
    
    # Find files matching pattern
    import glob
    files = list(source_dir.glob(config["pattern"]))
    
    if not files:
        print(f"  No files matching {config['pattern']} in {source_dir}")
        return
    
    print(f"\nProcessing {category_name} ({len(files)} files)...")
    print(f"  Scale: {config['scale']}x, Model: {config['model']}")
    print(f"  {config['note']}")
    
    for file in files:
        # Extract if needed (for .xpr/.xbck)
        if file.suffix in [".xpr", ".xbck"]:
            extract_dir = output_dir / file.stem
            extract_dir.mkdir(exist_ok=True)
            
            if file.suffix == ".xpr":
                extract_xpr(file, extract_dir)
            elif file.suffix == ".xbck":
                extract_xbck(file, extract_dir)
            
            # Upscale extracted textures
            for tex_file in extract_dir.rglob("*.png"):
                out_file = output_dir / f"{file.stem}_{tex_file.name}"
                upscale_texture(tex_file, out_file, config["scale"], config["model"])
        else:
            # Direct upscale for image files
            out_file = output_dir / f"{file.stem}_upscaled{file.suffix}"
            upscale_texture(file, out_file, config["scale"], config["model"])

def main():
    parser = argparse.ArgumentParser(description="Upscale Midnight Club 3 textures")
    parser.add_argument("--check", action="store_true", help="Check dependencies only")
    parser.add_argument("--category", help="Process specific category only")
    parser.add_argument("--all", action="store_true", help="Process all categories")
    args = parser.parse_args()
    
    print("Midnight Club 3 Texture Upscaler")
    print("=" * 50)
    
    if not check_dependencies():
        if not args.check:
            sys.exit(1)
    
    if args.check:
        print("\nAll dependencies satisfied!")
        return
    
    if args.category:
        if args.category in TEXTURE_CATEGORIES:
            process_category(args.category, TEXTURE_CATEGORIES[args.category])
        else:
            print(f"Unknown category: {args.category}")
            print(f"Available: {', '.join(TEXTURE_CATEGORIES.keys())}")
    elif args.all:
        for name, config in TEXTURE_CATEGORIES.items():
            process_category(name, config)
    else:
        print("Usage:")
        print("  --check           Check dependencies")
        print("  --all             Process all categories")
        print("  --category NAME   Process specific category")
        print(f"\nCategories: {', '.join(TEXTURE_CATEGORIES.keys())}")

if __name__ == "__main__":
    main()