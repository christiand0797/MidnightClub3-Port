import urllib.request
import json

# Search for Xbox texture extraction tools on GitHub
url = "https://api.github.com/search/repositories?q=xbox+xpr+extractor&sort=stars&order=desc"
try:
    response = urllib.request.urlopen(url)
    data = json.loads(response.read().decode())
    for item in data['items'][:10]:
        print(f"{item['full_name']}: {item['description']}")
        print(f"  {item['html_url']}")
except Exception as e:
    print(f"Error: {e}")