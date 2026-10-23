import sys
sys.path.insert(0, r'C:\Users\purpl\Desktop\MidnightClub3-Port\upscale_tools\xpz-source\xpz-image-extractor-main')
from xpz_core import extract_file
from pathlib import Path

source = r'C:\Users\purpl\Desktop\MidnightClub3-Port\templates\game\assets\media\dlcustom.xpr'
dest = r'C:\Users\purpl\Desktop\MidnightClub3-Port\extracted\dlcustom'
report = extract_file(source, dest)
print(f'Exported: {report["exported"]}, Skipped: {report["skipped"]}')
for t in report['textures']:
    print(f'  {t["name"]}: {t["status"]} - {t.get("file", t.get("reason", ""))}')