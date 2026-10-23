import urllib.request
import json

# Check ncnn-assets/models contents
url = "https://api.github.com/repos/nihui/ncnn-assets/contents/models"
try:
    response = urllib.request.urlopen(url)
    data = json.loads(response.read().decode())
    for item in data:
        print(f"  {item['name']}: {item['type']} - {item['download_url']}")
except Exception as e:
    print(f"Error: {e}")