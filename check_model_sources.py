import urllib.request
import json

# Check NCNN model zoo for models
url = "https://api.github.com/repos/nihui/ncnn-assets/releases"
try:
    response = urllib.request.urlopen(url)
    data = json.loads(response.read().decode())
    for r in data:
        print(r['tag_name'])
        for a in r['assets']:
            print(f"  {a['name']}: {a['browser_download_url']}")
except Exception as e:
    print(f"Error: {e}")

print("\n--- Checking another source ---")
# Check if there are models in the ncnn repo itself
url2 = "https://api.github.com/repos/Tencent/ncnn/contents/benchmark"
try:
    response = urllib.request.urlopen(url2)
    data = json.loads(response.read().decode())
    for item in data:
        print(f"  {item['name']}: {item['download_url']}")
except Exception as e:
    print(f"Error benchmark: {e}")