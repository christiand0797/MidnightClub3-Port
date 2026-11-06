"""Browser port: put the play page and the WebAssembly build into the mc3.top site (Cloudflare Worker assets).

  py -3.12 tools/web/stage_site.py      then   cd tools/multiplayer/cloudflare && npx wrangler deploy

Copies tools/web/play.html to tools/multiplayer/browser/play/index.html and the build (templates/game/out/wasm/mc3.js,
mc3.wasm) beside it. Cloudflare serves at most 25 MiB per file, so mc3.wasm goes up as parts (mc3.wasm.0, .1, ...)
listed in mc3.wasm.json; the page joins them. The build files are not committed (.gitignore). The game disc is never
part of the site: each player picks their own in the page.
"""
import hashlib
import json
import shutil
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
BUILD = REPO / "templates/game/out/wasm"
SITE = REPO / "tools/multiplayer/browser/play"
PART = 20 * 1024 * 1024


def main():
    SITE.mkdir(parents=True, exist_ok=True)
    for old in SITE.glob("mc3.wasm*"):
        old.unlink()
    shutil.copyfile(Path(__file__).with_name("play.html"), SITE / "index.html")
    shutil.copyfile(BUILD / "mc3.js", SITE / "mc3.js")
    data = (BUILD / "mc3.wasm").read_bytes()
    parts = []
    for i in range(0, len(data), PART):
        name = f"mc3.wasm.{len(parts)}"
        (SITE / name).write_bytes(data[i:i + PART])
        parts.append(name)
    version = hashlib.sha256(data).hexdigest()[:16]
    (SITE / "mc3.wasm.json").write_text(json.dumps({"parts": parts, "size": len(data), "version": version}) + "\n")
    print(f"[stage] {SITE}: index.html, mc3.js, {len(parts)} wasm parts ({len(data) / 2**20:.1f} MiB, version {version})")


if __name__ == "__main__":
    main()
