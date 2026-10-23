import struct

# Create a simple test DDS file with DXT3 data
width, height = 1024, 512
fourcc = b'DXT3'
block_size = 16
num_blocks = ((width + 3) // 4) * ((height + 3) // 4)
expected_size = num_blocks * block_size

# Create dummy DXT3 data (all zeros = transparent black)
data = b'\x00' * expected_size

# Create DDS header - with proper pitch for compressed
dds_header = struct.pack('<I', 124)  # dwSize
dds_header += struct.pack('<I', 0x80007)  # dwFlags (without DDSD_LINEARSIZE)
dds_header += struct.pack('<I', height)  # dwHeight
dds_header += struct.pack('<I', width)  # dwWidth
dds_header += struct.pack('<I', expected_size)  # dwPitchOrLinearSize
dds_header += struct.pack('<I', 0)  # dwDepth
dds_header += struct.pack('<I', 0)  # dwMipMapCount
dds_header += b'\x00' * 44  # dwReserved1[11]

# DDS pixel format (32 bytes)
pf = struct.pack('<I', 32)  # dwSize
pf += struct.pack('<I', 4)  # dwFlags (DDPF_FOURCC)
pf += struct.pack('<I', struct.unpack('<I', fourcc)[0])  # dwFourCC
pf += b'\x00' * 20  # dwRGBBitCount, dwRBitMask, dwGBitMask, dwBBitMask, dwABitMask

# DDS caps (16 bytes)
caps = struct.pack('<I', 0x1000)  # dwCaps1 (DDSCAPS_TEXTURE)
caps += struct.pack('<I', 0)  # dwCaps2
caps += struct.pack('<I', 0)  # dwCaps3
caps += struct.pack('<I', 0)  # dwCaps4

dds_data = b'DDS ' + dds_header + pf + caps + data

print(f'Header size: {len(dds_header)}')
print(f'PF size: {len(pf)}')
print(f'Caps size: {len(caps)}')
print(f'Total header: {4 + len(dds_header) + len(pf) + len(caps)}')
print(f'Data size: {len(data)}')
print(f'Total: {len(dds_data)}')

with open(r'C:\Users\purpl\Desktop\MidnightClub3-Port\test_dxt3.dds', 'wb') as f:
    f.write(dds_data)

print('Created test DDS')