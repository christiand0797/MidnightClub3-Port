import urllib.request
import json

# Check Real-ESRGAN repo for all releases
url = "https://api.github.com/repos/xinntao/Real-ESRGAN/releases"
try:
    response = urllib.request.urlopen(url)
    data = json.loads(response.read().decode())
    for r in data:
        print(r['tag_name'])
        for a in r['assets']:
            print(f"  {a['name']}: {a['browser_download_url']}")
except Exception as e:
    print(f"Error: {e}")