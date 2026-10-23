import urllib.request
import json

# Search for xemu texture handling
url = "https://api.github.com/search/code?q=repo:xemu-project/xemu+swizzle+xpr&per_page=10"
try:
    response = urllib.request.urlopen(url)
    data = json.loads(response.read().decode())
    for item in data['items']:
        print(f"{item['path']}: {item['html_url']}")
except Exception as e:
    print(f"Error: {e}")

# Also check xemu for XPR handling
url2 = "https://api.github.com/search/code?q=repo:xemu-project/xemu+XPR&per_page=10"
try:
    response = urllib.request.urlopen(url2)
    data = json.loads(response.read().decode())
    for item in data['items']:
        print(f"{item['path']}: {item['html_url']}")
except Exception as e:
    print(f"Error: {e}")