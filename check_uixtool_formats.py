import urllib.request
import json

# Check UIXTool Formats directory
url = "https://api.github.com/repos/Ernegien/UIXTool/contents/src/Formats"
try:
    response = urllib.request.urlopen(url)
    data = json.loads(response.read().decode())
    for item in data:
        print(f"  {item['name']}: {item['type']}")
        if item['type'] == 'file':
            # Get file content
            url2 = item['download_url']
            try:
                response2 = urllib.request.urlopen(url2)
                content = response2.read().decode()
                print(f"    --- {item['name']} ---")
                print(content[:2000])
                print("    ...")
            except Exception as e:
                print(f"    Error reading: {e}")
except Exception as e:
    print(f"Error: {e}")