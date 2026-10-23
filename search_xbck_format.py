import urllib.request
import json

# Search for XBCK format
url = "https://api.github.com/search/code?q=xbck+scaleform&per_page=10"
try:
    response = urllib.request.urlopen(url)
    data = json.loads(response.read().decode())
    for item in data['items']:
        print(f"{item['path']}: {item['html_url']}")
except Exception as e:
    print(f"Error: {e}")

# Also search for the magic
url2 = "https://api.github.com/search/code?q=82BF5980+scaleform&per_page=10"
try:
    response = urllib.request.urlopen(url2)
    data = json.loads(response.read().decode())
    for item in data['items']:
        print(f"{item['path']}: {item['html_url']}")
except Exception as e:
    print(f"Error: {e}")