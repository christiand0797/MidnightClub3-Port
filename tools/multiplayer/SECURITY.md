# Online security and deployment boundaries

## Node relay and paired stream controls

These account/storage controls apply to the Node service. The Cloudflare Worker
is currently guest-only; it does not implement account or cloud-save APIs. Its
Durable Object limits are tested separately in `cloudflare/world_security.test.cjs`.

| Boundary | Current behavior |
| --- | --- |
| Passwords | Unique salted scrypt; successful legacy PBKDF2 migration. Two concurrent password operations and eight queued requests. |
| Sessions | Random device tokens stored by digest; HttpOnly/SameSite cookies, Secure in production. Expiring JWTs must name a still-active device session. Logout revokes that session's sockets. |
| Browser requests | Same-origin checks for cookie authentication/account mutations. Invalid bearer credentials never fall back to a cookie. Exact `PUBLIC_ORIGIN` for TLS proxies. |
| Cloud upload | Authentication before the 24 MiB JSON parser; four active requests globally, one per account, 30-second deadline. Completion, abort and close release counters. |
| Cloud files | 256 files/16 MiB plus 128 KiB preferences; strict paths, encoding and SHA-256. Revision compare-and-swap, retained history and atomic file replacement. |
| Storage | 10,000 users; 4,096 sessions/32 per user; 128 history files and 256 MiB per account; 2 GiB/32,768 cloud files globally. Full storage rejects writes, preserving saves. |
| HTTP | 512 connections, 64 headers, 15-second header timeout, 30-second request/socket timeouts, 5-second keepalive. |
| Relay | 512 sockets, 16 per direct IP, four per account; authentication deadlines, bounded messages/bytes, room creation/membership and outgoing queues. |
| Host stream | Independent pairing, exact origins, bounded pending/paired viewers and view-only default. Actual executable path, creation identity and instance select the window; each capture rechecks identity. |
| Launcher | No elevation or game lifecycle changes; retained owned child handles; occupied ports rejected. |

The Main Lobby has no separate player cap, but server budgets apply. Named rooms
are unlisted, not access-controlled. File digests detect corruption; they do not
prove save validity, fair progression or acceptance by the original game loader.

## Operating the service

- Expose HTTPS/WSS through a TLS proxy. Keep the direct port private and configure
  exact `PUBLIC_ORIGIN`; preserve Host/Origin validation.
- Without `RELAY_TRUST_PROXY=1`, clients behind one proxy share its direct-IP
  budget. The deployment option trusts private/loopback proxy peers; expose it
  only behind the configured proxy on an isolated backend network. Review that
  trust boundary before deployment; private-network clients must not be able to
  bypass the proxy and supply their own forwarded address.
- Run one writer per `DATA_DIR`. Protect data/backups with OS access controls and
  encrypted storage. The JSON store is not end-to-end encrypted; administrators
  with disk access can read saves/configs.
- Back up data and signing keys consistently while stopped or through an
  application-consistent snapshot. Test restoration privately. Do not delete
  data volumes as an update procedure.
- Review/test locked dependency updates, rotate compromised keys/sessions and
  monitor quotas, errors and disk space. Keep passwords, request bodies, pairing
  links and authorization headers out of logs.
- Public deployment still requires measured load/memory limits, upstream traffic
  protection, monitoring, recovery drills and account recovery/device-management
  flows. Local tests do not establish denial-of-service immunity.

Automatic native restore, original-loader acceptance receipts and authoritative
shared career progression remain separate work. Do not write guessed economy
fields into guest memory or accept browser load acknowledgements as native proof.
