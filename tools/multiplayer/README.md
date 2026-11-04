# MC3 Browser Multiplayer — Launcher

Starts the full multiplayer stack:
1. WebSocket Relay (port 8080)
2. Stream Proxy (UDP 8765 + browser WebSocket 8766)
3. Opens the browser client

## Quick start

1. Close mc3.exe if it's running (linking will fail otherwise).
2. Rebuild the game with proxy forwarding:
   ```powershell
   powershell -NoProfile -ExecutionPolicy Bypass -File Build-MC3.ps1 -Jobs 8
   ```
3. Run this launcher:
   ```powershell
   powershell -NoProfile -ExecutionPolicy Bypass -File tools/multiplayer/Launch-Multiplayer.ps1
   ```
4. The browser client opens at `http://localhost:8766` (stream proxy serves the HTML too if you add a static file handler — otherwise open `tools/multiplayer/browser/index.html` directly).

## Manual start (step by step)

### Terminal 1 — Relay (already running on port 8080)
```powershell
node server.js
```
Working dir: `tools/multiplayer/relay`

### Terminal 2 — Stream proxy
```powershell
py -3.12 tools/multiplayer/stream/stream_proxy.py --relay ws://localhost:8080 --port 8765 --fps 60 --quality 75 --scale 1.0
```

### Terminal 3 — Game
```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File Launch-MC3.ps1
```

### Browser
Open `tools/multiplayer/browser/index.html` in Firefox/Chrome/Edge.
Click "Create Game" or join an existing game. The game stream appears fullscreen.

## Controls
- **Keyboard:** W/A/S/D steer, Space = handbrake, Enter = A, Esc = menu, F1 = settings
- **Gamepad:** Left stick steers, buttons map to game actions (Gamepad API)
- **Settings:** F1 opens stream quality / resolution / FPS controls

## Architecture
```
MC3 Game → UDP broadcast → StreamProxy (UDP 8765) → WebSocket → Relay (8080)
                                                       ↳ WebSocket → Browser (8766)
Browser input → WebSocket → StreamProxy → UDP → Game
```

Packets are opaque binary — no game protocol reverse-engineering required.
