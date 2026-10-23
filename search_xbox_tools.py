import urllib.request
import json

# Search for Xbox texture tools
queries = [
    "xbox texture extractor",
    "xbox xpr texture",
    "xbox xbpf",
    "original xbox texture format",
]

for q in queries:
    url = f"https://api.github.com/search/repositories?q={q.replace(' ', '+')}&sort=stars&order=desc"
    try:
        response = urllib.request.urlopen(url)
        data = json.loads(response.read().decode())
        print(f"\n=== {q} ===")
        for item in data['items'][:5]:
            print(f"  {item['full_name']}: {item['description'][:80] if item['description'] else 'No description'}")
            print(f"    {item['html_url']}")
    except Exception as e:
        print(f"Error searching {q}: {e}")