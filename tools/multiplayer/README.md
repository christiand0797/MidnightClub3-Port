# MC3 online tools

The native game uses its original **Networking → Xbox System Link** menus through
the relay tunnel. The browser supplies accounts, cloud file storage, rooms and an
optional paired view/controller connection to a Windows host. It does not yet
run the complete game locally in WebAssembly/WebGPU.

The `mc3.top` Cloudflare deployment provides the relay, accounts (register, sign
in, device sessions, sign out), cloud save/config snapshots and car clubs, in the
World Durable Object's SQLite (`cloudflare/src/accounts.js`). Its HTTP contract
matches the Node server's, so the account page and launcher work unchanged.
Test it with `MC3_WORLD=https://mc3.top node cloudflare/accounts_live.test.mjs`.

## Join from a native installation

Use the same explicit instance as the game you started. Instances 0 and 1 share
the original `hdd`; instances 2–8 use `hddN`. The launcher never starts, closes or
rebuilds the game and does not require administrator access.

By default the launcher connects to `wss://mc3.top/ws` (override with `-Server`
or `MC3_SERVER`), which does not need local Node:

```powershell
& 'C:/Users/purpl/AppData/Local/Programs/Python/Python312/python.exe' -m pip install 'websockets>=14,<18'
powershell -NoProfile -File tools/multiplayer/Launch-Multiplayer.ps1 -Instance 7
```

Race points and rankings go to your account once this PC is signed in: add `-SignIn` (or
`-Register` for a new account) once. It asks for your name and password, keeps a 30-day device
sign-in (never the password) and signs the tunnel in on every start. `-SignOut` forgets it,
`-Guest` races as a guest. `proxy/proxy.py --relay wss://mc3.top/ws --delete-account`
permanently deletes the account (needs its password).

For a local relay (`-Local`), install Node.js 24 LTS or 26, then install the
locked server dependencies once:

```powershell
Push-Location tools/multiplayer/relay
npm ci --omit=dev --ignore-scripts
Pop-Location
powershell -NoProfile -File tools/multiplayer/Launch-Multiplayer.ps1 -Instance 7 -Local
```

Remote plain `ws://` is rejected.
Without `-Room`, clients join the Main Lobby. Named rooms are unlisted and shared
by matching names; the name is **not an access-control password**. Original
matches retain their player limits and server traffic/connection budgets apply.

Select Create Match or Join Game in the game's System Link menu. The launcher
opens the account page over HTTP on loopback or HTTPS at the selected host.
Do not open `browser/index.html` as a `file:` URL. `-PlanOnly` prints routing
without starting services. Ctrl+C stops only children started by that launcher;
an existing healthy relay is reused. Occupied tunnel/stream ports are rejected.

## Optional browser view and controllers

```powershell
& 'C:/Users/purpl/AppData/Local/Programs/Python/Python312/python.exe' -m pip install -r tools/multiplayer/stream/requirements.txt
powershell -NoProfile -File tools/multiplayer/Launch-Multiplayer.ps1 -Instance 7 -Local -StartStream
```

This explicitly shares the selected MC3 game window. A fresh host pairing secret
is passed in the browser URL fragment, removed from the URL and kept in memory.
Account tokens cannot pair to a host. The stream listens on loopback and accepts
only configured browser origins. No game window means no stream; there is no
automatic desktop fallback. For another installation pass `-GameExecutable` with
the exact installed `mc3.exe` path.

Viewing is the default. Add `-AllowControl` when the host wants additional
controller ports enabled. Disconnects and slow consumers release their leases.
The controller path uses native host IPC, not physical keyboard/mouse injection.
Keep pairing links private. Remote streaming needs an independently configured
authenticated TLS route; the launcher does not expose a local stream publicly
or bypass mixed-content rules.

Current streaming uses lossless PNG with bounded buffering. Framed H.264 and
low-latency hardware encoding remain unfinished. Raw Annex-B chunks are not
complete WebCodecs/MediaSource frames. The capability panel reports available
APIs; it does not establish native-quality browser rendering. Isolated Firefox
and Edge account tests use software/headless profiles, not GPU acceptance.

## Accounts, saves and preferences

Each account owns its cloud snapshot/history. Login and signup include password
visibility. Remember-device sessions use HttpOnly cookies; short-lived access
tokens remain in memory. Logout revokes the current device and its relay sockets
without erasing cloud saves.

Selected original save/config bytes are stored unchanged with SHA-256 and
account-scoped revisions. Conflicts retain a local draft rather than silently
overwriting another device. Each snapshot allows 256 files/16 MiB plus 128 KiB
structured preferences. On shared devices leave remember-device disabled and
sign out afterwards.

Cloud downloads and the [offline snapshot collector](CLOUD_SNAPSHOT.md) transfer
files/proposals. **Native automatic cloud import/load is not implemented.** A
future restore must preserve rollback copies and use the original save loader
and signature checks. A browser acknowledgement cannot prove native acceptance.
Online career metadata is not authoritative competitive progression.

## Services and deployment

```text
Native game ⇄ MC3R UDP tunnel proxy ⇄ room relay ⇄ other native proxies
Browser account/rooms ⇄ same-origin HTTPS/WSS relay + account-scoped storage
Selected native window → paired loopback PNG stream → browser canvas
Paired browser controller → host IPC → additional native controller port
```

The relay serves browser files itself. Preserve `DATA_DIR` across updates. A
blank `JWT_SECRET` creates a random private signing key there; keep accounts and
keys out of Git and release images. Set `PUBLIC_ORIGIN` to the exact public HTTPS
origin behind a TLS proxy. Run one writer per data directory.

Compose builds from this directory, runs as a non-root user with a read-only
filesystem, persists `/data`, and publishes its port only on loopback. Configure
public TLS and traffic controls separately. Docker is unavailable in the current
development environment, so this image/Compose deployment has not been executed.
See [security and deployment boundaries](SECURITY.md).

## Validation

```powershell
Push-Location tools/multiplayer/relay
npm test
Pop-Location
& 'C:/Users/purpl/AppData/Local/Programs/Python/Python312/python.exe' -I -m unittest discover -s tools/multiplayer/tests -p 'test_*.py' -v
powershell -NoProfile -File tools/multiplayer/tests/Test-Launcher.ps1
```

Tests use synthetic accounts/files and isolated servers. Launcher tests fake
child processes; capture tests inject Win32 APIs. No personal save or active game
is needed. Foreign commit `f6301e6` reports a completed two-copy relay-only
System Link race; source tests do not extend that report to every browser,
controller, career or graphics path.
