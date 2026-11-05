# MC3 online world server

Players run the game on their own PCs (CPU, GPU, disc). This server only carries the online world: the Main Lobby,
named rooms, car clubs, club chat, rankings and accounts. Race traffic between players goes through it as small
datagrams, so a free-tier cloud machine is plenty. It needs no game files.

## Set it up once

1. **Domain.** Register one (for example `mc3.top`, about $2 the first year and $5 a year after). In its DNS settings
   add an `A` record for `@` pointing at the server's public IP (step 2 shows it).
2. **Server.** Create an Ubuntu 24.04 machine at any cloud provider. Oracle Cloud's *Always Free* Ampere A1 is enough
   and costs nothing. Allow inbound TCP 80 and 443 (and UDP 443) in the provider's firewall or security list.
   Paste [`cloud-init.yaml`](cloud-init.yaml) as its *user data*, after setting `MC3_DOMAIN` and `MC3_EMAIL` in it.
3. **Publish a version.** Mark the commit the server should run:

   ```bash
   git tag -f relay-live && git push -f github relay-live
   ```

About five minutes after the machine boots, `https://<your domain>/` answers and `wss://<your domain>/ws` is the
relay.

## What it does on its own

- Containers restart after a crash and come back after a reboot.
- Caddy gets and renews the HTTPS certificate.
- Ubuntu installs security updates nightly and reboots at 05:30 UTC when one needs it.
- Accounts, clubs and rankings are archived every night to `/opt/mc3/tools/multiplayer/deploy/backups` (last 14).
- Every hour it checks the `relay-live` tag. It rebuilds only when you've moved the tag, so pushing work in progress
  to `main` never reaches players.

## Players

```powershell
tools/multiplayer/Launch-Multiplayer.ps1 -Server wss://mc3.top/ws            # the Main Lobby
tools/multiplayer/Launch-Multiplayer.ps1 -Server wss://mc3.top/ws -Room Friday
```

Or set `MC3_SERVER=wss://mc3.top/ws` once and leave `-Server` off.

## Run it by hand (any Linux box with Docker)

```bash
cd tools/multiplayer/deploy
MC3_DOMAIN=mc3.top MC3_EMAIL=you@example.com docker compose up -d --build
```
