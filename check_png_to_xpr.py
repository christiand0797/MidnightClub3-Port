import urllib.request
import json

# Check png_to_xpr_rs repo
url = "https://api.github.com/repos/DiscoStarslayer/png_to_xpr_rs"
try:
    response = urllib.request.urlopen(url)
    data = json.loads(response.read().decode())
    print(f"Description: {data['description']}")
    print(f"Language: {data['language']}")
    print(f"Homepage: {data['homepage']}")
except Exception as e:
    print(f"Error: {e}")

# Check releases
url2 = "https://api.github.com/repos/DiscoStarslayer/png_to_xpr_rs/releases"
try:
    response = urllib.request.urlopen(url2)
    data = json.loads(response.read().decode())
    for r in data[:5]:
        print(f"\n{r['tag_name']}")
        for a in r['assets']:
            print(f"  {a['name']}: {a['browser_download_url']}")
except Exception as e:
    print(f"Error releases: {e}")

# Check repo contents
url3 = "https://api.github.com/repos/DiscoStarslayer/png_to_xpr_rs/contents"
try:
    response = urllib.request.urlopen(url3)
    data = json.loads(response.read().decode())
    for item in data:
        print(f"  {item['name']}: {item['type']}")
except Exception as e:
    print(f"Error contents: {e}")