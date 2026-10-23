import urllib.request
import json

# Check xpz-image-extractor releases
url = "https://api.github.com/repos/JonAlexInc/xpz-image-extractor/releases"
try:
    response = urllib.request.urlopen(url)
    data = json.loads(response.read().decode())
    for r in data[:5]:
        print(r['tag_name'])
        for a in r['assets']:
            print(f"  {a['name']}: {a['browser_download_url']}")
except Exception as e:
    print(f"Error: {e}")

# Check repo contents
url2 = "https://api.github.com/repos/JonAlexInc/xpz-image-extractor/contents"
try:
    response = urllib.request.urlopen(url2)
    data = json.loads(response.read().decode())
    for item in data:
        print(f"  {item['name']}: {item['type']}")
except Exception as e:
    print(f"Error contents: {e}")