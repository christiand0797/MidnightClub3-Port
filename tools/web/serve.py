"""Browser port: serve the play page and the WebAssembly build locally (docs/notes/browser_port_plan.md).

  py -3.12 tools/web/serve.py [--port 8800] [--build templates/game/out/wasm] [--bind mc3.top]

Then open http://localhost:8800/ (or https://mc3.top/ when deployed on your domain). Sends the Cross-Origin-Opener-Policy / Cross-Origin-Embedder-Policy headers the game's threads need (SharedArrayBuffer). Serves tools/web/play.html as / and mc3.js / mc3.wasm from the build folder; nothing else. The game disc is never served: each player picks their own in the page.

For production on mc3.top, use Cloudflare Worker instead:
  py -3.12 tools/web/stage_site.py    # copies build to tools/multiplayer/browser/play/
  cd tools/multiplayer/cloudflare && npx wrangler deploy   # deploys to mc3.top

Testing only: --dev-disc PATH also streams that disc image at /dev-disc to this computer (loopback only), and
http://localhost:8800/?devdisc copies it into the page's storage without the file picker (automated browser tests).
"""
import argparse
import http.server
import mimetypes
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
PAGE = Path(__file__).with_name("play.html")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--port", type=int, default=8800)
    ap.add_argument("--bind", default="0.0.0.0", help="Bind address (mc3.top or 0.0.0.0 for your domain)")
    ap.add_argument("--build", default=str(REPO / "templates/game/out/wasm"))
    ap.add_argument("--dev-disc", help="testing: stream this disc image at /dev-disc (loopback only)")
    a = ap.parse_args()
    build = Path(a.build).resolve()
    MP = REPO / "tools" / "multiplayer" / "browser"
    files = {"/": PAGE, "/index.html": PAGE, "/gpu.js": PAGE.with_name("gpu.js"), "/mc3.js": build / "mc3.js",
             "/mc3.wasm": build / "mc3.wasm",
             "/disc.iso": build / "disc.iso",
             "/static/relay-client.js": MP / "relay-client.js",
             "/static/account-ui.js": MP / "account-ui.js"}

    class Handler(http.server.BaseHTTPRequestHandler):
        def send_isolation(self):
            self.send_header("Cross-Origin-Opener-Policy", "same-origin")
            self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
            self.send_header("Cross-Origin-Resource-Policy", "same-origin")
            self.send_header("Cache-Control", "no-cache")

        def do_GET(self):
            if a.dev_disc and self.path == "/dev-disc" and self.client_address[0] == "127.0.0.1":
                return self.stream(Path(a.dev_disc))
            path = files.get(self.path.split("?")[0])
            if not path or not path.is_file():
                self.send_response(404)
                self.send_isolation()
                self.end_headers()
                return
            data = path.read_bytes()
            self.send_response(200)
            kind = "application/wasm" if path.suffix == ".wasm" else mimetypes.guess_type(path.name)[0] or "text/html"
            self.send_header("Content-Type", kind + ("; charset=utf-8" if kind.startswith(("text/", "application/javascript")) else ""))
            self.send_header("Content-Length", str(len(data)))
            self.send_isolation()
            self.end_headers()
            self.wfile.write(data)

        def stream(self, path):
            size = path.stat().st_size
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", str(size))
            self.send_isolation()
            self.end_headers()
            with path.open("rb") as f:
                while chunk := f.read(4 << 20):
                    self.wfile.write(chunk)

        def log_message(self, fmt, *args):
            pass

    if a.dev_disc and a.bind not in ("127.0.0.1", "localhost"):
        raise SystemExit("--dev-disc is for testing on this computer only (keep --bind 127.0.0.1)")
    server = http.server.ThreadingHTTPServer((a.bind, a.port), Handler)
    print(f"[serve] http://localhost:{a.port}/  (build {build})", flush=True)
    server.serve_forever()


if __name__ == "__main__":
    main()
