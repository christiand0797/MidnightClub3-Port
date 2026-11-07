"""Browser port: put the play page and the WebAssembly build into the mc3.top site (Cloudflare Worker assets).

  py -3.12 tools/web/stage_site.py      then   cd tools/multiplayer/cloudflare && npx wrangler deploy

Copies tools/web/play.html to tools/multiplayer/browser/play/index.html and the build (templates/game/out/wasm/mc3.js,
mc3.wasm) beside it. Cloudflare serves at most 25 MiB per file, so mc3.wasm and disc.iso go up as parts
(mc3.wasm.0, .1, ... and disc.iso.0, .1, ...) listed in mc3.wasm.json and disc.iso.json; the page joins them.
Place your original Xbox disc image at templates/game/out/wasm/disc.iso to bundle it with the site.
"""
import hashlib
import json
import shutil
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
BUILD = REPO / "templates/game/out/wasm"
MP = REPO / "tools" / "multiplayer" / "browser"
SITE = REPO / "tools/multiplayer/browser/play"
PART = 20 * 1024 * 1024


def split_into_parts(data, site, prefix):
    parts = []
    for i in range(0, len(data), PART):
        name = f"{prefix}.{len(parts)}"
        (site / name).write_bytes(data[i:i + PART])
        parts.append(name)
    return parts


def main():
    SITE.mkdir(parents=True, exist_ok=True)
    STATIC = REPO / "tools" / "multiplayer" / "browser" / "static"
    STATIC.mkdir(parents=True, exist_ok=True)
    for old in SITE.glob("mc3.wasm*"):
        old.unlink()
    for old in SITE.glob("disc.iso*"):
        old.unlink()
    shutil.copyfile(Path(__file__).with_name("play.html"), SITE / "index.html")
    shutil.copyfile(Path(__file__).with_name("gpu.js"), SITE / "gpu.js")
    shutil.copyfile(BUILD / "mc3.js", SITE / "mc3.js")
    shutil.copyfile(MP / "relay-client.js", STATIC / "relay-client.js")
    shutil.copyfile(MP / "account-ui.js", STATIC / "account-ui.js")

    wasm = (BUILD / "mc3.wasm").read_bytes()
    wasm_parts = split_into_parts(wasm, SITE, "mc3.wasm")
    wasm_version = hashlib.sha256(wasm).hexdigest()[:16]
    (SITE / "mc3.wasm.json").write_text(json.dumps({"parts": wasm_parts, "size": len(wasm), "version": wasm_version}) + "\n")
    print(f"[stage] {SITE}: index.html, mc3.js, {len(wasm_parts)} wasm parts ({len(wasm) / 2**20:.1f} MiB, version {wasm_version})")

    disc = BUILD / "disc.iso"
    if disc.exists():
        data = disc.read_bytes()
        disc_parts = split_into_parts(data, SITE, "disc.iso")
        disc_version = hashlib.sha256(data).hexdigest()[:16]
        (SITE / "disc.iso.json").write_text(json.dumps({"parts": disc_parts, "size": len(data), "version": disc_version}) + "\n")
        print(f"[stage] {SITE}: disc.iso, {len(disc_parts)} parts ({len(data) / 2**20:.1f} MiB, version {disc_version})")
    else:
        print(f"[stage] WARNING: {disc} not found — disc image not bundled; players must supply their own")
    print(f"[stage] {STATIC}: relay-client.js, account-ui.js")


if __name__ == "__main__":
    main()
