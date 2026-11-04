# Network/browser integration review — 2026-11-04

This source snapshot records the interfaces present during the active OpenCode
browser/network work. This review started no servers or games and touched no
saves. Concurrent changes may already supersede individual observations below.

The present stack has UI, HTTP storage and forwarding fragments. The inspected
fragments do not yet connect the game's Network menu to native/browser peers, and
the browser is a view/control client for a native game process rather than a
browser build of the renderer.

## Concrete transport mismatches

| Boundary | Inspected behavior | Current mismatch |
| --- | --- | --- |
| Native proxy address | `runtime/xdk/xnet.cpp:55,271` assigns `0x7F000001` directly to `sin_addr.s_addr`. On this little-endian host those bytes address `1.0.0.127`. | Python listens locally, not at that address. |
| Native header magic | `xnet.cpp:56,265` copies integer `0x4D433350` into the packet, yielding `P3CM`; both Python proxies expect literal `MC3P`. | Byte sequences differ. |
| Native source port | `xnet.cpp:584` retains network-order `me.sin_port`, then copies it at `:266`; Python parses `<H`. | Parsed port is byte-swapped. |
| Proxy port | Native sends UDP to `30741`; the launcher starts stream proxy UDP `8765` / WS `8766`. The separate UDP proxy defaults to `30741`, but the launcher does not start it. | Sender and launched receiver endpoints differ. |
| Header/payload handling | `proxy/proxy.py:51-60` and `stream/stream_proxy.py:326-330` learn the header's port and `continue`, dropping the payload carried in that same datagram. | Native sends a combined header+payload, not a separate registration. |
| Relay message framing | `relay/server.js:622-626` only parses messages when `typeof raw === 'string'`. Installed `ws` emits text as a Buffer with `isBinary=false` (`node_modules/ws/lib/receiver.js:633`). Binary datagrams are ignored too. | Current handler returns for both delivered Buffer forms. |
| Relay session handshake | Both Python adapters send `{type:'join', name:...}`. The relay expects `auth`, then `create_game` / `join_game`; there is no `join` case. | Adapter has no matching attach/session response. |
| Discovery versus game traffic | `xnet.cpp:SendOnce` forwards only broadcast sends. Established unicast traffic continues to the ordinary IP path. | Current tunnel covers discovery only. |
| Browser input | Browser sends `[type,key,pressed,0]` or button/axis packets. Stream proxy sends `MC3P + port + input` to the game's gameplay UDP socket. Native `Recv` passes these bytes to the original game with no input decoder. | Browser pad protocol and original MC3 datagrams are different protocols. |

`g_game_udp_port` is one global updated by every bind, including binds not checked
as UDP; it cannot identify several native instances/sockets. Replies are sent to
`127.0.0.1`, whereas `--instance 7` binds `127.0.0.7`. The current adapter has no
explicit per-instance/socket mapping or distinct virtual source identity for
several remote peers.

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

The implemented browser client displays a native-rendered image. Native render
resolution, encoder resolution/codec/bitrate and browser display size are
separate settings. Independent playing clients, selected-window capture,
hardware video/audio transport and reconnect/backpressure behavior have no
end-to-end proof in this review.

## Career, saves and car meets

- `browser/index.html:1031-1049` creates a JSON placeholder with position zero,
  money 10000 and no owned cars. This is not an MC3 save or a career snapshot.
- `loadSave` announces success before sending `load_save`. The stream proxy has
  no handler for that message and no native save import/acknowledgment.
- `/api/career` stores caller-supplied counters. No source-backed original
  mission/race result, garage roster or reward event is connected to it.
- `/api/saves/:slot` deletion finds an index in a user-filtered list and uses
  that index in the global `cloud_saves` array (`server.js:396-400`), which can
  delete a different user's row.
- `GET /api/career` mutates `cities_unlocked` from JSON text into an array, then
  attempts to parse the same in-memory value on later reads.

No inspected path exports/imports original-format career files or acknowledges a
successful load in the native process. HTTP 200 currently means the database
operation succeeded, rather than that the game loaded a save. The reviewed root
PBR changes do not alter the original career writer/format.

Original system-link play and persistent shared-career free roam are distinct
systems. A relay can carry existing session datagrams; it does not make the
single-player career co-operative or synchronize roaming car transforms,
collisions, customization, event state or rewards. Lobby membership alone has no
corresponding remote actor in the inspected native implementation. The local UG2
folder was inspected only for relevant short
documentation; no source/protocol contract establishing UG2 career/car-meet
semantics was found, so no such implementation is inferred from its name.

## Integration surfaces visible from this snapshot

- The native XNet layer has socket and virtual address information; the relay
  has room/member identities. The six-byte `MC3P` record currently carries only
  one port and payload, with no room, instance or remote peer identity.
- The runtime already has additive physical/scripted pad handling. The browser
  protocol currently has key edges and digital axis directions, with no native
  receiver, sequence/lease or disconnect release. Its 30 ms global key cooldown
  can drop a release immediately following another key event.
- The original game Network-menu session and native-to-native bidirectional
  unicast have no observed relay integration in this review. There is likewise
  no observed independent browser/native two-player session.
- Career synchronization, roaming remote actors and result/reward integration
  remain separate from the relay's existing HTTP records and chat/lobby state.

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
