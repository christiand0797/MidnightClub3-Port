# Career, save completion and original network trace

Static research, **2026-11-04**, original USA Remix XBE SHA-256
`0b3d81a02834e57864a601d473d5296cc0a2463e4b837373d91030f751fa51a3`.
No game was launched or memory/save file modified. Address citations refer to
the original instruction comments in the generated partitions. Names such as
"reward observation" describe the bounded behavior traced here, not an invented
whole-game class/API.

## Career/profile ownership

| Item | Original evidence | Scope |
| --- | --- | --- |
| Career manager | `[0x557F0C]`, vtable `0x473B98`; construction `0x27816E/0x2781A5` | Runtime singleton, not a persistent account ID. |
| Career data | manager `+0x0C`, vtable `0x474288`, size `0xCC8` | Money at `+0xACC`; current city at `+0x50`. |
| Profile image | global `0x53F308`; original copy `0x26BD10` | Raw profile image size `0x18340`. |
| Owned roster | profile `+0x504` count byte, `+0x505` selection byte, first CarCfg `+0x508`, stride `0x108`, max 30 | CarCfg vtable `0x4483DC`; model string `+0xE3`, 22-byte bounded field. |
| Roster compaction | `0x26BB80 -> 0x271E20` copies remaining configs | No original saved unique-car ID has been proved. |

Current `templates/game/src/game/tuning_profiles.cpp` observes actual config
copy/reset/compaction lineage and stores PC identities in
`config/tuning_profiles.toml`. It does not replace the original save writer.
The sidecar's identity is tied to the matching career/roster, as documented in
[per-owned-car tuning](per_car_tuning.md).

Host routing is explicit: `runtime/app/main.cpp:386-389` boots with `ExeDir/hdd`
or `hddN` when instance > 1. `runtime/kernel/files.cpp:MountDefaults` maps Xbox
Harddisk0 partitions 1/2/3/4/5 to the selected root's E/C/X/Y/Z. A profile name
alone therefore does not identify a particular installation or instance.

## Save buffer is not completed disk output

The existing per-car hook at `sub_0026BD10` recognizes original return addresses
`0x1A5A4` and `0x1A6CE` when the source is the global profile. These are useful
normal-save-flow observations, but they mean **profile bytes were copied into a
save buffer**:

1. `sub_0001A550` (`x86_recomp.1.cpp`) writes its outer header at object `+0x10`,
   copies profile `0x53F308` into object `+0x14` at `0x1A594..0x1A5A4`, and
   prepares career data beginning at object `+0x18358`.
2. The same function clears manager dirty byte `+0x20` at `0x1A62D` and marks
   its own preparation state `+0x3355C = 1` at `0x1A631`. Neither is a host disk
   flush acknowledgment.
3. `sub_000193F0` emits the outer file spans. Four header bytes, `0x18340`
   profile bytes, one progress byte and one city byte precede the career blob;
   the career blob begins at disk offset `0x18346`, while its object offset is
   `+0x18358`. Treating an in-memory offset as a file offset would be wrong.
4. The outer writer subsequently emits other spans and a trailing 20-byte
   signature. The bounded saved-money parser previously proved only the prefix
   through Money, not that tail/signature.

The independently preserved schema/parser is under the external R3 proof:
`G:/MC3_Backups/release-work/corrected-content-0.3.3-dev.20261026-r1/feature-native-qa-20261028-r3/economy-disk-proof-r1/`.
Its `schema.md` records field order and alignment from original reader/writer
pairs. Parser SHA-256:
`dc431bd69651c0c676b08454524b7e59d6f00d5369d26700a9022fedbd59a461`.
The original stream uses `XJ97`, flags 0, values without field-name strings;
alignment is relative to blob base, not the unaligned file start.

At the host boundary, `runtime/kernel/files.cpp:Transfer` invokes buffered
`fwrite`, updates the file position and completes the guest I/O status/event/APC
before returning. `NtWriteFile` (export 236) and gather (237) share it. This
completes that transfer, rather than an entire multi-file career transaction.
`NtFlushBuffersFile` (198) calls `fflush`, but its current implementation reports
success without testing `fflush`'s result; a later `File` destructor calls
`fclose`. No durable complete-career cloud snapshot acknowledgment exists in
these hooks today.

For the current cloud-account work, an exact opaque file copy after the native
process has closed is a simpler established observation boundary than the
profile-copy hook. Stable before/read/after hashes can identify the copied
bytes. It still does not authenticate the original console signature or confer
permission to merge a different career. A future live snapshot would have to
account for file-handle completion, all related files, the original save state
machine and concurrent changes; none is established by an HTTP response.

## Race completion and money observation

A newly traced bounded path exists in `x86_recomp.50.cpp` and `.52.cpp`:

```text
sub_001C2B00
  [0x533E24]+4 -> race-like object used as ECX
  original mode guard [0x557ED0]+4 != 3
  0x1C2B34 -> sub_001CFC50
    guard object+0x0E == 0, global game state == 2, mode != 3
    sub_001CF440
    career manager+0x14 -> secondary/current-race data
    0x1CFCBA -> sub_00285B70(position-like argument, selected record+0xAC)
    0x1CFCC2 -> sub_002843E0
    0x1CFCC9 -> sub_002840E0 => secondary/current-race data+0xBC
    0x1CFCD5..0x1CFCD7 adds that returned integer to career data+0xACC
    object+0x0E = 1 at 0x1CFCFD
```

These are actual instructions: `sub_002840E0` is a two-instruction getter for
`[ecx+0xBC]`. The added amount is not inferred from a displayed wallet delta.
`sub_00285B70` uses state 2 and `secondary+0x4C -> sub_0027F030`; it computes a
value via `0x285B20`, converts it with `0x2DAE60`, stores secondary `+0xBC` and,
when nonzero, sets career manager dirty byte `+0x20` at `0x285BC6`.

The local object's `+0x0E` prevents this path applying twice for that object.
Its persistent identity, every race type and the meaning of all record fields
remain open. The `0xB8` selected-record stride and helper `0x2694D0` are observed;
calling the latter a universal network-player index would exceed the evidence.

`ResultsScreen` is original string `0x45D72C`, pushed by `sub_00164EB0` and
`sub_001650E0` (`x86_recomp.39.cpp`). That is a presentation boundary, not an
authoritative reward event. Likewise, `SavingScreen` string `0x45B910` is used
by several original menus, including garage update `sub_00152420` at
`0x152DA8`; seeing its name alone does not prove a completed disk save.

A read-only observer around the original `0x1CFC50` could record before/after
money and once flag in this scope without replacing reward logic. That is a
candidate integration point, not an implemented observer or validated
shared-career reward policy. Loss/retry, tournaments, hookmen, network results,
replays and every other cash writer still need separate classification.

## Original Network menu and protocol concepts

Original strings/data include:

| Address | Value/source relationship |
| --- | --- |
| `0x45BB44` | `MenuNetworkHostJoin`; allocation size `0x1E8` and constructor `0x16CC20` are called at `0x1581C2..0x1581E1`. |
| `0x45BAF8` | `MenuNetworkHostSelect`; a separate menu instance is built by the following original setup. |
| `0x45E6B4` | `mcNetworkHostJoin`, returned by `sub_0016CC60`. |
| `0x45E6C8` | Host/join menu vtable installed by constructor at `0x16CC47`. |

The handwritten host XNet layer is `runtime/xdk/xnet.cpp`. Original guest API
entry points are fixed in `templates/game/config/game.toml`: `recvfrom`
`0x3CC0BE`, `sendto` `0x3CC0DC`, `WSASendTo` `0x3CC0EB`, `select` `0x3CC086`,
`XNetXnAddrToInAddr` `0x3CBFBB`. Port code emulates an `XNADDR` as 36 bytes:
IPv4 at +0, online IPv4 +4, online port +8, Ethernet bytes +0x0A and online
bytes +0x10. Host secure-address handling currently maps peers to IPv4 and
retains original XNADDR bytes in a peer table; this is port behavior, not Xbox
Live authentication/encryption.

### Original network message-name table

`sub_001AFBB0` (`x86_recomp.48.cpp`) takes an integer argument, subtracts
`0x40`, range-checks through `0x28`, and dispatches through original jump table
`0x1AFD10`. The following IDs were read from that table and each branch's string
pointer in the pinned XBE. They are **name mappings**, not yet packet offsets,
lengths or authorization rules.

| ID | Original name | ID | Original name |
| --- | --- | --- | --- |
| `0x40` | INPUT | `0x41` | PLAYER |
| `0x42` | PLAYER_AND_INPUT | `0x43` | RACE_CONFIG |
| `0x44` | MCNET_PLAYER_CAR_CONFIG | `0x45` | PAUSE_SIMULATION |
| `0x46` | RESUME_SIMULATION | `0x47` | LOAD_RACE |
| `0x48` | RESET | `0x49` | GAME_STATE |
| `0x4A` | RACE_STATE | `0x4B` | PLAYER_CONFIG |
| `0x4C` | CHECKPOINT | `0x4D` | default/unnamed branch |
| `0x4E` | RESET_FLAG | `0x4F` | PICKUP_FLAG |
| `0x50` | RETURN_FLAG | `0x51` | DROP_FLAG |
| `0x52` | SWAP_FLAG | `0x53` | SHOOT_PLAYER |
| `0x54` | SWAP_AND_TRIGGER | `0x55` | MCNET_MISSILE_ACTION |
| `0x56` | PLAYER_DAMAGE | `0x57` | PICKUP |
| `0x58` | CHAT | `0x59` | CHAT_STATUS |
| `0x5A` | CUSTOM_RACE | `0x5B` | START_COUNTDOWN |
| `0x5C` | STOP_COUNTDOWN | `0x5D` | VOICE |
| `0x5E` | VOICE_CONFIG (coloured diagnostic string) | `0x5F` | HOST_INFO |
| `0x60` | UPDATE_HOST_INFO_ACK | `0x61` | TEAM_REQUEST |
| `0x62` | MCNET_CLAN_INFO | `0x63` | MCNET_NETWORK_AVAILABLE |
| `0x64` | OBSERVER_REQUEST | `0x65` | MCNET_CLAN_REQUEST |
| `0x66` | MCNET_PLAYER_SCORE | `0x67` | MCNET_MIGRATE_HOST_INDEX |
| `0x68` | MCNET_HOST_DONE_WAITING_FOR_PEERS | | |

For example, ID `0x44` selects branch `0x1AFBE7` returning string `0x4669AC`;
`0x63` selects `0x1AFCC7` returning `0x466824`. This establishes existing
original configuration/input/score/host concepts. It does not establish that a
new relay's JSON events share their wire representation or that campaign
progression is already networked.

## Car construction and remote-actor scope

`sub_002CBC40` is a complete virtual factory build, rather than just an allocator.
It invokes several virtual slots, constructs/registers components and visits
additional object registries. The human component hook is `0x2CBDE0`, reached
through factory vtable `0x476940`; its car pointer lives at factory `+0x44`.
The AI factory uses vtable `0x46C730` and creator `0x1FAE10`. Existing validated
ownership details are in [vehicle ownership](vehicle_ownership.md):

- Car `+0x04` is a controller; human vtable `0x47684C`.
- Human controller `+0x50` points back to the same car.
- Car `+0x50` is vehSim, `+0x64` is CarCfg.
- Human factory writes car byte `+0x5E = 0`; AI writes 1.
- Car `+0x14` is damage, not input; `+0x18` is feedback/rumble.

The original street path constructs a factory on its guest stack at
`0x2C004C..0x2C005E`, which is why heap-only address guards once missed valid
cars. `garage.cpp` now observes committed stack factories and validates concrete
controller/backpointer ownership. These hooks do not provide a remote actor
spawner.

Original network names PLAYER / PLAYER_AND_INPUT / PLAYER_CONFIG and the
complete factory show promising existing machinery. The receiver-to-factory
chain, remote controller vtable, actor lifetime, city streaming membership,
collision authority and net interpolation have not yet been established here.
Writing a received transform into an arbitrary local car would not prove a
shared-career remote actor. The current replay transform correction is also a
separate playback-only feature, not a network replication implementation.

## Research status

The broad [knowledge index](reverse_engineering_index.md) links already completed
subsystem notes. This trace adds exact original message-name IDs and a bounded
result-to-money caller chain. The next unknowns are the original message payload
serializers/dispatchers, the remote car/controller constructor chain and actual
complete save transaction/load acknowledgments. Current browser adapter facts
are separately captured in [the integration snapshot](network_integration_review_20261104.md).
