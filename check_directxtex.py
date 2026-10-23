import urllib.request
import json

# Check DirectXTex releases
url = "https://api.github.com/repos/microsoft/DirectXTex/releases"
try:
    response = urllib.request.urlopen(url)
    data = json.loads(response.read().decode())
    for r in data[:3]:
        print(r['tag_name'])
        for a in r['assets']:
            if 'texconv' in a['name'].lower() or 'directxtex' in a['name'].lower():
                print(f"  {a['name']}: {a['browser_download_url']}")
except Exception as e:
    print(f"Error: {e}")