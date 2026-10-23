import os
import struct

# Check toplevel.xbck in detail
path = r'C:\Users\purpl\Desktop\MidnightClub3-Port\templates\game\assets\flash\toplevel.xbck'
with open(path, 'rb') as f:
    data = f.read()

print(f'File size: {len(data)}')
print(f'Magic: {data[:4].hex()}')

# Header seems to be 128 bytes based on XPR
# Let's look at the structure
# magic(4), unk1(4), unk2(4), data_size(4), then maybe more?
vals = struct.unpack('<IIII', data[:16])
print(f'Header (16 bytes): {vals}')

# Check if there's a resource table like XPR
# XPR0 has: magic(4), total(4), header_size(4), then resources
# But this magic is different

# Look for texture data
# Search for DXT magic
for i in range(0, min(2048, len(data)), 16):
    chunk = data[i:i+16]
    hex_str = ' '.join(f'{b:02x}' for b in chunk)
    ascii_str = ''.join(chr(b) if 32 <= b < 127 else '.' for b in chunk)
    if any(c != '.' for c in ascii_str) or i < 64:
        print('{:04x}: {:48s} {}'.format(i, hex_str, ascii_str))

# Check at offset 128 (like XPR)
if len(data) > 128:
    print('\nAt offset 128:')
    vals = struct.unpack('<IIIII', data[128:148])
    print(f'  {vals}')