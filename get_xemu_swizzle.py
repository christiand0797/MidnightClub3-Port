import urllib.request
import json

# Get xemu swizzle.c
url = "https://api.github.com/repos/xemu-project/xemu/contents/hw/xbox/nv2a/pgraph/swizzle.c"
try:
    response = urllib.request.urlopen(url)
    data = json.loads(response.read().decode())
    if data['type'] == 'file':
        url2 = data['download_url']
        response2 = urllib.request.urlopen(url2)
        content = response2.read().decode()
        print(content)
except Exception as e:
    print(f"Error: {e}")

# Also get swizzle.h
url3 = "https://api.github.com/repos/xemu-project/xemu/contents/hw/xbox/nv2a/pgraph/swizzle.h"
try:
    response = urllib.request.urlopen(url3)
    data = json.loads(response.read().decode())
    if data['type'] == 'file':
        url4 = data['download_url']
        response2 = urllib.request.urlopen(url4)
        content = response2.read().decode()
        print("\n=== swizzle.h ===")
        print(content)
except Exception as e:
    print(f"Error: {e}")