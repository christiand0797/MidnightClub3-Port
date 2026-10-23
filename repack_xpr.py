#!/usr/bin/env python3
"""
Original Xbox XPR0 Texture Repacker
Packs PNG textures back into XPR0 format for Midnight Club 3
"""

import struct
import os
import tempfile
import subprocess
from pathlib import Path
import cv2
import numpy as np

TEXCONV_PATH = r'C:\Users\purpl\Desktop\MidnightClub3-Port\upscale_tools\texconv.exe'

# Original Xbox texture format codes (reverse mapping)
FORMAT_TO_CODE = {
    'DXT1': 0x0C,
    'DXT3': 0x0E,
    'DXT5': 0x0F,
    'A8R8G8B8': 0x06,
    'X8R8G8B8': 0x07,
    'A4R4G4B4': 0x04,
    'R5G6B5': 0x05,
    'A1R5G5B5': 0x02,
    'X1R5G5B5': 0x03,
    'L8': 0x00,
    'A8': 0x19,
    'A8L8': 0x1A,
    'A8B8G8R8': 0x3A,
    'R8G8B8A8': 0x3C,
}


def generate_swizzle_masks(width, height, depth):
    """Generate swizzle masks for Z-order curve"""
    x = y = z = 0
    bit = 1
    mask_bit = 1
    done = False
    while not done:
        done = True
        if bit < width:
            x |= mask_bit
            mask_bit <<= 1
            done = False
        if bit < height:
            y |= mask_bit
            mask_bit <<= 1
            done = False
        if bit < depth:
            z |= mask_bit
            mask_bit <<= 1
            done = False
        bit <<= 1
    return x, y, z


def swizzle_box(src_buf, width, height, depth, bytes_per_pixel):
    """Swizzle a texture (linear -> swizzled)"""
    mask_x, mask_y, mask_z = generate_swizzle_masks(width, height, depth)
    
    row_pitch = width * bytes_per_pixel
    slice_pitch = row_pitch * height
    
    dst = bytearray(width * height * depth * bytes_per_pixel)
    
    off_z = 0
    for z in range(depth):
        off_y = 0
        for y in range(height):
            off_x = 0
            src_tmp = y * row_pitch + z * slice_pitch
            dst_tmp = (off_y + off_z) * bytes_per_pixel
            for x in range(width):
                src_idx = src_tmp + x * bytes_per_pixel
                dst_idx = dst_tmp + off_x * bytes_per_pixel
                dst[dst_idx:dst_idx+bytes_per_pixel] = src_buf[src_idx:src_idx+bytes_per_pixel]
                off_x = (off_x - mask_x) & mask_x
            off_y = (off_y - mask_y) & mask_y
        off_z = (off_z - mask_z) & mask_z
    
    return bytes(dst)


def convert_png_to_xbox(png_path, fmt_code, width, height, is_linear=False):
    """Convert PNG to Xbox texture format"""
    img = cv2.imread(str(png_path), cv2.IMREAD_UNCHANGED)
    if img is None:
        raise ValueError(f'Failed to read {png_path}')
    
    # Ensure RGBA
    if img.shape[2] == 3:
        img = cv2.cvtColor(img, cv2.COLOR_BGR2RGBA)
    elif img.shape[2] == 4:
        img = cv2.cvtColor(img, cv2.COLOR_BGRA2RGBA)
    
    # Resize if needed
    if img.shape[1] != width or img.shape[0] != height:
        img = cv2.resize(img, (width, height), interpolation=cv2.INTER_LANCZOS4)
    
    if fmt_code in (0x0C, 0x0E, 0x0F):  # DXT compressed
        fourcc = {0x0C: b'DXT1', 0x0E: b'DXT3', 0x0F: b'DXT5'}[fmt_code]
        return encode_dxt_texconv(img, fourcc)
    
    elif is_linear:
        # Linear format
        base_map = {0x10: 0x12, 0x11: 0x12, 0x12: 0x12, 0x1C: 0x06, 0x1D: 0x06, 
                   0x1E: 0x07, 0x1F: 0x19, 0x20: 0x06, 0x3F: 0x3A}
        base_fmt = base_map.get(fmt_code, fmt_code)
        bpp = 4  # default
        if base_fmt == 0x06: bpp = 4
        elif base_fmt == 0x07: bpp = 4
        elif base_fmt == 0x19: bpp = 1
        elif base_fmt == 0x3A: bpp = 4
        elif base_fmt == 0x04: bpp = 2
        elif base_fmt == 0x05: bpp = 2
        
        return encode_linear(img, base_fmt, bpp)
    
    else:
        # Swizzled format
        base_map = {
            0x00: 0x00, 0x02: 0x02, 0x03: 0x03, 0x04: 0x04, 0x05: 0x05,
            0x06: 0x06, 0x07: 0x07, 0x0B: 0x06, 0x19: 0x19, 0x1A: 0x1A,
            0x1E: 0x07, 0x30: 0x05, 0x3A: 0x3A, 0x3C: 0x06, 0x3F: 0x3A
        }
        base_fmt = base_map.get(fmt_code, fmt_code)
        bpp = 4
        if base_fmt in (0x00, 0x19): bpp = 1
        elif base_fmt in (0x02, 0x03, 0x04, 0x05, 0x1A): bpp = 2
        elif base_fmt in (0x06, 0x07, 0x3A): bpp = 4
        
        return encode_swizzled(img, base_fmt, bpp)


def encode_dxt_texconv(img, fourcc):
    """Encode image to DXT using texconv"""
    # Save as temporary PNG
    with tempfile.NamedTemporaryFile(suffix='.png', delete=False) as tmp_png:
        cv2.imwrite(tmp_png.name, cv2.cvtColor(img, cv2.COLOR_RGBA2BGRA))
        png_path = tmp_png.name
    
    dds_path = png_path.replace('.png', '.dds')
    
    try:
        # Run texconv to compress to DXT (no mipmaps)
        result = subprocess.run([
            TEXCONV_PATH, '-f', fourcc.decode(), '-ft', 'dds', '-m', '1',
            '-o', os.path.dirname(dds_path), png_path
        ], capture_output=True, text=True, timeout=30)
        
        if result.returncode != 0:
            raise ValueError(f'texconv failed: {result.stdout} {result.stderr}')
        
        # Read the DDS and extract raw DXT data
        with open(dds_path, 'rb') as f:
            dds_data = f.read()
        
        # Skip DDS header (128 bytes: 4 magic + 124 header)
        dxt_data = dds_data[128:]
        return dxt_data
    finally:
        for p in [png_path, dds_path]:
            try:
                os.unlink(p)
            except:
                pass


def encode_linear(img, base_fmt, bpp):
    """Encode image to linear format"""
    height, width = img.shape[:2]
    row = width * bpp
    
    if bpp == 4 and base_fmt in (0x06, 0x07, 0x3A):
        # Convert RGBA to target format
        if base_fmt == 0x06:  # A8R8G8B8 (BGRA)
            arr = cv2.cvtColor(img, cv2.COLOR_RGBA2BGRA)
        elif base_fmt == 0x07:  # X8R8G8B8 (BGRX)
            arr = cv2.cvtColor(img, cv2.COLOR_RGBA2BGRA)
            arr[:, :, 3] = 255
        elif base_fmt == 0x3A:  # A8B8G8R8 (RGBA)
            arr = img
        
        return arr.tobytes()
    
    # Other formats - convert manually
    pixels = bytearray()
    for y in range(height):
        for x in range(width):
            r, g, b, a = img[y, x]
            if base_fmt == 0x00:  # L8
                v = (r + g + b) // 3
                pixels.append(v)
            elif base_fmt == 0x19:  # A8
                pixels.append(a)
            elif base_fmt == 0x1A:  # A8L8
                l = (r + g + b) // 3
                pixels.extend(struct.pack('<H', (a << 8) | l))
            elif base_fmt == 0x04:  # A4R4G4B4
                v = ((a >> 4) << 12) | ((r >> 4) << 8) | ((g >> 4) << 4) | (b >> 4)
                pixels.extend(struct.pack('<H', v))
            elif base_fmt == 0x05:  # R5G6B5
                v = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
                pixels.extend(struct.pack('<H', v))
            else:  # A1R5G5B5, X1R5G5B5
                v = ((a >> 7) << 15) | ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3)
                pixels.extend(struct.pack('<H', v))
    
    return bytes(pixels)


def encode_swizzled(img, base_fmt, bpp):
    """Encode image to swizzled format"""
    linear = encode_linear(img, base_fmt, bpp)
    height, width = img.shape[:2]
    return swizzle_box(linear, width, height, 1, bpp)


def repack_xpr(source_xpr, png_dir, output_xpr):
    """Repack XPR file with new textures"""
    # Read original XPR to get structure
    with open(source_xpr, 'rb') as f:
        data = f.read()
    
    magic, total, header_size = struct.unpack('<III', data[:12])
    magic_str = magic.to_bytes(4, 'little').decode('ascii', errors='ignore')
    
    if magic_str != 'XPR0':
        raise ValueError('Only XPR0 supported')
    
    # Parse texture entries
    entries = []
    offset = 12
    while offset < header_size:
        if offset + 20 > len(data):
            break
        common, data_offset, lock, gpu_format, size = struct.unpack('<IIIII', data[offset:offset+20])
        
        if common == 0xffffffff or common == 0xadadadad:
            break
        
        if (common >> 16) & 7 == 4:
            fmt_code = (gpu_format >> 8) & 0xFF
            is_linear = fmt_code in {0x10, 0x11, 0x12, 0x1C, 0x1D, 0x1E, 0x1F, 0x20, 0x3F}
            
            if is_linear:
                width = (size & 0xFFF) + 1
                height = ((size >> 12) & 0xFFF) + 1
            else:
                width = 1 << ((gpu_format >> 20) & 15)
                height = 1 << ((gpu_format >> 24) & 15)
            
            entries.append({
                'index': len(entries),
                'common': common,
                'data_offset': data_offset,
                'lock': lock,
                'gpu_format': gpu_format,
                'size': size,
                'fmt_code': fmt_code,
                'is_linear': is_linear,
                'width': width,
                'height': height,
            })
        
        offset += 20
    
    print(f'Found {len(entries)} textures to repack')
    
    # Process each texture
    png_dir = Path(png_dir)
    texture_data = []
    new_data_offsets = []
    current_offset = 0
    
    for i, entry in enumerate(entries):
        # Find corresponding PNG
        png_files = list(png_dir.glob(f'*texture_{entry["index"]:04d}*_4x.png'))
        if not png_files:
            png_files = list(png_dir.glob(f'*{entry["index"]:04d}*.png'))
        
        if not png_files:
            print(f'  Warning: No PNG found for texture {entry["index"]}, using original')
            # Use original texture data
            data_start = header_size + entry['data_offset']
            # Calculate size
            fmt_code = entry['fmt_code']
            if fmt_code in (0x0C, 0x0E, 0x0F):
                block_size = 8 if fmt_code == 0x0C else 16
                num_blocks = ((entry['width'] + 3) // 4) * ((entry['height'] + 3) // 4)
                tex_size = num_blocks * block_size
            else:
                bpp = 4
                if entry['is_linear']:
                    bpp = {0x10:2, 0x11:2, 0x12:4, 0x1C:2, 0x1D:2, 0x1E:4, 0x1F:1, 0x20:2, 0x3F:4}.get(fmt_code, 4)
                else:
                    base_map = {0x00:1, 0x02:2, 0x03:2, 0x04:2, 0x05:2, 0x06:4, 0x07:4, 0x0B:4, 0x19:1, 0x1A:2, 0x1E:4, 0x3A:4, 0x3C:4, 0x3F:4}
                    base_fmt = {0x00:0x00, 0x02:0x02, 0x03:0x03, 0x04:0x04, 0x05:0x05, 0x06:0x06, 0x07:0x07, 0x0B:0x06, 0x19:0x19, 0x1A:0x1A, 0x1E:0x07, 0x3A:0x3A, 0x3C:0x06, 0x3F:0x3A}.get(fmt_code, fmt_code)
                    bpp = base_map.get(base_fmt, 4)
                tex_size = entry['width'] * entry['height'] * bpp
            
            tex_data = data[data_start:data_start + tex_size]
        else:
            png_file = png_files[0]
            print(f'  Repacking texture {entry["index"]}: {png_file.name} ({entry["width"]}x{entry["height"]}, fmt=0x{entry["fmt_code"]:02x})')
            tex_data = convert_png_to_xbox(png_file, entry['fmt_code'], entry['width'], entry['height'], entry['is_linear'])
        
        texture_data.append(tex_data)
        new_data_offsets.append(current_offset)
        current_offset += len(tex_data)
    
    # Build new XPR
    new_header_size = 12 + len(entries) * 20
    # Align to 1024 bytes
    new_header_size = ((new_header_size + 1023) // 1024) * 1024
    
    new_total = new_header_size + current_offset
    new_total = ((new_total + 1023) // 1024) * 1024
    
    # Build header
    header = struct.pack('<III', struct.unpack('<I', b'XPR0')[0], new_total, new_header_size)
    header += b'\x00' * (new_header_size - 12)  # Pad header
    
    # Write entries
    entry_bytes = bytearray()
    for i, entry in enumerate(entries):
        new_gpu_format = entry['gpu_format']
        # Update data_offset in entry
        new_entry = struct.pack('<IIIII', 
            entry['common'],
            new_data_offsets[i],
            entry['lock'],
            new_gpu_format,
            entry['size']
        )
        entry_bytes.extend(new_entry)
    
    # Write entries into header at offset 12
    header = header[:12] + entry_bytes + header[12 + len(entry_bytes):]
    
    # Write output
    with open(output_xpr, 'wb') as f:
        f.write(header)
        for tex_data in texture_data:
            f.write(tex_data)
    
    print(f'Repacked XPR saved to {output_xpr} ({len(texture_data)} textures)')
    return output_xpr


if __name__ == '__main__':
    import sys
    
    if len(sys.argv) < 4:
        print('Usage: python repack_xpr.py <source.xpr> <png_dir> <output.xpr>')
        sys.exit(1)
    
    repack_xpr(sys.argv[1], sys.argv[2], sys.argv[3])