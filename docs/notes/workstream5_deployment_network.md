# MC3 Multiplayer — Deployment & Network Documentation

Component layout (`tools/multiplayer/`):

```
relay/             Express + ws HTTP/WebSocket relay (port 8080)
stream/            stream_proxy.py  (UDP 8765 game packets, WS 8766 browser frames)
proxy/             proxy.py         (UDP 30741 native game <-> WS relay)
browser/           index.html + theme.css  (browser spectator/client)
docker-compose.yml relay only (mc3-relay on 8080)
Launch-Multiplayer.ps1  starts relay + stream proxy + opens browser
```

---

## Ports used

| Port | Proto | Service | Notes |
|------|-------|---------|-------|
| 8080 | TCP | Relay HTTP + WebSocket (`/ws`) | `RELAY_PORT`. Public-facing rendezvous for browsers, game proxies, and the stream proxy. |
| 8443 | TCP | WSS (documented only) | `WSS_PORT` in `.env.example`; **not wired** in `server.js` (no HTTPS server). Terminate TLS externally if needed. |
| 30741 | UDP | Native game proxy (proxy.py) | Default `--proxy-udp`. Native MC3 sends discovery/game datagrams here. **Not started by the launcher** (review note #4). |
| 3074 | UDP | Game UDP port (proxy.py `--game-udp`) | Default target port the proxy forwards browser input to. |
| 8765 | UDP | Stream proxy game listener | `stream_proxy.py --port`. Receives game capture/input datagrams on the machine running mc3.exe. |
| 8766 | TCP | Stream proxy browser WebSocket | `port + 1`. Browser connects here for frames (MJPEG/PNG base64) and sends input. |

The launcher (`Launch-Multiplayer.ps1`) starts only the relay (8080) and the
stream proxy (UDP 8765 / WS 8766), then opens `browser/index.html?relay=ws://localhost:8080&stream=ws://localhost:8766`.

---

## Firewall rules

Inbound (on the relay host, if publicly reachable):

- `8080/tcp` — relay HTTP+WS (browsers, game proxies, stream proxy → relay).
- `8443/tcp` — WSS if you terminate TLS at the relay (not currently wired).
- `8766/tcp` — stream proxy browser WS (only if browsers are remote; usually localhost/LAN).
- `8765/udp` — stream proxy game listener (only if the game is remote; usually localhost).
- `30741/udp` — native game proxy (only when deploying proxy.py; not started by launcher).

Outbound:

- Relay host → game proxies / stream proxies (outbound ephemeral; typically unrestricted).
- Game machine → relay `8080/tcp` (outbound WS from proxy.py / stream_proxy.py).

If the relay sits behind a reverse proxy (TLS termination, load balancer), set
`CORS_ORIGIN` to the public domain and **do not** enable `trust proxy` unless the
reverse proxy is trusted — `server.js` explicitly sets `app.set('trust proxy',
false)` and rate-limits on the direct peer address.

---

## NAT considerations — two peers behind the same NAT

Both peers connecting to the relay:

- The relay is a **central outbound rendezvous**: each peer opens an outbound
  `ws://relay:8080` connection. Outbound-initiated connections work through most
  NATs (including symmetric NAT for the initial connect), so no inbound port
  forwarding or STUN/TURN is required for peer-to-relay traffic.
- **Shared IP = shared quota.** `TransportSecurity` applies connect, message,
  byte, and room-creation token buckets per **direct peer IP**. Two peers behind
  the same NAT appear as ONE IP to the relay, so they share:
  - connect burst 8 at 0.5/s,
  - 120 msg/s per connection (burst 240),
  - room-creation burst 2 at 1 per 30s.
  One peer's traffic consumes the other's quota. For LAN play, consider running a
  local relay instance or raising the per-IP limits via `TransportSecurity`
  options.

Direct peer-to-peer (native system-link) is a separate concern:

- The native XNet layer and `proxy.py`/`stream_proxy.py` use UDP. Direct UDP
  hole-punching between two peers behind different NATs requires STUN/TURN or
  port forwarding; the relay only carries the datagrams, it does not perform NAT
  traversal. Two peers behind the **same** NAT can reach each other via the
  relay's public IP without any port forwarding.

Recommended deployment:

1. Relay on a public VPS: `RELAY_PORT=8080`, `JWT_SECRET` set (32+ chars),
   `CORS_ORIGIN=<public domain>`, `NODE_ENV=production`. Terminate TLS in front
   (or use an external WSS endpoint) so tokens and credentials are not plaintext.
2. Stream proxy on the machine running `mc3.exe`: UDP 8765 + WS 8766, pointing
   `--relay ws://relay:8080`. Bind to LAN or localhost depending on whether
   browsers are remote.
3. Native proxy (optional, not launched by default): `proxy.py --relay
   ws://relay:8080 --proxy-udp 30741 --game-udp 3074`.
4. Open firewall: 8080/tcp (public), 8765/udp + 8766/tcp (LAN or localhost),
   30741/udp (if using proxy.py).
5. Data: `server.js` uses `DATA_DIR` (default `./data`); `docker-compose.yml`
   passes `DATABASE_PATH` which `server.js` does **not** read — set `DATA_DIR`
   explicitly in compose if you want `/data`.
