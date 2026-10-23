#!/usr/bin/env python3
"""
XBCK Texture Extractor for Midnight Club 3
XBCK appears to be a Flash/Scaleform GFx container with embedded XPR textures
"""

import struct
import os
from pathlib import Path

# XBCK magic: 0x82BF5980 (little endian: 80 59 BF 82)
XBCK_MAGIC = b'\x80\x59\xBF\x82'

# XPR magic for reference
XPR0_MAGIC = b'XPR0'
XPR1_MAGIC = b'XPR1'

# Known Xbox texture format codes
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


def scan_for_xpr(data, start=0):
    """Scan data for XPR0/XPR1 signatures"""
    results = []
    pos = start
    while True:
        pos = data.find(XPR0_MAGIC, pos)
        if pos == -1:
            pos = data.find(XPR1_MAGIC, pos)
            if pos == -1:
                break
            magic = XPR1_MAGIC
        else:
            magic = XPR0_MAGIC
        
        if pos + 12 <= len(data):
            total, header_size = struct.unpack('<II', data[pos+4:pos+12])
            if 32 <= header_size <= total <= len(data) - pos:
                results.append({
                    'offset': pos,
                    'magic': magic,
                    'total': total,
                    'header_size': header_size,
                })
        pos += 4
    return results


def extract_embedded_xpr(xbck_data, xpr_info):
    """Extract a single embedded XPR"""
    offset = xpr_info['offset']
    total = xpr_info['total']
    header_size = xpr_info['header_size']
    
    if offset + total > len(xbck_data):
        return None
    
    xpr_data = xbck_data[offset:offset+total]
    return xpr_data


def extract_xbck(xbck_path, output_dir):
    """Extract all embedded XPR textures from XBCK file"""
    output_dir = Path(output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)
    
    with open(xbck_path, 'rb') as f:
        data = f.read()
    
    if len(data) < 4 or data[:4] != XBCK_MAGIC:
        print(f'Not an XBCK file: {xbck_path}')
        return False
    
    print(f'XBCK file: {xbck_path} ({len(data)} bytes)')
    
    # Scan for embedded XPR files
    xpr_list = scan_for_xpr(data)
    print(f'Found {len(xpr_list)} embedded XPR(s)')
    
    for i, xpr_info in enumerate(xpr_list):
        xpr_data = extract_embedded_xpr(data, xpr_info)
        if xpr_data:
            out_path = output_dir / f'embedded_{i:03d}_{xpr_info["magic"].decode()}_0x{xpr_info["offset"]:x}.xpr'
            out_path.write_bytes(xpr_data)
            print(f'  Extracted: {out_path.name} ({len(xpr_data)} bytes)')
            
            # Also extract textures from this XPR
            xpr_out_dir = output_dir / f'embedded_{i:03d}_textures'
            xpr_out_dir.mkdir(exist_ok=True)
            extract_xpr_from_bytes(xpr_data, xpr_out_dir)
    
    return True


def extract_xpr_from_bytes(xpr_data, output_dir):
    """Extract textures from XPR data (in-memory version of extract_xpr.py)"""
    import cv2
    import numpy as np
    import tempfile
    import subprocess
    import zlib
    
    output_dir = Path(output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)
    
    if len(xpr_data) < 12:
        return
    
    magic, total, header_size = struct.unpack('<III', xpr_data[:12])
    magic_str = magic.to_bytes(4, 'little').decode('ascii', errors='ignore')
    
    if magic_str not in ['XPR0', 'XPR1']:
        return
    
    if magic_str == 'XPR1':
        count = struct.unpack('<I', xpr_data[12:16])[0]
        for i in range(count):
            name_at, record_at, _ = struct.unpack('<III', xpr_data[16 + i*12:16 + i*12 + 12])
            name_at += 12
            record_at += 12
            name_end = xpr_data.find(b'\x00', name_at, header_size)
            name = xpr_data[name_at:name_end].decode('utf-8', errors='replace')
            common, data_offset, lock, gpu_format, size = struct.unpack('<IIIII', xpr_data[record_at:record_at+20])
            
            if (common >> 16) & 7 == 4:
                extract_texture_from_xpr(xpr_data, header_size, name, i, common, data_offset, lock, gpu_format, size, output_dir)
    
    elif magic_str == 'XPR0':
        offset = 12
        index = 0
        while offset < header_size:
            if offset + 20 > len(xpr_data):
                break
            common, data_offset, lock, gpu_format, size = struct.unpack('<IIIII', xpr_data[offset:offset+20])
            
            if common == 0xffffffff or common == 0xadadadad:
                break
            
            if (common >> 16) & 7 == 4:
                name = f'texture_{index:04d}'
                extract_texture_from_xpr(xpr_data, header_size, name, index, common, data_offset, lock, gpu_format, size, output_dir)
                index += 1
            
            offset += 20


def extract_texture_from_xpr(data, header_size, name, index, common, data_offset, lock, gpu_format, size, output_dir):
    """Extract a single texture from XPR data"""
    # This is a simplified version - for full implementation see extract_xpr.py
    fmt_code = (gpu_format >> 8) & 0xFF
    is_linear = fmt_code in LINEAR_FORMATS
    
    if is_linear:
        width = (size & 0xFFF) + 1
        height = ((size >> 12) & 0xFFF) + 1
    else:
        width = 1 << ((gpu_format >> 20) & 15)
        height = 1 << ((gpu_format >> 24) & 15)
    
    if width > 4096 or height > 4096:
        return
    
    data_start = header_size + data_offset
    fmt_name = XPR_TEXTURE_FORMATS.get(fmt_code, ('UNKNOWN', 4))[0]
    print(f'    {name}: format={fmt_name} (0x{fmt_code:02x}), {width}x{height}, offset=0x{data_offset:x}')


if __name__ == '__main__':
    import sys
    if len(sys.argv) < 3:
        print('Usage: python extract_xbck.py <input.xbck> <output_dir>')
        sys.exit(1)
    extract_xbck(sys.argv[1], sys.argv[2])