with open(r'C:\Users\purpl\Desktop\MidnightClub3-Port\templates\game\assets\resources\vehicle\rim.xbpmap', 'r') as f:
    content = f.read()

import csv
from io import StringIO
reader = csv.DictReader(StringIO(content))
rows = list(reader)
print(f'Rows: {len(rows)}')
print(f'Fields: {reader.fieldnames}')

# Group by type
from collections import Counter
types = Counter(row['type'] for row in rows)
print(f'Types: {types}')

# Show first 20
for row in rows[:20]:
    print(f'  page={row["# pageno"]}, offset={row["offset"]}, name={row["name"]}, type={row["type"]}, size={row["size"]}')