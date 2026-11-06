# Scaling mc3.top to thousands of players (5 Nov 2026)

## How Underground 2 Revived holds 13,000+ players (github.com/th0mk/nfsu2-web, web/server-rs, web/deploy)

- **One process, many cores**: a Rust (tokio) server on one VPS, workers = cores/2 (2-8), compression on its own
  pool, the free-roam hub and the online host each on a thread, mimalloc. Measured by its authors: 2,000 players in free
  roam on ~3-4 cores; the hub's interest pass for 12,000 players takes ~0.46 s of each second; ~0.5 GB RAM per 1,000
  players (4.9 GB at 13,163 players, 5 Nov 2026). A 16-vCPU VPS behind Caddy, optionally Cloudflare in front.
- **One WebSocket per player, binary frames**: a virtual LAN (10.77.x.x per player) with compact binary headers
  (kind, src/dst address and port, time); JSON only for control (hello, queue, ping).
- **Traffic stays where it is needed**: broadcasts are charged per recipient and delivered only to peers in the same
  room that have bound that UDP port; race traffic is a star around the race's host.
- **Interest management** in free roam: each player is linked to the nearest 32 players (48/64 when the device has
  frames to spare, 16 on phones) measured at predicted positions; friends, crew and race-lobby members first; links held
  for a while; the cap drops by a quarter when the hub thread is busy.
- **A hard cap and a queue**: `--max-peers` (2,000 on the systemd unit, up to 16,000); when full, a FIFO queue tells
  each waiter its place and an estimated wait; silent waiters dropped after a minute, a dropped waiter keeps its place
  briefly. `/players` publishes the counts.
- **Per-connection limits**: token buckets per connection, client and site; capped queues toward lagging players.
- **Game data** from the same server in compressed 1 MiB blocks (we do not serve game data: players use their own disc).

## Where mc3.top is today

One Cloudflare Durable Object (`World`, tools/multiplayer/cloudflare/src/world.js) holds every connection, every room
and the social state. The Main Lobby has no cap and relays every System Link datagram (JSON + base64) to every game
client in it: traffic grows with the square of the players in the lobby, and one Durable Object is one thread.

## Done (5 Nov 2026, deployed)

- Main Lobby instances of 32 ("Main Lobby", "Main Lobby 2", ...; the fullest with room first; empty ones removed).
- Player cap (MAX_PLAYERS, default 500) with a queue: a sign-in past the cap waits, gets `{type:'queue',pos,of}` and its
  `auth_ok` when a place frees (old clients just wait; the game's lobby window shows the place). `/api/players`.
- The relay no longer scans every connection per datagram: an in-memory index of sockets and room members, rebuilt
  after hibernation; datagrams encoded once per room; rate budgets written to the attachment at most once a second;
  room list broadcasts batched to once a second.
- Measured locally (`load_test.mjs` against `wrangler dev`): one World object relays about 20,000 copies a second
  whatever the mix (one client or two, 1 or 5 sends a second) — the single thread is the limit. Real lobbies
  (one discovery broadcast a second each) at 32 per instance: 500 players = ~15,500 copies a second. Beyond that the
  rooms must leave the World object (step 2 below).

## Plan, in order

1. **Main Lobby instances**: the Main Lobby becomes "Main Lobby 1, 2, ..." of at most 64 players each (System Link
   races are 8 at most); a new player goes to the fullest instance with room, so lobbies feel busy. Same protocol;
   the in-game Online Lobbies list shows the instances.
2. **A Durable Object per room**: the `World` keeps accounts, presence, clubs and rankings and hands out rooms; each
   room (and each Main Lobby instance) is its own Durable Object, so rooms run on separate threads and machines.
   The client's WebSocket moves to the room object when it enters one.
3. **Binary datagrams**: `input` frames as binary WebSocket messages (12-byte header + payload) instead of JSON with
   base64: about a third less bandwidth and no JSON work per packet. Both the built-in link (core/relay_link.cpp) and
   proxy.py speak it; JSON stays for control.
4. **Deliver only where bound**: the link reports the game's bound System Link ports; broadcasts go only to peers that
   bound that port, unicast stays unicast.
5. **Cap and queue**: a total player cap with a FIFO queue, place and estimate shown in the lobby window and on the
   site; `/api/players` with live counts for the site and the game.
6. **Rate limits per connection and per room** (already partly in place: MSG_RATE / BYTE_RATE per socket).
7. **Free roam (Cruise) with nearby players**: the per-room interest pass (nearest N by predicted position), once the
   Cruise player cap work (docs/notes/cruise_player_cap_resize_plan.md) allows more than 8 cars.
