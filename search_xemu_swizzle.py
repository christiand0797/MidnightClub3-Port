import urllib.request
import json

# Search for swizzle in xemu
url = "https://api.github.com/repos/xemu-project/xemu/contents/hw/xbox/nv2a/pgraph"
try:
    response = urllib.request.urlopen(url)
    data = json.loads(response.read().decode())
    for item in data:
        print(f"  {item['name']}: {item['type']}")
except Exception as e:
    print(f"Error: {e}")