import struct

# Let's check the exact DDS header structure from Microsoft docs
# https://learn.microsoft.com/en-us/windows/win32/direct3ddds/dds-header

# DDS_HEADER (124 bytes)
# DWORD dwSize = 124
# DWORD dwFlags
# DWORD dwHeight
# DWORD dwWidth
# DWORD dwPitchOrLinearSize
# DWORD dwDepth
# DWORD dwMipMapCount
# DWORD dwReserved1[11]  # 44 bytes

# DDS_PIXELFORMAT (32 bytes)
# DWORD dwSize = 32
# DWORD dwFlags
# DWORD dwFourCC
# DWORD dwRGBBitCount
# DWORD dwRBitMask
# DWORD dwGBitMask
# DWORD dwBBitMask
# DWORD dwABitMask

# DDS_CAPS (16 bytes)
# DWORD dwCaps1
# DWORD dwCaps2
# DWORD dwCaps3
# DWORD dwCaps4

# Total: 124 + 32 + 16 = 172 bytes + 4 magic = 176 bytes

# Let me try with the correct structure

width, height = 1024, 512
fourcc = b'DXT3'
block_size = 16
num_blocks = ((width + 3) // 4) * ((height + 3) // 4)
expected_size = num_blocks * block_size
data = b'\x00' * expected_size

dds_header = struct.pack('<I', 124)  # dwSize
dds_header += struct.pack('<I', 0x81007)  # dwFlags (DDSD_CAPS|DDSD_HEIGHT|DDSD_WIDTH|DDSD_PIXELFORMAT|DDSD_LINEARSIZE)
dds_header += struct.pack('<I', height)
dds_header += struct.pack('<I', width)
dds_header += struct.pack('<I', expected_size)  # dwPitchOrLinearSize
dds_header += struct.pack('<I', 0)  # dwDepth
dds_header += struct.pack('<I', 1)  # dwMipMapCount (1 = no mips beyond base)
dds_header += b'\x00' * 44  # dwReserved1[11] = 44 bytes

# Verify header size
assert len(dds_header) == 124, f'Header size: {len(dds_header)}'

pf = struct.pack('<I', 32)  # dwSize
pf += struct.pack('<I', 4)  # dwFlags (DDPF_FOURCC)
pf += fourcc  # dwFourCC
pf += struct.pack('<I', 0)  # dwRGBBitCount
pf += struct.pack('<I', 0)  # dwRBitMask
pf += struct.pack('<I', 0)  # dwGBitMask
pf += struct.pack('<I', 0)  # dwBBitMask
pf += struct.pack('<I', 0)  # dwABitMask

assert len(pf) == 32, f'PF size: {len(pf)}'

caps = struct.pack('<I', 0x1000)  # dwCaps1 (DDSCAPS_TEXTURE)
caps += struct.pack('<I', 0)  # dwCaps2
caps += struct.pack('<I', 0)  # dwCaps3
caps += struct.pack('<I', 0)  # dwCaps4

assert len(caps) == 16, f'Caps size: {len(caps)}'

dds_data = b'DDS ' + dds_header + pf + caps + data

print(f'Magic: 4, Header: {len(dds_header)}, PF: {len(pf)}, Caps: {len(caps)}')
print(f'Total header+magic: {4 + len(dds_header) + len(pf) + len(caps)}')
print(f'Data: {len(data)}')
print(f'Total: {len(dds_data)}')

with open(r'C:\Users\purpl\Desktop\MidnightClub3-Port\test_dxt3.dds', 'wb') as f:
    f.write(dds_data)

print('Created test DDS')