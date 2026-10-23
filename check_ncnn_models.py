import urllib.request
import json

# Check NCNN model zoo for realesrgan models
url = "https://api.github.com/repos/nihui/ncnn-assets/releases"
try:
    response = urllib.request.urlopen(url)
    data = json.loads(response.read().decode())
    for r in data:
        print(r['tag_name'])
        for a in r['assets']:
            if 'realesrgan' in a['name'].lower():
                print(f"  {a['name']}: {a['browser_download_url']}")
except Exception as e:
    print(f"Error: {e}")