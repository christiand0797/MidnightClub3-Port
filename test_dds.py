import struct

# Create a simple test DDS file with DXT3 data
width, height = 1024, 512
fourcc = b'DXT3'
block_size = 16
num_blocks = ((width + 3) // 4) * ((height + 3) // 4)
expected_size = num_blocks * block_size

# Create dummy DXT3 data (all zeros = transparent black)
data = b'\x00' * expected_size

# Create DDS header
dds_header = struct.pack('<IIIIIIIII',
    124,  # dwSize
    0x81007,  # dwFlags
    height, width,
    expected_size,  # dwPitchOrLinearSize
    0,  # dwDepth
    0,  # dwMipMapCount
    0,  # dwReserved1
    0   # dwReserved2
)

# DDS pixel format
pf = struct.pack('<IIIIIIII',
    32,  # dwSize
    4,   # dwFlags
    struct.unpack('<I', fourcc)[0],  # dwFourCC
    0, 0, 0, 0, 0
)

# DDS caps
caps = struct.pack('<IIIII',
    0x1000,  # dwCaps1
    0, 0, 0, 0
)

dds_data = b'DDS ' + dds_header + pf + caps + data

with open(r'C:\Users\purpl\Desktop\MidnightClub3-Port\test_dxt3.dds', 'wb') as f:
    f.write(dds_data)

print(f'Created test DDS: {len(dds_data)} bytes')