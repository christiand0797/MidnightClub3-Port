# MC3 Multiplayer — Workstream 5 Security Gaps

Source of truth: `tools/multiplayer/relay/` (relay `server.js`, `transport_security.js`,
`account_service.js`, `.env.example`), `tools/multiplayer/browser/index.html`,
`tools/multiplayer/stream/stream_proxy.py`, `tools/multiplayer/proxy/proxy.py`,
and `docs/notes/network_integration_review_20261104.md`.

Test status (2026-11-04):
- `transport_security.test.js` (unit): 12 pass / 0 fail
- `integration.test.js` (integration/load/security): 9 pass / 0 fail
- `server_security.test.js` (live HTTP/WS server): 1 pass / 0 fail

---

## 1. TransportSecurity integration (partial / redundant)

`TransportSecurity` IS imported and wired into `wss.on('connection')` via
`security.attach(ws, req, handler)`, and it enforces connect quotas, per-connection
and per-IP message/byte token buckets, message schema validation, authentication
timeouts, heartbeats, and room quotas.

However, `server.js` ALSO keeps a parallel inline `clients` Map tracking
`{id, type, userId, gameId, role, username, color}` per socket. The two layers can
disagree:

- `server.js` `join_game` (line 693) rejects with `'Leave the current room first.'`
  whenever `client.gameId` is set — even when rejoining the room the socket already
  owns. `TransportSecurity.joinRoom` would allow a same-room re-join (`ok()`).
  Result: the stricter server.js check wins, so a host cannot re-join their own
  game without leaving first. This is redundant, not a bypass, but it is an
  inconsistent enforcement surface that is easy to get wrong during future edits.

Gap: consolidate client/session state into a single authoritative layer
(TransportSecurity) and remove the redundant `clients` Map tracking, or document
which layer is authoritative for each decision.

---

## 2. Authentication / token handling

- **Tokens in localStorage.** `browser/index.html` stores `mc3_token`, `mc3_username`,
  `mc3_userId` in `localStorage` (lines 1142-1144). This is XSS-vulnerable: any
  script injection on the relay origin can steal the session. `account_service`
  issues HttpOnly, SameSite=Lax device cookies for the same purpose, but the browser
  does not use them for the relay WS — it uses localStorage + Bearer.
- **Token sent in WS message bodies.** The browser sends `token` inside
  `create_game` (line 854) and `join_game` (line 860) WS messages, and
  `connectStream` sends `{type:'join', name, token}` (line 884). The relay ignores
  these tokens (it uses the already-authenticated `client.userId`), so the token is
  redundant exposure. If WS traffic is ever logged or forwarded, tokens leak.
  `stream_proxy.py` sends `{type:'join', name}` to the relay WITHOUT a token
  (line 169) — inconsistent with the browser, and that stream→relay link is
  unauthenticated.
- **No WSS/TLS wired.** `.env.example` documents `WSS_PORT=8443`, but `server.js`
  only creates an `http` server on `RELAY_PORT` (default 8080, `ws://`). JWTs and
  credentials travel in plaintext unless an external TLS terminator is placed in
  front. `RELAY_URL` defaults to `ws://localhost:8080`.

Gap: move browser session storage to HttpOnly cookies (or keep localStorage but add
XSS mitigations / CSP), stop sending tokens in WS message bodies after `auth`,
authenticate the stream_proxy→relay link, and terminate TLS at or before the relay
for any non-local deployment.

---

## 3. Authorization gaps

- **Cloud saves / career are user-scoped but not tied to a specific game session.**
  `/api/saves`, `/api/career`, `/api/car-setups` are all `authMiddleware`-protected
  and filtered by `user_id`, so cross-user access is prevented. Good.
- **Save load is not acknowledged by the game.** `loadSave` shows a success
  notification before sending `load_save` to the stream proxy, and the stream
  proxy has no `load_save` handler and no native save import/acknowledgment. HTTP
  200 means "database write succeeded," not "the game loaded the save." This is a
  correctness/integrity gap, not an authentication bypass.
- **No per-account original save/config bytes import** (from the review notes):
  there is no path that imports/acknowledges original-format MC3 career/save files.

---

## 4. Rate limiting / quotas

- **Per-IP quotas are shared behind NAT.** `TransportSecurity` applies connect,
  message, byte, and room-creation buckets per direct peer IP. Two players behind
  the same NAT share ONE IP, so one peer's traffic consumes the other's quota
  (connect burst 8 @ 0.5/s; messages 120/s burst 240; room creates burst 2 @ 1/30s).
  This is a deliberate anti-abuse control but will couple two legitimate peers on
  the same LAN/NAT.
- **Room-creation quota is very tight.** `creates` bucket = burst 2, refill 1 per
  30 seconds per IP. Two peers behind one NAT can create at most ~2 rooms in a
  30-second window together.
- **No rate limiting on stream ports.** `stream_proxy.py` (UDP 8765 / WS 8766) and
  `proxy.py` (UDP 30741) have no connection quotas, message validation, or byte
  limits. The relay's `TransportSecurity` does not protect them.

---

## 5. Input validation / message handling

- **Binary frames arrive as non-binary via the ws client.** Empirically, sending a
  `Buffer` from the `ws` client to the relay results in `isBinary=false` at the
  server, so binary is rejected by the JSON parser (close 1007) rather than the
  explicit binary check (close 1003). The security goal (binary is rejected, never
  processed as a relay message) is still met, but the explicit 1003 path is dead
  code for browser/game-proxy clients.
- **`stream_proxy.py` drops payloads.** `udp_reader` calls `continue` after learning
  the game port from a header datagram, dropping any payload carried in the same
  datagram (review note #5). Native sends combined header+payload.

---

## 6. Configuration / deployment

- **`.env.example` now present** (added during this workstream) and documents
  `RELAY_PORT`, `WSS_PORT`, `NODE_ENV`, `JWT_SECRET`, `CORS_ORIGIN`,
  `DATABASE_PATH`, `MAX_PLAYERS`, `GAME_TIMEOUT`.
- **`DATABASE_PATH` vs `DATA_DIR` mismatch.** `docker-compose.yml` passes
  `DATABASE_PATH=/data/mc3_relay.db`, but `server.js` reads `DATA_DIR` (default
  `./data`) and never reads `DATABASE_PATH`. The compose env var is a dead key;
  data lands in the container's `./data` unless `DATA_DIR` is also set.
- **`JWT_SECRET` documented as empty** in `.env.example`. `server.js` auto-generates
  and persists a random key when unset, but an operator may not expect a key to be
  written to disk, and the empty default is confusing.

---

## 7. What is NOT a gap (verified by tests)

- TransportSecurity is wired into the WS connection handler (not just unit-tested).
- Passwords use `scrypt` with `timingSafeEqual`; `pbkdf2` fallback upgrades legacy
  hashes on successful sign-in.
- JWTs are HS256 with 30-minute expiry; `JWT_SHORT` (<32 chars) is rejected.
- Security headers set: `X-Content-Type-Options: nosniff`,
  `X-Frame-Options: DENY`, `Referrer-Policy: no-referrer`; `x-powered-by` disabled.
- CORS origin validated; direct peer address used for rate limits
  (`trust proxy: false`); `X-Forwarded-For` ignored.
- Cookies: `HttpOnly`, `SameSite=Lax`, `Secure` in production.
- Cloud files: canonical base64, `sha256` digest verification, size caps,
  atomic JSON writes, per-account files.
- No tokens are logged by the relay (`reject` sends close codes/reasons only;
  the catch handler logs a static string without message content).
