import urllib.request
import json

# Check NCNN model zoo
url = "https://api.github.com/repos/nihui/ncnn-assets/releases"
try:
    response = urllib.request.urlopen(url)
    data = json.loads(response.read().decode())
    for r in data[:10]:
        print(r['tag_name'])
        for a in r['assets']:
            name_lower = a['name'].lower()
            if any(x in name_lower for x in ['realesrgan', 'esrgan', 'x4plus', 'x4', 'upscale']):
                print(f"  {a['name']}: {a['browser_download_url']}")
except Exception as e:
    print(f"Error: {e}")