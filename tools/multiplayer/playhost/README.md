# Play in the browser (play host)

`playhost.py` lets people play the PC port from a web page with nothing installed. Each browser player gets a copy
of the game of their own, run by the host machine from the same build local players use: the same mc3.exe,
settings, HD textures, PBR packs and mods. Rendering occurs on the host; streamed image quality,
frame rate and latency are separate from local-play quality.

Browser and local players share one online world. Each copy runs relay-only (`MC3_RELAY_ONLY=1`), and its tunnel
(`proxy/proxy.py`) joins the relay's Main Lobby, or the room the player typed, exactly as a player at home does.
Everyone in a room sees everyone's System Link matches in Join Game.

```
browser --HTTPS/WSS--> playhost --> stream_proxy <--window-- mc3.exe --instance N <--UDP--> proxy.py --> relay
```

## Run

```powershell
& 'C:/Users/purpl/AppData/Local/Programs/Python/Python312/python.exe' tools/multiplayer/playhost/playhost.py --install C:/dedicated-playhost-install `
    --relay wss://relay.example.net/ws --max-sessions 2
```

Then open `http://127.0.0.1:8790/`. Use a dedicated installation; it shares PC settings and desktop/GPU resources
between hosted instances. Do not point it at a personal installation while it is being used locally. Public hosting
still needs access control/admission protection, measured GPU quotas and TLS. Only use `--trust-proxy` behind a
trusted proxy that overwrites forwarded addresses and cannot be bypassed; the option trusts that header.

- Copies use instance numbers 1-6 and 8 (`--instances`). Instance 7 is never used. A copy's save folder (`hdd`,
  `hddN`) is used only if it doesn't exist yet or this host made it (`.playhost` marker), so saves already on the
  machine are skipped unless previously marked for hosting. Before reuse, the prior UDATA is retained under a unique
  sibling name. Linked/junction/hard-linked save paths and excessive save trees are refused.
- The page keeps a private player key in the browser. Each career is restored when that player comes back and saved
  when the session ends (`--saves`, default `<install>/playhost_saves`). This key is a bearer credential, not a cloud
  account or cross-device login. Retained copies preserve prior bytes; the host does not prove original-loader validity
  or flush unsaved game memory. Save in the original game before leaving. Lost browser storage can lose access to this key.
- One game per client address (`--per-address`). A copy closes after `--idle` seconds with no browser attached.
- The browser's gamepad or keyboard is player 1 (`MC3_REMOTE_PRIMARY=1`, `runtime/core/remote_pad.cpp`).
- The game disc is never served. The host needs the game installed and built from a disc it owns, as for local play.

## Limits today

- The stream is lossless PNG frames (`stream/stream_proxy.py`), roughly 7-20 fps on a LAN. A video codec
  (H.264/WebCodecs) is the next step before it suits internet play. `--scale` trades resolution for frame rate.
- Each copy needs GPU time and its own desktop window. Size `--max-sessions` to the GPU.
- One viewer is admitted per hosted session; messages and send waits are bounded. Save copies allow 16 MiB,
  retained slot trees 256 MiB and the save root 2 GiB. When full, new changes are refused without deleting prior saves.
- This is not the Cloudflare Worker: the Worker has no Windows game process or GPU streaming host. Account-scoped
  cloud imports and per-player PC configuration isolation are not implemented here.

## Test

```powershell
py -3.12 tools/multiplayer/playhost/test_playhost.py
```
