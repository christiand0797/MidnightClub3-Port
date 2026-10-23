import urllib.request
import json

# Check xpz-image-extractor repo
url = "https://api.github.com/repos/JonAlexInc/xpz-image-extractor"
try:
    response = urllib.request.urlopen(url)
    data = json.loads(response.read().decode())
    print(f"Description: {data['description']}")
    print(f"Language: {data['language']}")
    print(f"Homepage: {data['homepage']}")
except Exception as e:
    print(f"Error: {e}")

# Check xbox-swizzle repo
url2 = "https://api.github.com/repos/abaire/xbox-swizzle"
try:
    response = urllib.request.urlopen(url2)
    data = json.loads(response.read().decode())
    print(f"\nxbox-swizzle:")
    print(f"Description: {data['description']}")
    print(f"Language: {data['language']}")
    print(f"Homepage: {data['homepage']}")
except Exception as e:
    print(f"Error: {e}")