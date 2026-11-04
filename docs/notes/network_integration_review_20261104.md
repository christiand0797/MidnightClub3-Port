# Network/browser integration review — 2026-11-04

This is a source review for coordination with the active OpenCode browser/network
owner. It does not start servers, launch a game, capture the desktop, touch saves,
or establish successful multiplayer. The browser/relay files are concurrently
edited foreign work; recheck them before applying any finding.

The present stack has UI, HTTP storage and forwarding fragments. The inspected
fragments do not yet connect the game's Network menu to native/browser peers, and
the browser is a view/control client for a native game process rather than a
browser build of the renderer.

## Concrete transport mismatches

| Boundary | Inspected behavior | Required integration |
| --- | --- | --- |
| Native proxy address | `runtime/xdk/xnet.cpp:55,271` assigns `0x7F000001` directly to `sin_addr.s_addr`. On this little-endian host those bytes address `1.0.0.127`. | Construct the address with `htonl`/`inet_pton`; test the actual four wire bytes. |
| Native header magic | `xnet.cpp:56,265` copies integer `0x4D433350` into the packet, yielding `P3CM`; both Python proxies expect literal `MC3P`. | One versioned byte-defined protocol, with identical C++/Python fixtures. |
| Native source port | `xnet.cpp:584` retains network-order `me.sin_port`, then copies it at `:266`; Python parses `<H`. | Define the wire order explicitly. Keep host-order and network-order variables distinct. |
| Proxy port | Native sends UDP to `30741`; the launcher starts stream proxy UDP `8765` / WS `8766`. The separate UDP proxy defaults to `30741`, but the launcher does not start it. | Select one transport adapter/endpoint and pass it consistently. |
| Header/payload handling | `proxy/proxy.py:51-60` and `stream/stream_proxy.py:326-330` learn the header's port and `continue`, dropping the payload carried in that same datagram. | Registration and datagram records need separate types, or parse-and-forward a combined record. |
| Relay message framing | `relay/server.js:622-626` only parses messages when `typeof raw === 'string'`. Installed `ws` emits text as a Buffer with `isBinary=false` (`node_modules/ws/lib/receiver.js:633`). Binary datagrams are ignored too. | Use `(raw,isBinary)`, bounded UTF-8 JSON for text, and a separately validated datagram path. |
| Relay session handshake | Both Python adapters send `{type:'join', name:...}`. The relay expects `auth`, then `create_game` / `join_game`; there is no `join` case. | One authenticated host/proxy attach flow bound to a selected room and native instance. |
| Discovery versus game traffic | `xnet.cpp:SendOnce` forwards only broadcast sends. Established unicast traffic continues to the ordinary IP path. | Tunnel both discovery and subsequent datagrams, preserving virtual peer address/port identity and replies. |
| Browser input | Browser sends `[type,key,pressed,0]` or button/axis packets. Stream proxy sends `MC3P + port + input` to the game's gameplay UDP socket. Native `Recv` passes these bytes to the original game with no input decoder. | A distinct authenticated native remote-input endpoint must feed the additive pad path, never the original gameplay datagram parser. |

`g_game_udp_port` is one global updated by every bind, including binds not checked
as UDP; it cannot identify several native instances/sockets. Replies are sent to
`127.0.0.1`, whereas `--instance 7` binds `127.0.0.7`. A bridge must preserve an
explicit instance/socket mapping. A source endpoint must also retain each remote
peer's identity; collapsing all tunneled peers onto one localhost source is not
an eight-player session.

## Browser quality and player ownership

`stream_proxy.py:81-121` captures an entire monitor with `mss` and encodes JPEG at
quality 75 by default. It does not capture a selected game render target/window
or any game audio. Every connected browser receives the same picture, and all
browser inputs are sent to the same native port. That is neither independent
players nor an established match between browser and local image quality.

The current capture/encoding function runs synchronously on the asyncio loop;
`broadcast_frame` awaits each client serially. There is no implemented bounded
latest-frame queue or slow-client policy despite the adaptive-stream comments.
The browser sends FPS settings, but the handler updates only quality/scale, not
`fps` and `frame_interval`.

The native game should remain the renderer, with each playing browser assigned
an owned native process/profile or a proven original local-player slot. A
spectator may intentionally share a stream. The host should explicitly choose
which owned game window/output is sent; capture must not expose an unrelated
monitor. Native render resolution, encoder resolution/codec/bitrate and browser
display size are separate settings. Full graphics features remain native-side;
stream quality and latency require their own measured acceptance. Hardware video
encoding/audio transport, reconnect and backpressure remain implementation work.

## Career, saves and car meets

- `browser/index.html:1031-1049` creates a JSON placeholder with position zero,
  money 10000 and no owned cars. This is not an MC3 save or a career snapshot.
- `loadSave` announces success before sending `load_save`. The stream proxy has
  no handler for that message and no native save import/acknowledgment.
- `/api/career` stores caller-supplied counters. No source-backed original
  mission/race result, garage roster or reward event is connected to it.
- `/api/saves/:slot` deletion finds an index in a user-filtered list and uses
  that index in the global `cloud_saves` array (`server.js:396-400`), which can
  delete a different user's row. Fix before allowing real save storage.
- `GET /api/career` mutates `cities_unlocked` from JSON text into an array, then
  attempts to parse the same in-memory value on later reads. Keep persisted and
  response representations separate.

Preserve the original career writer/format. Start with explicit, independently
backed-up per-session copies and read-only state snapshots. A real import needs
the native process closed or its original supported load boundary, a verified
original-format payload, identity/size/hash checks, an independent previous-copy
backup and an actual native acknowledgment. A database row or HTTP 200 does not
establish that the game loaded a save.

Original system-link play and persistent shared-career free roam are distinct
systems. A relay can carry existing session datagrams; it does not make the
single-player career co-operative or synchronize roaming car transforms,
collisions, customization, event state or rewards. Car meets need a defined
authoritative session/ownership model and real remote actors, rather than only
lobby membership. The local UG2 folder was inspected only for relevant short
documentation; no source/protocol contract establishing UG2 career/car-meet
semantics was found, so no such implementation is inferred from its name.

## Proposed owner contract and first acceptance slice

1. Agree one bounded `MC3` tunnel envelope: version/type, native instance/socket
   token, room/peer identifiers, source/destination virtual IPv4+ports, payload
   length and bytes. Specify integer order, max datagram size and control types.
   Keep controller input and media off the original game datagram channel.
2. Native owns socket/peer mapping and original Network-menu lifecycle. Relay
   owns authentication, room membership and destination validation. A proxy owns
   only the explicitly attached local native instance, not an arbitrary port.
3. Browser input uses full bounded pad snapshots with sequence/lease and release
   on blur/disconnect, applied additively to the authorized native instance.
   Preserve physical keyboard/controller input. The current 16 ms global key
   cooldown can drop a release; do not rely on individual lossy key edges.
4. Prove two independent native copies discover, join, exchange bidirectional
   unicast, drive, disconnect and rejoin through the original Network menu. Then
   prove one browser controls its assigned native player while a second native
   client remains independent. Use private saves and retain original career data.
5. Measure actual displayed game output, audio, input latency, resolution and
   encoder drops. Only then expand to career synchronization and persistent car
   meets, with source-backed original event/actor contracts.

## Reviewed source identities

These hashes bind this review's snapshot, not final acceptance. Foreign edits may
supersede them during integration.

| File | SHA-256 |
| --- | --- |
| `runtime/xdk/xnet.cpp` | `7594b25732f623f2f456acd8d29e3aa40d2a26a0aaa4b4f258afb877728f14f9` |
| `tools/multiplayer/proxy/proxy.py` | `915e20928d9258a15c3ea75801d5716d6bdceed4c7d853dee1d307c50cc1bb6e` |
| `tools/multiplayer/stream/stream_proxy.py` | `4664dcc4142ac03a43d22e274e0260c05c2497dffedd70cf32cd270e6a4068a3` |
| `tools/multiplayer/relay/server.js` | `c3587fc9c65b438a73fab4c3872c618dd59fb2351f00c879472071646da00afa` |
| `tools/multiplayer/browser/index.html` | `26e73c71d3401b78f8c4d11e81df72802bd65b654290ea9f6a8b60a4fdda2851` |
| `tools/multiplayer/Launch-Multiplayer.ps1` | `eb54b8641006b7911daffb33718748c56156e9905e09c5bcd87c12cc46d13214` |
