import urllib.request
import json

# Check Real-ESRGAN-ncnn-vulkan repo for all releases including models
url = "https://api.github.com/repos/xinntao/Real-ESRGAN-ncnn-vulkan/releases"
try:
    response = urllib.request.urlopen(url)
    data = json.loads(response.read().decode())
    for r in data:
        print(r['tag_name'])
        for a in r['assets']:
            print(f"  {a['name']}: {a['browser_download_url']}")
except Exception as e:
    print(f"Error: {e}")