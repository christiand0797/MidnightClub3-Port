import urllib.request
import json

# Check xemu-project/xemu swizzle code
url = "https://api.github.com/repos/xemu-project/xemu/contents/hw/xbox/nv2a"
try:
    response = urllib.request.urlopen(url)
    data = json.loads(response.read().decode())
    for item in data:
        print(f"  {item['name']}: {item['type']}")
except Exception as e:
    print(f"Error: {e}")

# Check for swizzle.c
url2 = "https://api.github.com/repos/xemu-project/xemu/contents/hw/xbox/nv2a/swizzle.c"
try:
    response = urllib.request.urlopen(url2)
    data = json.loads(response.read().decode())
    if data['type'] == 'file':
        url3 = data['download_url']
        response2 = urllib.request.urlopen(url3)
        content = response2.read().decode()
        print(content[:5000])
except Exception as e:
    print(f"Error swizzle: {e}")