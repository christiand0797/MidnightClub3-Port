import struct

width, height = 1024, 512
fourcc = b'DXT3'
block_size = 16
num_blocks = ((width + 3) // 4) * ((height + 3) // 4)
expected_size = num_blocks * block_size
data = b'\x00' * expected_size

# Correct DDS header (124 bytes)
dds_header = struct.pack('<I', 124)           # dwSize
dds_header += struct.pack('<I', 0x81007)      # dwFlags
dds_header += struct.pack('<I', height)       # dwHeight
dds_header += struct.pack('<I', width)        # dwWidth
dds_header += struct.pack('<I', expected_size) # dwPitchOrLinearSize
dds_header += struct.pack('<I', 0)            # dwDepth
dds_header += struct.pack('<I', 1)            # dwMipMapCount
dds_header += b'\x00' * (4 * 11)              # dwReserved1[11] = 44 bytes

print(f'Header size: {len(dds_header)}')
assert len(dds_header) == 124, f'Header size: {len(dds_header)}'

# DDS_PIXELFORMAT (32 bytes)
pf = struct.pack('<I', 32)        # dwSize
pf += struct.pack('<I', 4)        # dwFlags (DDPF_FOURCC)
pf += fourcc                      # dwFourCC (4 bytes)
pf += struct.pack('<I', 0)        # dwRGBBitCount
pf += struct.pack('<I', 0)        # dwRBitMask
pf += struct.pack('<I', 0)        # dwGBitMask
pf += struct.pack('<I', 0)        # dwBBitMask
pf += struct.pack('<I', 0)        # dwABitMask

print(f'PF size: {len(pf)}')
assert len(pf) == 32, f'PF size: {len(pf)}'

# DDS_CAPS (16 bytes)
caps = struct.pack('<I', 0x1000)  # dwCaps1
caps += struct.pack('<I', 0)      # dwCaps2
caps += struct.pack('<I', 0)      # dwCaps3
caps += struct.pack('<I', 0)      # dwCaps4

print(f'Caps size: {len(caps)}')
assert len(caps) == 16, f'Caps size: {len(caps)}'

dds_data = b'DDS ' + dds_header + pf + caps + data

print(f'Magic: 4, Header: {len(dds_header)}, PF: {len(pf)}, Caps: {len(caps)}')
print(f'Total header+magic: {4 + len(dds_header) + len(pf) + len(caps)}')
print(f'Data: {len(data)}')
print(f'Total: {len(dds_data)}')

with open(r'C:\Users\purpl\Desktop\MidnightClub3-Port\test_dxt3.dds', 'wb') as f:
    f.write(dds_data)

print('Created test DDS')