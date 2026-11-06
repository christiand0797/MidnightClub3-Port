// Browser port under Node (docs/notes/browser_port_plan.md): node tools/web/run_node.cjs <disc image> [--instance N]
// Runs templates/game/out/wasm/mc3-node.cjs (tools/web/build_wasm.py --target node) in the current folder, which gets
// logs, hdd and cap. A WebAssembly program sees only its own environment, so the harness settings are passed on as
// --env arguments: REF=N (every Nth frame drawn on the CPU into cap/ref_<frame>.tga), CAP=N (draw lists), PROBES,
// TRACE, PROBE_WIDTH, SCRIPT.
const path = require('path');
const forward = ['REF', 'CAP', 'PROBES', 'TRACE', 'PROBE_WIDTH', 'SCRIPT'];
for (const k of forward) if (process.env[k] !== undefined) process.argv.push('--env', `${k}=${process.env[k]}`);
// Emscripten's socket layer under Node needs the 'ws' package; the relay folder has it
process.env.NODE_PATH = [path.resolve(__dirname, '../multiplayer/relay/node_modules'), process.env.NODE_PATH]
  .filter(Boolean).join(path.delimiter);
require('module').Module._initPaths();
require(process.env.MC3_WASM || path.resolve(__dirname, '../../templates/game/out/wasm/mc3-node.cjs'));
