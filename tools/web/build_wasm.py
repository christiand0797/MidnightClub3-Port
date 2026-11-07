"""Browser port: build the game as WebAssembly with Emscripten (docs/notes/browser_port_plan.md).

Same source set as harness/build.sh (the headless harness): the SDK runtime without its window, SDL input, XAudio2
and D3D12 parts, the game's own sources, the harness stand ins and the generated game code. Objects are rebuilt only
when their source (or, for runtime objects, any header) is newer.

  py -3.12 tools/web/build_wasm.py [--out DIR] [--target node|web] [--jobs N] [--only REGEX] [--keep-going]

Needs Emscripten (EMSDK, default ~/emsdk, version 4.0.15).
"""
import argparse
import hashlib
import os
import re
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
RT = REPO / "runtime"
GAME = REPO / "templates/game"
GEN = GAME / "generated/game"
HARNESS = REPO / "harness"
DEPS = GAME / "out/native/_deps"

# runtime parts the browser replaces (as the harness leaves out), plus Windows-only tools
EXCLUDE = [r"/runtime/app/", r"/core/input\.cpp$", r"/gpu/d3d12\.cpp$", r"/audio/xaudio2\.cpp$", r"/gpu/dlss\.cpp$",
           # PC enhancements on Direct3D 12 / WIC / Media Foundation / bcrypt; the browser's WebGPU backend replaces
           # them (stand ins for the names the rest of the runtime calls: tools/web/web_stubs.cpp)
           r"/gpu/pbr_[a-z_]+\.cpp$", r"/gpu/shader_cache\.cpp$", r"/gpu/texture_pack\.cpp$",
           r"/gpu/texturedumper\.cpp$", r"/gpu/video_hd\.cpp$", r"/gpu/draw_evidence\.cpp$"]


def emxx():
    sdk = Path(os.environ.get("EMSDK", Path.home() / "emsdk"))
    em = sdk / "upstream/emscripten/em++.py"
    if not em.exists():
        sys.exit(f"Emscripten not found at {em}")
    return [sys.executable, str(em)]


def sources(only):
    src = [p for p in sorted(RT.glob("*/*.cpp")) if not any(re.search(x, p.as_posix()) for x in EXCLUDE)]
    src += sorted(GAME.glob("src/*/*.cpp"))
    # the harness stand ins, with the browser entry point in place of its main
    src += [p for p in sorted(HARNESS.glob("*.cpp")) if p.name != "main_test.cpp"]
    src += [Path(__file__).with_name("web_main.cpp"), Path(__file__).with_name("web_stubs.cpp"),
            Path(__file__).with_name("webgpu_backend.cpp"),
            GAME / "out/native/Game_xbe.cpp"]
    src += [DEPS / "imgui-src" / f for f in ("imgui.cpp", "imgui_draw.cpp", "imgui_tables.cpp", "imgui_widgets.cpp")]
    if only:
        src = [p for p in src if re.search(only, p.as_posix())]
    return src


def newest_header():
    stamps = [p.stat().st_mtime for d in (RT, GAME / "src", HARNESS) for p in d.rglob("*.h")]
    return max(stamps + [(GEN / "image_symbols.h").stat().st_mtime])


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--out", default=str(REPO / "templates/game/out/wasm"))
    ap.add_argument("--target", choices=["node", "web"], default="node")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    ap.add_argument("--only", help="compile only host sources matching this regex (no link)")
    ap.add_argument("--keep-going", action="store_true", help="compile everything, report all failures")
    ap.add_argument("--no-link", action="store_true")
    ap.add_argument("--debug", action="store_true", help="link with assertions and function names")
    a = ap.parse_args()
    out = Path(a.out)
    (out / "gen").mkdir(parents=True, exist_ok=True)
    (out / "host").mkdir(parents=True, exist_ok=True)
    em = emxx()

    base = ["-std=c++20", "-pthread", "-fno-strict-aliasing", "-msimd128", "-msse4.2"]
    gen_flags = base + ["-O2", "-g0", "-w", f"-I{RT}", f"-I{RT / 'cpu/freestanding'}", f"-I{GEN}"]
    host_flags = base + (["-O1", "-g"] if a.debug else ["-O2"]) + ["-Wno-everything", f"-I{GAME / 'src'}", f"-I{RT}", f"-I{GEN}", f"-I{HARNESS}",
                         f"-I{HARNESS / 'compat'}", f"-I{DEPS / 'tomlplusplus-src/include'}", f"-I{DEPS / 'imgui-src'}",
                         "-DXBE_RAW_VERTICES=1", "-DXBE_RENDER_THREAD=0", "-DXBE_WEB=1"]
    if a.target == "web":
        host_flags.append("-DMC3_WEB_PAGE=1")
    stamp = hashlib.md5(" ".join(host_flags).encode()).hexdigest()[:8]
    newest = newest_header()

    jobs = []
    for f in sorted(GEN.glob("x86_recomp.*.cpp")) + [GEN / "x86_func_mapping.cpp"]:
        o = out / "gen" / (f.stem + ".o")
        if not a.only and not (o.exists() and o.stat().st_mtime > f.stat().st_mtime):
            jobs.append((f, o, gen_flags))
    host_objs = []
    for f in sources(a.only):
        rel = (f.relative_to(REPO).as_posix() if f.is_relative_to(REPO) else f.name).replace("/", "_")
        o = out / "host" / rel.replace(".cpp", f".{stamp}.o")
        host_objs.append(o)
        if o.exists() and o.stat().st_mtime > max(f.stat().st_mtime, newest):
            continue
        jobs.append((f, o, host_flags))

    failed = []

    def compile_one(job):
        f, o, flags = job
        tmp = o.with_suffix(".tmp")
        r = subprocess.run(em + flags + ["-c", str(f), "-o", str(tmp)], capture_output=True, text=True)
        if r.returncode:
            msg = "\n".join(l for l in (r.stderr + r.stdout).splitlines() if "error" in l.lower())
            return f, msg or r.stderr[-2000:]
        tmp.replace(o)
        return f, None

    print(f"[wasm] {len(jobs)} object(s) to build", flush=True)
    with ThreadPoolExecutor(a.jobs) as pool:
        for f, err in pool.map(compile_one, jobs):
            if err:
                failed.append(f)
                print(f"[wasm] FAILED {f.relative_to(REPO)}\n{err}\n", flush=True)
                if not a.keep_going:
                    pool.shutdown(cancel_futures=True)
                    sys.exit(1)
            else:
                print(f"[wasm] ok {f.relative_to(REPO)}", flush=True)
    if failed:
        sys.exit(f"[wasm] {len(failed)} source(s) failed")
    if a.only or a.no_link:
        return

    exe = out / ("mc3-node.cjs" if a.target == "node" else "mc3.js")
    link = em + base + ["-O2", "-o", str(exe), *map(str, host_objs), *map(str, sorted((out / "gen").glob("*.o"))),
                        "-sSTACK_SIZE=8MB", "-sDEFAULT_PTHREAD_STACK_SIZE=4MB", "-sEXIT_RUNTIME=0"]
    if a.target == "node":
        link += ["-sENVIRONMENT=node", "-sNODERAWFS=1", "-sALLOW_MEMORY_GROWTH=1", "-sINITIAL_MEMORY=1024MB",
                 "-sMAXIMUM_MEMORY=4GB", "-sPTHREAD_POOL_SIZE=32", "-sEXPORTED_RUNTIME_METHODS=ENV,FS,HEAPU8",
                 "-sEXPORTED_FUNCTIONS=_main,_web_set_pad,_malloc,_free"]
    else:
        # the page (tools/web/play.html): main() on a worker so it may block; WASMFS with the browser's private
        # storage (OPFS) for the disc copy and saves; fixed memory so the page's views of it never go stale; the page
        # reads frames and writes the pad through these exports
        link += ["-sENVIRONMENT=web,worker", "-sPROXY_TO_PTHREAD=1", "-sWASMFS=1", "-sALLOW_MEMORY_GROWTH=0",
                 "-sINITIAL_MEMORY=2048MB", "-sPTHREAD_POOL_SIZE=24", "-sEXPORTED_RUNTIME_METHODS=HEAPU8,HEAPU32",
                 "-sEXPORTED_FUNCTIONS=_main,_web_set_pad,_web_frame,_web_frame_lock,_web_gpu_ring,_malloc,_free"]
    if a.debug:
        link += ["-sASSERTIONS=2", "-g2", "-sSTACK_OVERFLOW_CHECK=1"]
    print("[wasm] link", flush=True)
    r = subprocess.run(link, capture_output=True, text=True)
    if r.returncode:
        undefined = sorted(set(re.findall(r"undefined symbol: (\S+)", r.stderr)))
        print("\n".join(undefined[:200]) if undefined else r.stderr[-6000:])
        sys.exit(f"[wasm] link failed ({len(undefined)} undefined symbols)")
    print(f"[wasm] built {exe}")


if __name__ == "__main__":
    main()
