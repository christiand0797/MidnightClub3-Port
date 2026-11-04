# MC3 Network Menu + Native Save Import — Integration Plan

This document is the concrete, address-level plan for connecting the native
`mc3.exe` to the relay/proxy stack. All addresses are from the original USA Remix
XBE (`0b3d81a02834e57864a601d473d5296cc0a2463e4b837373d91030f751fa51a3`),
cited via generated `x86_recomp.N.cpp` instruction comments in
`docs/notes/career_save_network_trace_20261104.md`. None of this requires
launching mc3.exe; it is the exact hook list for when a native build is available.

No C++ compiler is present on this host (no `cl.exe`, `g++`), so the host XNet
layer (`runtime/xdk/xnet.cpp`) changes below are **syntax-reviewed only** — they
cannot be compiled here. The proxy/relay stack (Python/Node) is fully tested
(12/12 + 12/12 + 1/1).

---

## 1. Native save import (bug #11 remainder)

### What exists today (server side)
- `POST /api/saves` — stores caller-supplied `save_data` (opaque JSON), sets
  `native_ack = false`, returns `native_ack` in the response.
- `POST /api/saves/:slot/ack` — sets `native_ack = true` and
  `native_acked_at`. The native game (or stream proxy) calls this after
  successfully importing a cloud save. HTTP 200 on `/api/saves` means DB write
  only; `native_ack == true` means the game loaded it.
- `GET /api/saves` / `GET /api/saves/:slot` — return `native_ack` so the browser
  can show "loaded by game" vs "pending".

### What mc3.exe must do (new native code)

**Step A — Read the cloud save from the relay**
The browser sends `load_save` over the stream WebSocket → stream proxy forwards
as a UDP datagram to mc3.exe's bound game port (MC3P 8-byte header + JSON
payload). mc3.exe's `xnet.cpp` `Recv()` already reads datagrams into guest
buffers. The game must:

1. Intercept `recvfrom`/`WSARecvFrom` on the game's UDP socket (already HLE'd by
   `xnet.cpp`).
2. Detect the `MC3P` magic (bytes 0–3) in the received datagram.
3. Parse bytes 8+ as JSON: `{slot, save_name, save_data (base64), t}`.
4. Base64-decode `save_data` → original save bytes.

**Step B — Import into career memory**
Using the traced original offsets
(`docs/notes/career_save_network_trace_20261104.md`):

| Item | Original offset | Size | Notes |
|------|----------------|------|-------|
| Career manager singleton | `[0x557F0C]` | 4-byte pointer | Runtime singleton |
| Career data object | manager `+0x0C` | `0xCC8` bytes | |
| Money (wallet) | career data `+0xACC` | int32 | Read `0x148A76`/`0x152746`, write `0x1527DE` |
| Current city | career data `+0x50` | — | |
| Owned car count | global `0x53F80C` | uint8, max 30 | |
| Owned car selected | global `0x53F80D` | uint8 | |
| Owned roster | profile `+0x504` count, `+0x505` selection, first CarCfg `+0x508` | stride `0x108` | CarCfg vtable `0x4483DC` |
| CarCfg RideHeight | `+0x16`/`+0x17` | 2 bytes | per axle |
| CarCfg TireWidth | `+0x18`/`+0x19` | 2 bytes | per axle |
| CarCfg RimSize | `+0x1A`/`+0x1B` | 2 bytes | per axle |
| CarCfg TireProfile | `+0x1C`/`+0x1D` | 2 bytes | per axle |
| Dirty byte | manager `+0x20` | uint8 | Set when career data changes |

The native import must:
- Write money to career data `+0xACC`.
- Write city to career data `+0x50`.
- Write owned cars into the roster (profile `+0x504` count, CarCfg stride
  `0x108`, fields at `+0x16`/`+0x18`/`+0x1A`/`+0x1C` per axle).
- Set the dirty byte at manager `+0x20` so the original save writer picks it up.
- Set car `+0x5E = 0` for human-owned cars (AI writes 1).

**Step C — Acknowledge via the relay**
After the import succeeds, mc3.exe (or its stream proxy) calls:
```
POST /api/saves/:slot/ack  (Authorization: Bearer <token>)
```
This sets `native_ack = true` on the server. The browser's `sendLoadSave()`
waits for the `load_save_ack` WebSocket message with `ok: true`, then shows
"Game acknowledged save: <name>".

If the import fails (invalid save format, wrong slot, etc.), mc3.exe does **not**
call `/ack`, and the browser shows an error after the 8s timeout.

### Native code locations to add (when building mc3.exe)
- `runtime/xdk/xnet.cpp`: In `Recv()`, after copying datagram bytes into guest
  buffers, check for `MC3P` magic at bytes 0–3. If present, parse bytes 8+ as
  JSON and enqueue a save-import work item on the game thread (do not block the
  recv pump).
- A new save-import module (e.g. `runtime/app/save_import.cpp`) that:
  1. Parses the JSON payload.
  2. Writes career fields using the offsets above.
  3. Sets the dirty byte.
  4. Calls `POST /api/saves/:slot/ack` via WinHTTP (or delegates to the stream
     proxy, which already has HTTP to the relay).

---

## 2. Network menu integration

### Original game Network menu (addresses from the trace)

| Address | Meaning |
|---------|---------|
| `0x45BB44` | `MenuNetworkHostJoin` string; allocation `0x1E8`, constructor `0x16CC20`, called at `0x1581C2..0x1581E1` |
| `0x45BAF8` | `MenuNetworkHostSelect` string; separate menu instance |
| `0x45E6B4` | `mcNetworkHostJoin`, returned by `sub_0016CC60` |
| `0x45E6C8` | Host/join menu vtable, installed by constructor at `0x16CC47` |

Original guest API entry points (fixed in `templates/game/config/game.toml`):
- `recvfrom 0x3CC0BE`
- `sendto 0x3CC0DC`
- `WSASendTo 0x3CC0EB`
- `select 0x3CC086`
- `XNetXnAddrToInAddr 0x3CBFBB`

### Original message-name table (IDs `0x40–0x68`)

These are **name mappings only** — no packet offsets/lengths are proven yet.
The relay treats payloads as opaque bytes, which is correct for Phase 1.

| ID | Name | Relevant for |
|----|------|--------------|
| `0x40` | INPUT | Remote control input (per-frame) |
| `0x41` | PLAYER | Remote player transform/state |
| `0x42` | PLAYER_AND_INPUT | Combined state + input |
| `0x43` | RACE_CONFIG | Lobby race setup |
| `0x44` | MCNET_PLAYER_CAR_CONFIG | Car + CarCfg sync |
| `0x49` | GAME_STATE | Session state (countdown/racing/finished) |
| `0x4A` | RACE_STATE | Checkpoint/lap progress |
| `0x4B` | PLAYER_CONFIG | Player name/color/car |
| `0x4C` | CHECKPOINT | Checkpoint passed |
| `0x56` | PLAYER_DAMAGE | Car damage sync |
| `0x58` | CHAT | Text chat |
| `0x5F`–`0x68` | Host migration/session control | Host handoff, start |

### What mc3.exe must do (new native code)

**Phase 1 — Datagram passthrough (no game changes)**
The host XNet layer (`xnet.cpp:SendOnce()`) already forwards every datagram
(broadcast + unicast, except local loopback) to the proxy on `127.0.0.1:30741`
with the 8-byte `MC3P` header (src_port + dst_port) + opaque original payload.
The original game message bytes travel as-is after the header. The relay treats
them as opaque. This gives two independent peers discovery + unicast through the
tunnel (verified 12/12 in `test_two_peer.py`).

**Phase 2 — Message schema discovery (native RE)**
Trace each branch of `sub_001AFBB0` / jump table `0x1AFD10` to determine field
offsets and lengths for at least:
- `0x40` INPUT — steer, gas, brake, handbrake
- `0x41` PLAYER / `0x42` PLAYER_AND_INPUT — position, rotation, health
- `0x44` MCNET_PLAYER_CAR_CONFIG — CarCfg fields (ride height `+0x16`, tire
  width `+0x18`, rim size `+0x1A`, tire profile `+0x1C` per axle)
- `0x49` GAME_STATE / `0x4A` RACE_STATE — session state
- `0x4B` PLAYER_CONFIG — name, color, car choice
- `0x4C` CHECKPOINT / `0x66` MCNET_PLAYER_SCORE — results
- `0x58` CHAT — text

Observe `sendto`/`recvfrom` call sites to find serializers/deserializers.

**Phase 3 — Remote actor (native + relay)**
Build a remote player car/controller:
- Car factory: `sub_002CBC40`, human factory `0x2CBDE0` (vtable `0x476940`),
  AI factory `0x1FAE10` (vtable `0x46C730`).
- Car `+0x50` = vehSim, `+0x64` = CarCfg, `+4` = controller, `+0x5E` = human/AI
  flag (human writes 0, AI writes 1).
- Map `0x41` PLAYER / `0x42` PLAYER_AND_INPUT → remote transform interpolation.
- Map `0x44` MCNET_PLAYER_CAR_CONFIG → car + CarCfg sync.
- Map `0x49` GAME_STATE / `0x4A` RACE_STATE / `0x4C` CHECKPOINT / `0x66`
  MCNET_PLAYER_SCORE → session state and results.

No remote-player factory, controller vtable, actor lifetime, city streaming
membership, collision authority, or net interpolation is established yet. This
is the largest remaining native task.

---

## 3. Browser HW-accelerated stream (bug #10, implemented)

`stream_proxy.py` supports two paths:
- **PNG/monitor fallback** (default): whole-monitor mss capture → PNG → base64
  → JSON frame. Works without ffmpeg.
- **HW-accelerated game-window** (`--capture-window --hw-encoder h264_nvenc`):
  Win32 `EnumWindows` match MC3 title → `GetClientRect` → `PrintWindow` → raw
  BGR24 → ffmpeg HW encoder (`h264_nvenc`/`qsv`/`amf`/libx264 fallback) → H.264
  Annex-B → WebSocket **binary** frames.

`browser/index.html` matches:
- `hello` message reports `codec: "h264"` or `"png"`.
- `VideoDecoder` (config `avc1.640028`) → `<canvas>`; fallback to `<video>`
  + `MediaSource`; fallback to `data:image/png;base64`.

Not yet end-to-end tested with mc3.exe running (window match requires the game
window title to contain "Midnight Club 3").

---

## 4. Remaining open work (requires mc3.exe build)

| # | Task | What's needed |
|---|------|---------------|
| 1 | Build mc3.exe | Visual Studio with VC tools; `Build-MC3.ps1 -Jobs 8`. No compiler on this host. |
| 2 | Native save import | xnet.cpp `MC3P` detection in `Recv()`; save-import module writing career `+0xACC`, `+0x50`, roster `+0x504`/stride `0x108`; dirty byte `+0x20`; `POST /api/saves/:slot/ack`. |
| 3 | Network menu integration | Hook `MenuNetworkHostJoin` (`0x45BB44`/`0x16CC20`) to create/join games via relay; message schema discovery for `sub_001AFBB0` branches. |
| 4 | Remote actor | Car factory/remote controller chain; transform interpolation; collision authority. |
| 5 | End-to-end test | Launch two `mc3.exe --instance N` instances; verify discovery + unicast + save load through the tunnel. |

---

## 5. Verification status (all tests pass, mc3.exe not launched)

| Test | Result |
|------|--------|
| `test_two_peer.py` | 12/12 PASS — two independent peers, A→relay→B datagram with src+dst port identity, ~0.6ms RTT, relay restart + reconnect, malformed frames dropped |
| `transport_security.test.js` | 12/12 PASS — rate limiting, quotas, message validation, auth, backpressure |
| `server_security.test.js` | 1/1 PASS — live HTTP/WS server |
| `node --check` (7 JS files) | EXIT 0 all |
| Python `ast.parse` (3 py files) | OK all |

No C++ compiler on this host → `runtime/xdk/xnet.cpp` changes are syntax-reviewed only, not compiled.

---

## 6. Files changed (this workstream)

- `runtime/xdk/xnet.cpp` — 8-byte proxy header, per-socket peers, loopback dedup, unicast forwarding
- `tools/multiplayer/proxy/proxy.py` — 8-byte header, no `continue`, dst_port routing, no feedback loop
- `tools/multiplayer/stream/stream_proxy.py` — HW encode path, window capture, `load_save` handler
- `tools/multiplayer/browser/index.html` — WebCodecs H.264 decode, `sendLoadSave`, `load_save_ack`
- `tools/multiplayer/browser/account-ui.js` — "Load into game" button, `onLoadSave` mount option
- `tools/multiplayer/relay/server.js` — save size cap, validation, `native_ack`, `POST /api/saves/:slot/ack`
- `tools/multiplayer/Launch-Multiplayer.ps1` — starts UDP proxy on 30741
- `tools/multiplayer/tests/test_two_peer.py` — 12 checks including malformed frames
- `docs/notes/opencode_online_progress.md` — full findings and test results
