# Online lobbies in place of Xbox Live (2026-11-05)

Goal: online play that needs nothing but the game. Choose **Networking > Online Lobbies** and you are in the Main Lobby
with everyone else on mc3.top; pick a room, then race with the game's own System Link screens.

## What the reference project does (Underground 2 Revived, nfsu2.xyz)

The user pointed at "nfs.xyz" / "nfsmod.xyz": nfs.xyz is a parked domain for sale and nfsmod.xyz does not resolve.
The project meant is Underground 2 Revived at nfsu2.xyz (a browser port of NFS Underground 2 built from the
NFSU2Recomp static recompiler, source github.com/th0mk/nfsu2-web). From its pages and press coverage (5 Nov 2026):

- **Setup**: open the site, press Play; nothing installed, no EA account, about 7 MB of code first, game data streamed
  (optional ~2 GB preload). Chrome/Firefox with WebGPU, HTTPS. Saves in browser storage; optional cloud saves keyed by
  a passphrase, no account.
- **Online** reuses the game's own Online menu: *Online > Lobby lists games > Create Game / Refresh and Join Game >
  Ready > host presses Start Race*, up to 6 racers. Underneath is one virtual LAN served by the project's server
  (players get virtual addresses), so the original networking code runs unchanged. Optional free roam pairs you with the
  32 nearest players. A queue shows your place when the server is full. Names from settings (a keyphrase can claim one),
  quick chat, friends, crews, block list. Live counters at /players (about 8,000 players when read).
- **Game data**: the site serves the game files to everyone. That is the part we do not copy: mc3.top never hosts the
  MC3 disc or its files; each player uses their own copy.

Ideas taken: reuse the game's own network screens over a server-run virtual LAN; zero setup (no launcher, no extra
program); one public lobby by default with rooms on top; names without accounts; live player counts.

## How MC3 does it

- **Menu**: the Networking page (built at `0x180220`) has two entries. "Internet_Play" (callback `0x180160`) starts the
  Xbox Live sign-in (account list at `0x17FB4A`, then page `0x1F`); "LAN_Play" (callback `0x1801C0`) sets network mode
  `[0x50F14C] = 1`, starts System Link (`0x1B0AD0`) and opens page `0x20` (Create Match / Join Game / Enter Nickname).
  `templates/game/src/game/online.cpp` overrides `0x180160`: connect the built-in link, open the lobby window, then run
  the LAN body. `hooks.cpp` relabels the entry "Online Lobbies" (UTF-16 string copy). `[online] enabled = false` in
  settings.toml restores the original.
- **Built-in link**: `runtime/core/relay_link.cpp` is `tools/multiplayer/proxy/proxy.py` inside mc3.exe (WinHTTP
  WebSocket, wss by default). It listens where `xnet.cpp` already sends relay traffic (127.0.0.1, 30741 + instance),
  joins the server as a guest under the player's name, enters the Main Lobby / a hosted room / a private room, and
  carries each datagram as an `input` envelope; remote players appear at 127.0.1.<peer>. If proxy.py already holds the
  port, the link stands aside. Reconnects with backoff.
- **Lobby window**: `runtime/app/lobby_menu.cpp`, an ImGui overlay like the settings menu (game input frozen while open,
  controller or mouse): status, your name (saved, reconnects at once), the listed lobbies with player counts and hosts,
  host a public room, enter a private room by name, Race.
- **Settings**: `[online]` in settings.toml: `enabled`, `relay` (`wss://mc3.top/ws`), `name`, `room`.

Verified in the game (instance 5): Networking shows "Online Lobbies"; choosing it connected to mc3.top, joined the Main
Lobby ("relay: in the Main Lobby (1 here)"), showed the window over the game's Create Match / Join Game page.

## Next

- A race between two machines through the built-in link (two instances on one PC first, then two PCs).
- Sign in with the mc3.top account from the window (a guest using a registered name is shown as "name (guest)"), so
  points and the creator badge follow the player.
- Lobby chat and the online count on the main menu; free roam (Cruise) with other players, as Underground 2 Revived
  does; a server-side queue when full.
- The same lobby flow in the browser build (docs/notes/browser_port_plan.md step 6).
