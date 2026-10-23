import urllib.request
import json

# Check UIXTool repo contents
url = "https://api.github.com/repos/Ernegien/UIXTool/contents"
try:
    response = urllib.request.urlopen(url)
    data = json.loads(response.read().decode())
    for item in data:
        print(f"  {item['name']}: {item['type']}")
        if item['type'] == 'dir':
            # Check subdirectory
            url2 = f"https://api.github.com/repos/Ernegien/UIXTool/contents/{item['name']}"
            try:
                response2 = urllib.request.urlopen(url2)
                data2 = json.loads(response2.read().decode())
                for subitem in data2:
                    print(f"    {subitem['name']}: {subitem['type']}")
            except Exception as e:
                print(f"    Error: {e}")
except Exception as e:
    print(f"Error: {e}")