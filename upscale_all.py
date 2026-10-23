#!/usr/bin/env python3
"""Batch upscale all extracted textures"""

import subprocess
import os
from pathlib import Path

ESRGAN = r'C:\Users\purpl\Desktop\MidnightClub3-Port\upscale_tools\realesrgan-ncnn-vulkan-v0.2.0-windows\realesrgan-ncnn-vulkan.exe'
MODEL_DIR = r'C:\Users\purpl\Desktop\MidnightClub3-Port\upscale_tools\realesrgan-ncnn-vulkan-v0.2.0-windows\models'

INPUT_DIRS = [
    r'C:\Users\purpl\Desktop\MidnightClub3-Port\extracted\dlcustom',
    r'C:\Users\purpl\Desktop\MidnightClub3-Port\extracted\dlres',
]

SCALE = 4
MODEL = 'realesrgan-x4plus'

for input_dir in INPUT_DIRS:
    input_path = Path(input_dir)
    output_path = input_path.parent / f'{input_path.name}_4x'
    output_path.mkdir(exist_ok=True)
    
    png_files = list(input_path.glob('*.png'))
    print(f'Processing {len(png_files)} files from {input_path}')
    
    for png_file in png_files:
        out_file = output_path / f'{png_file.stem}_4x.png'
        print(f'  Upscaling {png_file.name} -> {out_file.name}')
        
        cmd = [
            ESRGAN,
            '-i', str(png_file),
            '-o', str(out_file),
            '-n', MODEL,
            '-s', str(SCALE),
            '-m', MODEL_DIR
        ]
        
        result = subprocess.run(cmd, capture_output=True, text=True)
        if result.returncode != 0:
            print(f'    Error: {result.stderr}')
        else:
            print(f'    Done')

print('All done!')