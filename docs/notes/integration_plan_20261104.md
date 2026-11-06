# MC3 Network Menu + Native Save Import — Integration Plan

This records the original-game interfaces and proposed cloud bridge, with static
evidence kept separate from adapter tests and native acceptance. Original-image
addresses refer to USA Remix XBE SHA-256
`0b3d81a02834e57864a601d473d5296cc0a2463e4b837373d91030f751fa51a3`.
The detailed caller chains are in
[career/save/network trace](career_save_network_trace_20261104.md).

A C++ toolchain **is available on this host**. Root's native builds are recorded
under `.local/continuation-20261104-r1/native-build-r2.log` and later numbered
logs. Build success does not by itself prove native network/save/PBR behavior.
The Python/Node results below describe the particular adapter tests reported by
that workstream; they are separate from launching two original-game peers.

---

## 1. Opaque cloud snapshot and original load boundary

### Current server and adapter facts

`/api/cloud` stores account-scoped opaque original files with canonical base64,
SHA-256 and optimistic `base_revision`. This is distinct from legacy
`/api/saves`, whose `save_data` is caller-supplied JSON. Neither response proves
a game loaded the bytes.

The current `POST /api/saves/:slot/ack` handler accepts an authenticated client
claim and writes `native_ack=true`. It has no original-loader observer, pinned
snapshot digest, native process identity or challenge binding. Its value is
therefore a **client-reported acknowledgment**, not evidence of native load
acceptance. Likewise a proxy forwarding a `load_save` datagram establishes
transport delivery only.

The host `MC3P` envelope belongs to the local opaque game-datagram tunnel. Its
header length is a versioned adapter detail. An arbitrary JSON cloud-save
command sent to an original gameplay UDP socket has no established original
message decoder or save-import caller chain. Copying it into a guest receive
buffer does not load a career.

### Source-backed file and loader boundaries

The original save is a complete serialized profile/career image with additional
tail data and a 20-byte signature; money/city/roster offsets describe **live
objects**, not a complete interchangeable save representation.

- Save preparation `0x1A550` copies the profile into a buffer, then calls
  `0x22040 -> 0x193F0`. The outer writer emits the full ordered spans and
  signature. `0x22040` then calls original buffered stream close `0x1A2630`.
  Individual write/flush failures are not all checked by its success return.
- Original read wrapper `0x22310 -> 0x19EC0` reads the complete spans, compares
  the total consumed bytes to file length, and compares the stored 20-byte
  signature to its calculation before returning success.
- `0x19560` applies a successfully read profile to `0x53F308`, invokes the
  original profile normalization, and passes the career blob through
  `0x277E20`. Observing this successful path is stronger evidence than an HTTP
  response or an echoed datagram.
- `kernel::Shutdown` currently calls `std::_Exit`, so process exit alone does
  not imply C stdio buffers/destructors were flushed. An exited process plus
  stable file hashes identifies the collected bytes and a no-writer boundary;
  it is not physical-disk durability or original signature acceptance.

Writing selected money/city/roster fields or a car's ownership byte would skip
the original profile/career load logic, signature checks, registries and
per-car sidecar lineage. Such writes are not the proposed cloud restore path.

### Concrete bridge design and present implementation scope

The new CPU reader `tools/multiplayer/cloud_snapshot.py` collects **explicitly
selected** original save/metadata files and optional known preference files.
It checks an actual process handle, executable path, creation identity and
`--instance`, waits for that same process to exit, and maps its physical
`hddN` to virtual `hdd`. Two stable file passes retain opaque bytes and hashes.
Its output is an exclusive offline proposal, not an upload or restore.

A future restore boundary can use the authenticated opaque snapshot, its exact
revision/digests, an independently preserved local baseline and the selected
closed installation/instance. Once the restored bytes have been installed and
hash-checked, the **original** profile selection/load path determines whether
the game accepts them. No restore writer or native-load observer is implemented
by the reader. Original-loader acceptance remains a separate status with the
exact snapshot digest and process/build context; a caller's `/ack` claim
does not establish it.

This preserves the original save format and normal loader behavior. Shared
career reward/ownership policy is a separate feature, not an automatic merger
of unrelated opaque careers.

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
The branches of `sub_001AFBB0` / jump table `0x1AFD10` return diagnostic
names; they do not serialize payload fields or prove a message-type byte's wire
offset. Actual send/receive caller chains remain the source for schemas. The
names identify candidate concepts for further tracing:
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
| 1 | Exact native candidate | Native builds exist; bind later acceptance to an exact source/runtime hash. |
| 2 | Opaque snapshot/restore and load evidence | Closed selected instance, preserved baseline, opaque exact bytes; original `0x22310/0x19EC0 -> 0x19560` acceptance observed separately from client claims. |
| 3 | Network menu integration | Hook `MenuNetworkHostJoin` (`0x45BB44`/`0x16CC20`) to create/join games via relay; message serializers/dispatchers traced from actual I/O callers. |
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
