import urllib.request
import json

# Check JPEXS releases
url = "https://api.github.com/repos/jindrapetrik/jpexs-decompiler/releases"
try:
    response = urllib.request.urlopen(url)
    data = json.loads(response.read().decode())
    for r in data[:5]:
        print(r['tag_name'])
        for a in r['assets']:
            print(f"  {a['name']}: {a['browser_download_url']}")
except Exception as e:
    print(f"Error: {e}")