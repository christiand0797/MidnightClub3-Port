#!/usr/bin/env python3
"""
Original Xbox XPR0/XPR1 Texture Extractor
Based on xemu swizzle code and UIXTool C# code
Uses texconv for DXT decoding
"""

import struct
import zlib
import os
import tempfile
import subprocess
from pathlib import Path
import cv2
import numpy as np

# Original Xbox texture format codes
XPR_TEXTURE_FORMATS = {
    0x00: ('L8', 1),
    0x02: ('A1R5G5B5', 2),
    0x03: ('X1R5G5B5', 2),
    0x04: ('A4R4G4B4', 2),
    0x05: ('R5G6B5', 2),
    0x06: ('A8R8G8B8', 4),
    0x07: ('X8R8G8B8', 4),
    0x0B: ('I8_A8R8G8B8', 4),
    0x0C: ('DXT1', 8),  # block compressed
    0x0E: ('DXT3', 16), # block compressed
    0x0F: ('DXT5', 16), # block compressed
    0x11: ('R5G6B5', 2),
    0x12: ('A8R8G8B8', 4),
    0x19: ('A8', 1),
    0x1E: ('X8R8G8B8', 4),
    0x30: ('DEPTH_Y16', 2),
    0x3A: ('A8B8G8R8', 4),
    0x3C: ('R8G8B8A8', 4),
    0x3F: ('A8B8G8R8', 4),
}

# Linear formats (non-swizzled)
LINEAR_FORMATS = {0x10, 0x11, 0x12, 0x1C, 0x1D, 0x1E, 0x1F, 0x20, 0x3F}

# BPP for linear formats
LINEAR_BPP = {
    0x10: 2,
    0x11: 2,
    0x12: 4,
    0x1C: 2,
    0x1D: 2,
    0x1E: 4,
    0x1F: 1,
    0x20: 2,
    0x3F: 4,
}

# BPP for base formats
BASE_BPP = {
    0x06: 4, 0x07: 4, 0x3A: 4,
    0x00: 1, 0x19: 1,
    0x1A: 2,
    0x04: 2, 0x05: 2,
    0x02: 2, 0x03: 2,
}

TEXCONV_PATH = r'C:\Users\purpl\Desktop\MidnightClub3-Port\upscale_tools\texconv.exe'


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


def unswizzle_box(src_buf, width, height, depth, bytes_per_pixel):
    """Unswizzle a texture (swizzled -> linear)"""
    mask_x, mask_y, mask_z = generate_swizzle_masks(width, height, depth)
    
    row_pitch = width * bytes_per_pixel
    slice_pitch = row_pitch * height
    
    dst = bytearray(width * height * depth * bytes_per_pixel)
    
    off_z = 0
    for z in range(depth):
        off_y = 0
        for y in range(height):
            off_x = 0
            src_tmp = (off_y + off_z) * bytes_per_pixel
            dst_tmp = y * row_pitch + z * slice_pitch
            for x in range(width):
                src_idx = src_tmp + off_x * bytes_per_pixel
                dst_idx = dst_tmp + x * bytes_per_pixel
                dst[dst_idx:dst_idx+bytes_per_pixel] = src_buf[src_idx:src_idx+bytes_per_pixel]
                off_x = (off_x - mask_x) & mask_x
            off_y = (off_y - mask_y) & mask_y
        off_z = (off_z - mask_z) & mask_z
    
    return bytes(dst)


def decode_dxt_texconv(data, width, height, fourcc):
    """Decode DXT compressed texture using texconv"""
    block_size = 8 if fourcc == b'DXT1' else 16
    num_blocks = ((width + 3) // 4) * ((height + 3) // 4)
    expected_size = num_blocks * block_size
    
    if len(data) < expected_size:
        raise ValueError(f'Truncated DXT data: need {expected_size}, got {len(data)}')
    
    data = data[:expected_size]
    
    # Create correct DDS header (124 bytes) with embedded pixel format and caps
    dds_header = struct.pack('<I', 124)           # dwSize
    dds_header += struct.pack('<I', 0x81007)      # dwFlags (DDSD_CAPS|DDSD_HEIGHT|DDSD_WIDTH|DDSD_PIXELFORMAT|DDSD_LINEARSIZE)
    dds_header += struct.pack('<I', height)       # dwHeight
    dds_header += struct.pack('<I', width)        # dwWidth
    dds_header += struct.pack('<I', expected_size) # dwPitchOrLinearSize
    dds_header += struct.pack('<I', 0)            # dwDepth
    dds_header += struct.pack('<I', 1)            # dwMipMapCount
    dds_header += b'\x00' * (4 * 11)              # dwReserved1[11] = 44 bytes

    # DDS_PIXELFORMAT (32 bytes) - embedded in header
    dds_header += struct.pack('<I', 32)           # dwSize
    dds_header += struct.pack('<I', 4)            # dwFlags (DDPF_FOURCC)
    dds_header += fourcc                          # dwFourCC (4 bytes)
    dds_header += struct.pack('<I', 0)            # dwRGBBitCount
    dds_header += struct.pack('<I', 0)            # dwRBitMask
    dds_header += struct.pack('<I', 0)            # dwGBitMask
    dds_header += struct.pack('<I', 0)            # dwBBitMask
    dds_header += struct.pack('<I', 0)            # dwABitMask

    # DDS_CAPS (16 bytes) - embedded in header
    dds_header += struct.pack('<I', 0x1000)       # dwCaps
    dds_header += struct.pack('<I', 0)            # dwCaps2
    dds_header += struct.pack('<I', 0)            # dwCaps3
    dds_header += struct.pack('<I', 0)            # dwCaps4
    dds_header += struct.pack('<I', 0)            # dwReserved2
    
    assert len(dds_header) == 124, f'Header size: {len(dds_header)}'
    
    dds_data = b'DDS ' + dds_header + data
    
    # Write to temp file
    with tempfile.NamedTemporaryFile(suffix='.dds', delete=False) as tmp_dds:
        tmp_dds.write(dds_data)
        dds_path = tmp_dds.name
    
    # Output directory
    out_dir = os.path.dirname(dds_path)
    
    try:
        # Run texconv - output as PNG
        result = subprocess.run([TEXCONV_PATH, '-f', 'R8G8B8A8_UNORM', '-ft', 'png', '-o', out_dir, dds_path], 
                              capture_output=True, text=True, timeout=30)
        
        if result.returncode != 0:
            raise ValueError(f'texconv failed: {result.stdout} {result.stderr}')
        
        # Read the output PNG (texconv uses the same base name)
        png_path = dds_path.replace('.dds', '.png')
        if not os.path.exists(png_path):
            # Try with .dds output
            dds_out = dds_path
            if os.path.exists(dds_out):
                img = cv2.imread(dds_out, cv2.IMREAD_UNCHANGED)
            else:
                raise ValueError(f'Output file not found: {png_path}')
        else:
            img = cv2.imread(png_path, cv2.IMREAD_UNCHANGED)
        
        if img is None:
            raise ValueError('Failed to read output image')
        
        # Convert to RGBA
        if img.shape[2] == 3:
            img = cv2.cvtColor(img, cv2.COLOR_BGR2RGBA)
        elif img.shape[2] == 4:
            img = cv2.cvtColor(img, cv2.COLOR_BGRA2RGBA)
        
        return img
    finally:
        for p in [dds_path, png_path, dds_path.replace('.dds', '.dds')]:
            try:
                os.unlink(p)
            except:
                pass


def decode_linear(data, width, height, fmt_code, layout='auto'):
    """Decode linear (non-swizzled) texture"""
    bpp = LINEAR_BPP.get(fmt_code, 4)
    row = width * bpp
    stride = row
    
    if layout == 'auto':
        stride = row
    
    if stride < row or stride * height > len(data):
        stride = row
    
    pixels = bytearray()
    for y in range(height):
        start = y * stride
        end = start + row
        pixels.extend(data[start:end])
    
    return bytes(pixels), bpp


def decode_swizzled(data, width, height, fmt_code):
    """Decode swizzled texture"""
    base_fmt = fmt_code
    bpp = BASE_BPP.get(base_fmt, 4)
    row = width * bpp
    expected = row * height
    
    if len(data) < expected:
        raise ValueError(f'Truncated swizzled texture: need {expected}, got {len(data)}')
    
    data = data[:expected]
    pixels = unswizzle_box(data, width, height, 1, bpp)
    return pixels, bpp


def convert_pixels_cv2(pixels, width, height, bpp, base_fmt):
    """Convert raw pixels to RGBA numpy array for OpenCV"""
    if bpp == 4 and base_fmt in (0x06, 0x07, 0x3A):
        arr = np.frombuffer(pixels, dtype=np.uint8).reshape(height, width, 4)
        if base_fmt == 0x06:  # A8R8G8B8 (BGRA)
            return cv2.cvtColor(arr, cv2.COLOR_BGRA2RGBA)
        elif base_fmt == 0x07:  # X8R8G8B8 (BGRX)
            bgra = arr.copy()
            bgra[:, :, 3] = 255
            return cv2.cvtColor(bgra, cv2.COLOR_BGRA2RGBA)
        elif base_fmt == 0x3A:  # A8B8G8R8 (RGBA)
            return arr
    
    rgba = np.zeros((height, width, 4), dtype=np.uint8)
    idx = 0
    for y in range(height):
        for x in range(width):
            if bpp == 1:
                v = pixels[idx]
                idx += 1
                if base_fmt == 0x00:  # L8
                    rgba[y, x] = [v, v, v, 255]
                elif base_fmt == 0x19:  # A8
                    rgba[y, x] = [255, 255, 255, v]
            elif bpp == 2:
                v = int.from_bytes(pixels[idx:idx+2], 'little')
                idx += 2
                if base_fmt == 0x1A:  # A8L8
                    rgba[y, x] = [v & 255, v & 255, v & 255, v >> 8]
                elif base_fmt == 0x04:  # A4R4G4B4
                    rgba[y, x] = [((v >> 8) & 15) * 17, ((v >> 4) & 15) * 17, (v & 15) * 17, (v >> 12) * 17]
                elif base_fmt == 0x05:  # R5G6B5
                    rgba[y, x] = [((v >> 11) & 31) * 255 // 31, ((v >> 5) & 63) * 255 // 63, (v & 31) * 255 // 31, 255]
                else:  # A1R5G5B5, X1R5G5B5
                    rgba[y, x] = [((v >> 10) & 31) * 255 // 31, ((v >> 5) & 31) * 255 // 63, (v & 31) * 255 // 31,
                                  255 if base_fmt == 0x03 or (v & 0x8000) else 0]
    
    return rgba


def extract_xpr(xpr_path, output_dir):
    """Extract all textures from an XPR0/XPR1 file"""
    output_dir = Path(output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)
    
    with open(xpr_path, 'rb') as f:
        data = f.read()
    
    if len(data) < 12:
        raise ValueError('File too small')
    
    magic, total, header_size = struct.unpack('<III', data[:12])
    magic_str = magic.to_bytes(4, 'little').decode('ascii', errors='ignore')
    
    print(f'Magic: {magic_str}, Total: {total}, Header: {header_size}')
    
    if magic_str == 'XPR1':
        count = struct.unpack('<I', data[12:16])[0]
        print(f'XPR1 with {count} resources')
        for i in range(count):
            name_at, record_at, _ = struct.unpack('<III', data[16 + i*12:16 + i*12 + 12])
            name_at += 12
            record_at += 12
            name_end = data.find(b'\x00', name_at, header_size)
            name = data[name_at:name_end].decode('utf-8', errors='replace')
            common, data_offset, lock, gpu_format, size = struct.unpack('<IIIII', data[record_at:record_at+20])
            
            if (common >> 16) & 7 == 4:
                extract_texture(data, header_size, name, i, common, data_offset, lock, gpu_format, size, output_dir)
    
    elif magic_str == 'XPR0':
        offset = 12
        index = 0
        while offset < header_size:
            if offset + 20 > len(data):
                break
            common, data_offset, lock, gpu_format, size = struct.unpack('<IIIII', data[offset:offset+20])
            
            if common == 0xffffffff:
                break
            if common == 0xadadadad:
                break
            
            if (common >> 16) & 7 == 4:
                name = f'texture_{index:04d}'
                extract_texture(data, header_size, name, index, common, data_offset, lock, gpu_format, size, output_dir)
                index += 1
            
            offset += 20
    
    else:
        raise ValueError(f'Unknown magic: {magic_str}')
    
    print(f'Extracted {index} textures to {output_dir}')


def extract_texture(data, header_size, name, index, common, data_offset, lock, gpu_format, size, output_dir):
    """Extract a single texture"""
    fmt_code = (gpu_format >> 8) & 0xFF
    is_linear = fmt_code in LINEAR_FORMATS
    
    if is_linear:
        width = (size & 0xFFF) + 1
        height = ((size >> 12) & 0xFFF) + 1
    else:
        width = 1 << ((gpu_format >> 20) & 15)
        height = 1 << ((gpu_format >> 24) & 15)
    
    if width > 4096 or height > 4096:
        print(f'  Skipping {name}: dimensions too large ({width}x{height})')
        return
    
    data_start = header_size + data_offset
    
    fmt_name, block_bpp = XPR_TEXTURE_FORMATS.get(fmt_code, ('UNKNOWN', 4))
    print(f'  {name}: format={fmt_name} (0x{fmt_code:02x}), {width}x{height}, offset=0x{data_offset:x}, linear={is_linear}')
    
    try:
        if fmt_code in (0x0C, 0x0E, 0x0F):  # DXT compressed
            fourcc = {0x0C: b'DXT1', 0x0E: b'DXT3', 0x0F: b'DXT5'}[fmt_code]
            block_size = 8 if fmt_code == 0x0C else 16
            num_blocks = ((width + 3) // 4) * ((height + 3) // 4)
            expected_size = num_blocks * block_size
            
            tex_data = data[data_start:data_start + expected_size]
            img = decode_dxt_texconv(tex_data, width, height, fourcc)
            
        elif is_linear:
            base_map = {0x10: 0x12, 0x11: 0x12, 0x12: 0x12, 0x1C: 0x06, 0x1D: 0x06, 
                       0x1E: 0x07, 0x1F: 0x19, 0x20: 0x06, 0x3F: 0x3A}
            base_fmt = base_map.get(fmt_code, fmt_code)
            bpp = LINEAR_BPP.get(fmt_code, 4)
            
            row = width * bpp
            stride = row
            expected = stride * height
            
            tex_data = data[data_start:data_start + expected]
            pixels, actual_bpp = decode_linear(tex_data, width, height, fmt_code)
            img = convert_pixels_cv2(pixels, width, height, actual_bpp, base_fmt)
            
        else:
            base_map = {
                0x00: 0x00, 0x02: 0x02, 0x03: 0x03, 0x04: 0x04, 0x05: 0x05,
                0x06: 0x06, 0x07: 0x07, 0x0B: 0x06, 0x19: 0x19, 0x1A: 0x1A,
                0x1E: 0x07, 0x30: 0x05, 0x3A: 0x3A, 0x3C: 0x06, 0x3F: 0x3A
            }
            base_fmt = base_map.get(fmt_code, fmt_code)
            bpp = BASE_BPP.get(base_fmt, 4)
            
            row = width * bpp
            expected = row * height
            
            tex_data = data[data_start:data_start + expected]
            pixels, actual_bpp = decode_swizzled(tex_data, width, height, fmt_code)
            img = convert_pixels_cv2(pixels, width, height, actual_bpp, base_fmt)
        
        # Save
        safe_name = ''.join(c if c.isalnum() or c in '._-' else '_' for c in name)
        out_path = output_dir / f'{index:04d}_{safe_name}.png'
        cv2.imwrite(str(out_path), cv2.cvtColor(img, cv2.COLOR_RGBA2BGRA))
        print(f'    Saved: {out_path}')
        
    except Exception as e:
        print(f'    Error: {e}')
        import traceback
        traceback.print_exc()


if __name__ == '__main__':
    import sys
    
    if len(sys.argv) < 3:
        print('Usage: python extract_xpr.py <input.xpr> <output_dir>')
        sys.exit(1)
    
    extract_xpr(sys.argv[1], sys.argv[2])