# Play in the browser (play host)

`playhost.py` lets people play the PC port from a web page with nothing installed. Each browser player gets a copy
of the game of their own, run by the host machine from the same build local players use: the same mc3.exe,
settings, HD textures, PBR packs and mods. So the browser and local versions match.

Browser and local players share one online world. Each copy runs relay-only (`MC3_RELAY_ONLY=1`), and its tunnel
(`proxy/proxy.py`) joins the relay's Main Lobby, or the room the player typed, exactly as a player at home does.
Everyone in a room sees everyone's System Link matches in Join Game.

```
browser --HTTPS/WSS--> playhost --> stream_proxy <--window-- mc3.exe --instance N <--UDP--> proxy.py --> relay
```

## Run

```powershell
py -3.12 tools/multiplayer/playhost/playhost.py --install templates/game/out/native `
    --relay wss://relay.example.net/ws --max-sessions 2
```

Then open `http://127.0.0.1:8790/`. For players outside this PC, put it behind a TLS reverse proxy and pass
`--public-origin https://play.example.net --trust-proxy`.

- Copies use instance numbers 1-6 and 8 (`--instances`). Instance 7 is never used. A copy's save folder (`hdd`,
  `hddN`) is used only if it doesn't exist yet or this host made it (`.playhost` marker), so saves already on the
  machine are never touched.
- The page keeps a private player key in the browser. Each career is restored when that player comes back and saved
  when the session ends (`--saves`, default `<install>/playhost_saves`).
- One game per client address (`--per-address`). A copy closes after `--idle` seconds with no browser attached.
- The browser's gamepad or keyboard is player 1 (`MC3_REMOTE_PRIMARY=1`, `runtime/core/remote_pad.cpp`).
- The game disc is never served. The host needs the game installed and built from a disc it owns, as for local play.

## Limits today

- The stream is lossless PNG frames (`stream/stream_proxy.py`), roughly 7-20 fps on a LAN. A video codec
  (H.264/WebCodecs) is the next step before it suits internet play. `--scale` trades resolution for frame rate.
- Each copy needs GPU time and its own desktop window. Size `--max-sessions` to the GPU.

## Test

```powershell
py -3.12 tools/multiplayer/playhost/test_playhost.py
```
