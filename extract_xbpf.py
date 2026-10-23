#!/usr/bin/env python3
"""
XBPF/XBPMAP Texture Extractor for Midnight Club 3
XBPF = pf05 texture archive with table of contents
XBPMAP = CSV map with page numbers, offsets, names, types, sizes
"""

import struct
import csv
from io import StringIO
from pathlib import Path

# XBPF magic: "pf05" (little endian: 35 30 66 70)
XBPF_MAGIC = b'pf05'

# Texture format codes (from XPR)
XPR_TEXTURE_FORMATS = {
    0x00: ('L8', 1),
    0x02: ('A1R5G5B5', 2),
    0x03: ('X1R5G5B5', 2),
    0x04: ('A4R4G4B4', 2),
    0x05: ('R5G6B5', 2),
    0x06: ('A8R8G8B8', 4),
    0x07: ('X8R8G8B8', 4),
    0x0B: ('I8_A8R8G8B8', 4),
    0x0C: ('DXT1', 8),
    0x0E: ('DXT3', 16),
    0x0F: ('DXT5', 16),
    0x11: ('R5G6B5', 2),
    0x12: ('A8R8G8B8', 4),
    0x19: ('A8', 1),
    0x1E: ('X8R8G8B8', 4),
    0x30: ('DEPTH_Y16', 2),
    0x3A: ('A8B8G8R8', 4),
    0x3C: ('R8G8B8A8', 4),
    0x3F: ('A8B8G8R8', 4),
}

LINEAR_FORMATS = {0x10, 0x11, 0x12, 0x1C, 0x1D, 0x1E, 0x1F, 0x20, 0x3F}


def parse_xbpmap(map_path):
    """Parse XBPMAP CSV file"""
    with open(map_path, 'r') as f:
        content = f.read()
    
    reader = csv.DictReader(StringIO(content))
    entries = []
    for row in reader:
        entries.append({
            'page': int(row['# pageno']),
            'offset': int(row['offset']),
            'name': row['name'],
            'type': row['type'],
            'size': int(row['size']),
        })
    return entries


def parse_xbpf_toc(xbpf_path):
    """Parse XBPF table of contents"""
    with open(xbpf_path, 'rb') as f:
        # Read header
        magic = f.read(4)
        if magic != XBPF_MAGIC:
            raise ValueError(f'Not an XBPF file: {magic}')
        
        file_count, toc_offset, data_offset, unk = struct.unpack('<IIII', f.read(16))
        
        # Read table of contents (4 bytes per entry = offset)
        f.seek(toc_offset)
        toc = []
        for i in range(file_count):
            offset = struct.unpack('<I', f.read(4))[0]
            toc.append(offset)
        
        return {
            'file_count': file_count,
            'toc_offset': toc_offset,
            'data_offset': data_offset,
            'toc': toc,
        }


def extract_xbpf(xbpf_path, xbpmap_path, output_dir, filter_type='gfxImage'):
    """Extract textures from XBPF using XBPMAP"""
    output_dir = Path(output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)
    
    # Parse map
    map_entries = parse_xbpmap(xbpmap_path)
    print(f'Parsed {len(map_entries)} map entries')
    
    # Parse XBPF TOC
    xbpf_info = parse_xbpf_toc(xbpf_path)
    print(f'XBPF: {xbpf_info["file_count"]} files, TOC at 0x{xbpf_info["toc_offset"]:x}, data at 0x{xbpf_info["data_offset"]:x}')
    
    # Read full XBPF data
    with open(xbpf_path, 'rb') as f:
        xbpf_data = f.read()
    
    # Group map entries by page
    pages = {}
    for entry in map_entries:
        if entry['type'] != filter_type:
            continue
        page = entry['page']
        if page not in pages:
            pages[page] = []
        pages[page].append(entry)
    
    print(f'Found {len(pages)} pages with {filter_type} textures')
    
    # Extract each texture
    extracted = 0
    for page_num, entries in pages.items():
        if page_num >= len(xbpf_info['toc']):
            print(f'Warning: Page {page_num} exceeds TOC size')
            continue
        
        page_offset = xbpf_info['toc'][page_num]
        page_data_start = xbpf_info['data_offset'] + page_offset
        
        for entry in entries:
            name = entry['name']
            offset = entry['offset']
            size = entry['size']
            
            data_offset = page_data_start + offset
            if data_offset + size > len(xbpf_data):
                print(f'  Warning: {name} exceeds file bounds')
                continue
            
            texture_data = xbpf_data[data_offset:data_offset + size]
            
            # Save raw texture data
            out_path = output_dir / f'{name}.raw'
            out_path.write_bytes(texture_data)
            
            # Try to identify format and convert
            identify_and_convert(texture_data, name, output_dir)
            extracted += 1
            
            if extracted % 100 == 0:
                print(f'  Extracted {extracted} textures...')
    
    print(f'Extracted {extracted} textures to {output_dir}')
    return extracted


def identify_and_convert(texture_data, name, output_dir):
    """Try to identify texture format and convert to PNG"""
    if len(texture_data) < 16:
        return
    
    # Check for XPR header
    if texture_data[:4] in [b'XPR0', b'XPR1']:
        # Embedded XPR - extract using XPR extractor
        xpr_out_dir = output_dir / f'{name}_xpr'
        xpr_out_dir.mkdir(exist_ok=True)
        with open(xpr_out_dir / f'{name}.xpr', 'wb') as f:
            f.write(texture_data)
        # Could call extract_xpr.py here
        return
    
    # Check for DDS header
    if texture_data[:4] == b'DDS ':
        dds_out = output_dir / f'{name}.dds'
        dds_out.write_bytes(texture_data)
        convert_dds_to_png(dds_out, output_dir / f'{name}.png')
        return
    
    # Check for common Xbox texture patterns
    # Many gfxImage are likely DXT compressed
    # Try to guess format from size
    size = len(texture_data)
    
    # Common sizes for DXT textures:
    # 128x128 DXT1 = 8192 bytes (but we see 66688 = 256x256 DXT3/5 or 512x512 DXT1)
    # 66688 bytes = 256x256 DXT3/5 (16 bytes/block * 16*16 blocks = 4096 * 16 = 65536... close)
    # Actually 256x256 DXT3: 16 blocks * 16 blocks * 16 bytes = 4096 bytes? No.
    # 256x256 = 64x64 blocks = 4096 blocks * 16 bytes = 65536 bytes. Close to 66688.
    # The extra ~1KB might be mipmaps or headers.
    
    # For now, save raw and try DDS conversion
    raw_out = output_dir / f'{name}.raw'
    raw_out.write_bytes(texture_data)
    
    # Try to create a DDS header and convert
    try_create_dds(texture_data, name, output_dir)


def try_create_dds(texture_data, name, output_dir):
    """Try to create valid DDS from raw texture data"""
    import struct
    import tempfile
    import subprocess
    
    TEXCONV = r'C:\Users\purpl\Desktop\MidnightClub3-Port\upscale_tools\texconv.exe'
    
    size = len(texture_data)
    
    # Common texture dimensions for this game
    # 66688 bytes suggests 256x256 DXT3/5 (with mipmaps)
    # 66624 similar
    # 17536 bytes = 128x128 DXT3/5 with mipmaps (16384 + mipmaps)
    
    # Try different dimension/format combos
    attempts = [
        # (width, height, fourcc, block_size)
        (256, 256, b'DXT3', 16),  # 66688/16 = 4168 blocks -> 64x64 blocks = 256x256
        (256, 256, b'DXT5', 16),
        (256, 256, b'DXT1', 8),
        (128, 128, b'DXT3', 16),
        (128, 128, b'DXT5', 16),
        (128, 128, b'DXT1', 8),
        (512, 512, b'DXT1', 8),
        (512, 512, b'DXT3', 16),
    ]
    
    for width, height, fourcc, block_size in attempts:
        num_blocks = ((width + 3) // 4) * ((height + 3) // 4)
        expected_size = num_blocks * block_size
        
        # Allow some tolerance for mipmaps
        if abs(size - expected_size) < 2048:
            print(f'  {name}: Trying {width}x{height} {fourcc.decode()} (expected {expected_size}, got {size})')
            
            # Create DDS header
            dds_header = struct.pack('<IIIIIIIII',
                124, 0x81007, height, width,
                expected_size, 0, 0, 0, 0
            )
            pf = struct.pack('<IIIIIIII',
                32, 4, struct.unpack('<I', fourcc)[0], 0, 0, 0, 0, 0
            )
            caps = struct.pack('<IIIII', 0x1000, 0, 0, 0, 0)
            dds_data = b'DDS ' + dds_header + pf + caps + texture_data[:expected_size]
            
            # Try texconv
            with tempfile.NamedTemporaryFile(suffix='.dds', delete=False) as tmp_dds:
                tmp_dds.write(dds_data)
                dds_path = tmp_dds.name
            
            png_path = dds_path.replace('.dds', '.png')
            
            try:
                result = subprocess.run([
                    TEXCONV, '-f', 'R8G8B8A8_UNORM', '-ft', 'png',
                    '-o', str(output_dir), dds_path
                ], capture_output=True, text=True, timeout=10)
                
                if result.returncode == 0:
                    # Find output PNG
                    expected_png = output_dir / f'{Path(dds_path).stem}.png'
                    if expected_png.exists():
                        expected_png.rename(output_dir / f'{name}.png')
                        print(f'  ✓ Converted {name} as {width}x{height} {fourcc.decode()}')
                        # Cleanup
                        try: os.unlink(dds_path)
                        except: pass
                        return
            except:
                pass
            finally:
                try: os.unlink(dds_path)
                except: pass
    
    # If all attempts fail, save raw for manual inspection
    raw_out = output_dir / f'{name}.raw'
    raw_out.write_bytes(texture_data)


if __name__ == '__main__':
    import sys
    import os
    
    if len(sys.argv) < 4:
        print('Usage: python extract_xbpf.py <file.xbpf> <file.xbpmap> <output_dir> [filter_type]')
        sys.exit(1)
    
    filter_type = sys.argv[4] if len(sys.argv) > 4 else 'gfxImage'
    extract_xbpf(sys.argv[1], sys.argv[2], sys.argv[3], filter_type)