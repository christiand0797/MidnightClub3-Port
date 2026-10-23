import urllib.request
import json

url = "https://api.github.com/repos/xinntao/Real-ESRGAN-ncnn-vulkan/releases"
response = urllib.request.urlopen(url)
data = json.loads(response.read().decode())

for r in data:
    print(r['tag_name'])
    for a in r['assets']:
        print(f"  {a['name']}: {a['browser_download_url']}")