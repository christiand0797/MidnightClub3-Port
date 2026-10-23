import os
import struct

# Check a larger .xbck file
files = os.listdir(r'C:\Users\purpl\Desktop\MidnightClub3-Port\templates\game\assets\flash')
xbck_files = [f for f in files if f.endswith('.xbck')]

# Check a few different sizes
for f in ['toplevel.xbck', 'pause.xbck', 'ms_vp_350z_04.xbck']:
    path = os.path.join(r'C:\Users\purpl\Desktop\MidnightClub3-Port\templates\game\assets\flash', f)
    if os.path.exists(path):
        with open(path, 'rb') as fp:
            data = fp.read(64)
        print(f'{f}: magic={data[:4]}, size={os.path.getsize(path)}')
        vals = struct.unpack('<IIII', data[:16])
        print(f'  Header: {vals}')
        print()