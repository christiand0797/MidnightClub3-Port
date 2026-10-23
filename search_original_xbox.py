import urllib.request
import json

# Search for original Xbox texture tools
queries = [
    "original xbox texture extractor",
    "xbox xpr original",
    "xbpf extractor",
    "xbck extractor",
    "xbox classic texture",
]

for q in queries:
    url = f"https://api.github.com/search/repositories?q={q.replace(' ', '+')}&sort=stars&order=desc"
    try:
        response = urllib.request.urlopen(url)
        data = json.loads(response.read().decode())
        print(f"\n=== {q} ===")
        for item in data['items'][:5]:
            desc = item['description'][:80] if item['description'] else 'No description'
            print(f"  {item['full_name']}: {desc}")
            print(f"    {item['html_url']}")
    except Exception as e:
        print(f"Error searching {q}: {e}")