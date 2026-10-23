import urllib.request
import json

# Check Tencent ncnn model zoo
url = "https://api.github.com/repos/Tencent/ncnn/releases"
try:
    response = urllib.request.urlopen(url)
    data = json.loads(response.read().decode())
    for r in data[:5]:
        print(r['tag_name'])
        for a in r['assets']:
            if 'realesrgan' in a['name'].lower() or 'esrgan' in a['name'].lower():
                print(f"  {a['name']}: {a['browser_download_url']}")
except Exception as e:
    print(f"Error: {e}")