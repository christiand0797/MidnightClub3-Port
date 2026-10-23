# Collision bound ownership: vehicle resource instances

2026-10-23. Read-only original-source and asset analysis. No production collision change, game launch, memory
write, native game build or same-model live playtest was performed for this note. The existing three-car street
snapshot identifies different models and cannot by itself establish same-model isolation. See the candidate
correction and contact conditions in [ground contact audit](ground_contact_audit.md).

## Source result

The ordinary human-driving path and the specific AI construction flow at `0x1FB040` request **private vehicle
resource loads**. Their source path selects a fresh bundle allocation, with its own bundled bound, rather than
cloning only the bound after physics construction. The garage-preview path explicitly permits resource sharing;
other observed nonplayer cars also use shared-key entries. This is a source-backed lifetime distinction, not a
completed same-model live-pair test. A correction must verify the current cache entry before writing a bound.

The resource manager is `M = u32[0x50A08C]`. Its cache begins **at M itself**, with **16 entries of 0x10 bytes**:

| Entry field | Type | Meaning and original use |
|---|---|---|
| `+0x00` | u32 | Model ID from `0x26A6A0`; `0xFFFFFFFF` denotes a free slot |
| `+0x04` | pointer | Loaded **vehicle resource root**; this matches `car+0x4C` |
| `+0x08` | u8 | Sharing/cache key, the fifth argument of `0x105B80` |
| `+0x09` | u8 | Resource-user reference count; increment/decrement instructions are byte-sized |
| `+0x0A` | u8 | Pending asynchronous-load/promotion flag |
| `+0x0C` | pointer | Optional asynchronous request packet, not a collision shape |

At `0x105BA0..0x105BB1`, `0x105B80` compares the model ID and sharing key. A matching non-pending entry is
reused at `0x105C2A` **only if that key is nonzero**. With key zero, matching live entries are skipped; the scan
continues and then selects a free slot at `0x105BBC..0x105C41`. A pending entry instead enters the promotion path
at `0x105BD6`, so it must not be treated as an ordinary private loaded entry during a mutation.

The actual caller values are:

| Construction path | Call site | Fifth argument / result |
|---|---|---|
| Human driving, factory vtable `0x476940` slot `+0x20 = 0x2CB2D0` | `0x2CB30F` | `0`: private slot/load |
| AI, `0x1FB040` | `0x1FB111` | `0`: private slot/load |
| Garage preview, `0x2CBAE0` | `0x2CBB6F` | `1`: matching same-model previews may share |

These are distinct from the sixth argument, which participates in selecting the `_o` bundle variant. The fifth
argument is read into DL at `0x105B8F`; counting pushes without accounting for the three saved registers would
misidentify that flag. The asynchronous enqueue path `0x105CA0` uses the same model/key and reuse distinction.

`0x1057B0` finds the entry by its `+0x04` root, decrements the **byte** at `+0x09`, and releases the resource only
when that count reaches zero (`0x1057D8..0x105819`). The slot model ID then becomes `0xFFFFFFFF`.

## Fresh allocation and in-place bound relocation

For a new synchronous slot, `0x105B80` calls `0x105AA0`, which formats the selected vehicle bundle path and
calls `0x2105F0`. The latter jumps to `0x2126C0`, opens the file, reads its 0x80-byte header and validates the
single payload/version. At `0x21273D..0x212755` it calls `0x20DB20(payloadBytes,128)` and reads the payload into
that fresh allocation. `0x20DB20` normally obtains a new allocation through `0x1B3B90` at `0x20DB37`, aligns it,
and stores the original allocation pointer at aligned-address minus four. Its optional allocator hook receives
size/alignment, not a model name. This path contains no second name-keyed resource cache.

The shared source file therefore does not imply shared live bytes for two private slots. In contrast, a permitted
cache hit returns the same existing `entry+0x04`, which does imply shared bundled bytes.

`0x105830` relocates `root+0x0C` into the new payload at `0x105880..0x105894` and initializes the supplied bound
in place through `0x234660`. Bound type 3 dispatches to `0x239880`, then `0x25A150`; its `+0x68` vertex pointer
and `+0x6C` polygon pointer receive the same relocation delta (`0x25A164..0x25A181`). These arrays remain inside
the newly loaded bundle. This is relocation of a private bundle, not a separately allocated vertex clone.

The physics constructor `0x121DE0` passes that supplied `u32[carResourceRoot+0x0C]` to `0x2A03A0`. The setter
`0x2A4BB0` stores it at archetype `+0x0C`; it does not clone it. The fallback `0x2A0280` allocates a free archetype
registration slot and name; its duplicate name is not proof that the same archetype or shape was returned.
The live snapshot `.local/bound-alias-street-with-profiles.json` confirms that each observed physics bound equals
its own supplied resource-bound pointer for the Golf R32, GS430 and Cuevito. Those different-model observations
support this pointer path, but do not replace a same-model live comparison.

The later read-only `.local/bound-alias-cache-street.json` records manager `0x831C22C0` and these actual values:

| Cache slot / model | Resource root | Key / users / pending | Supplied and physics bound | Vertex array |
|---|---|---|---|---|
| 0 / GS430 | `0x8328E780` | `1 / 1 / 0` | `0x832DAD40` | `0x832DAF80` |
| 1 / Cuevito | `0x83338400` | `1 / 1 / 0` | `0x8338D320` | `0x8338D500` |
| 2 / human Golf R32 | `0x833F4700` | `0 / 1 / 0` | `0x8343D680` | `0x8343D8C0` |

The observed Golf entry meets the private-key/user-count/pending predicates. The two other cars do not, despite
each currently having one user. This prevents an inference of privacy from controller classification alone.
All three models differ, so these values still do not prove that a same-model AI remains unaffected by a write.
The sampled Golf rendered child is `0x833F4750`, distinct from the cache root `0x833F4700`.

## Authoritative payload extent

The common relocation entry is **`0x105A40(root, bakedBase, payloadBytes)`**, cdecl. Entry stack arguments are
`[esp+4]`, `[esp+8]`, `[esp+0x0C]`. It constructs the relocation context
`{root, root-bakedBase, payloadBytes, previousContext}` at `0x105A43..0x105A6C`.

Both load modes supply this exact extent:

- synchronous `0x105AA0 -> 0x105B57` uses the checked header outputs from `0x2105F0`;
- asynchronous promotion `0x105B80 -> 0x105C0C`, or `0x105CA0 -> 0x105D4D`, uses the completed packet's file
  header and `root = u32[packet+4] + 0x80`.

A future guard can capture this entry's checked `(root,payloadBytes)` and track its lifetime with cache release.
Use 64-bit end calculations and committed ranges. Kernel heap-page allocation size alone is not proof of the
extent of a particular resource within a guest heap. **`root+0x7C` is not a length**: asynchronous promotion
overwrites it with an original allocation pointer at `0x105C17..0x105C1A`. Do not infer a synchronous header at
`root-0x80`, since that path read the header separately before allocating only the payload.

The render/model-resource child may be `u32[root+8]`; it is not the cache root. In the sampled bundles that child
can be `root+0x50`. Match `car+0x4C` against `entry+4`, not a misleadingly labelled rendered child address.

## Bound field types and reference accounting

`bound+0x04` is a **u8 bound type**. The resource bound constructor `0x234BD0` sets a **u16** at `bound+0x48` to
`0xFDE8` (65000). `0x2A4BB0` increments that word when `[0x51BB9C]` enables counting; `0x233DB0` decrements it
and returns it with zero extension. Resource teardown `0x1039FF..0x103A02` explicitly sets the word to zero before
deleting the bundled shape. Thus this large pinned baseline is not a uniqueness count of one.

The recorded live dword `0xCDCDFDE9` is **u16 65001**, or signed int16 -535, followed by `0xCDCD` padding.
Reading all four bytes as a reference count is invalid. Prefer the original unsigned interpretation and the
cache entry's separate **u8 `+0x09`** when deciding whether a resource is private.

The floor classifier at `0x121170` loads `bound+0x4C` with **`mov ebp,[eax+0x4C]`**, a 32-bit load. The four
decoded examples have zero upper 16 bits, so u16 and u32 counts happen to agree. The vertex pointer at `+0x68`
addresses tightly packed float3 positions, stride 12. Counts and checked multiplication must cover all bytes
actually consumed by the original classifier/rebuild.

## Decoded original shapes

The following canonical bundles are from `assets_x.dat_2`, under `resources/vehicle/<model>/<model>.xbck`.
Every range was checked against the actual entry bytes, using `fileOffset = bakedPointer - bakedBase + 0x80`.
All four have bound type **3**. Payload offsets below exclude the 0x80-byte file header.

| Model | File / payload bytes | Bound / vertex payload offsets | Vertices | Original minimum Y | Next-layer Y minus 0.05 | Bottom indices |
|---|---:|---|---:|---:|---:|---|
| 350Z `vp_350z_04` | 353872 / 353744 | 316768 / 317344 | 14 | 0.327949 | 0.615000 | 0,1,6,7 |
| Golf R32 `vp_golfr32_04` | 336080 / 335952 | 298880 / 299456 | 14 | 0.300000 | 0.655000 | 0,1,6,7 |
| GS430 `vp_gs430_04` | 349936 / 349808 | 312768 / 313344 | 14 | 0.300000 | 0.830539 | 0,1,6,7 |
| Cuevito `vp_cuevito_99` | 430480 / 430352 | 393424 / 393904 | 9 | 0.245747 | 0.448682 | 0,3 |

Values are float32 source geometry in chassis coordinates; they are not a floorpan clearance measurement.
The Cuevito has only two bottom-layer vertices in the asset. Its sampled live indices are all `-1` and cached
limits remain 0.1/0.2; these must not be reinterpreted as a prepared four-corner chassis. The snapshot's field
named `body_type` mistakenly read body `+0x2C`, which is a pointer. The original classifier checks its THIS
`+0x18` against 2; a correct live read of that field is still needed before claiming that branch was taken.
`0x122050` builds the wheel list using `+0x18` as its running wheel count. Separately, `0xFF200(car)` checks
`u32[u32[u32[car+0x4C]+8]+0x3808] == 2`. Neither field is the bundled bound's type byte 3.

| Model | Canonical bundle SHA-256 | Pristine vertex-array SHA-256 |
|---|---|---|
| 350Z | `fb52ab184fa39abef5c92702dd940c0362bf7ecb0b4042fae1281ff76edb9c8b` | `49b5307be9b3a0ad634b6c4ce72d2a4decae487ca8c5d9bb0e774cb8ba0216ca` |
| Golf R32 | `75653a62cd134db1e01edd119a33dce136751794b3be98ce8b00ffbd03358a8b` | `1efcbdfebed7c5fba9b67efce657c0d786e01c24ff2d59b13d349ad1eac5e46d` |
| GS430 | `bf10bab51673d437a6d6dba787ab55ab406abf3e78e7ee6f9db98f64cc924e51` | `305a079f8f5cfb2e0e27577e0df4832f2f531314bf5141e82ac1f02548eaa727` |
| Cuevito | `c5f6b250129a56026515d7e15e91c92aaee129ceb6848fd8c0fda1351cecea97` | `f67a3a77b74e068576541032de243dcc612855ea3e715b39232b5c5242756f80` |

Each model's base, `_o` and `_g` variants had identical pristine vertex-array hashes. Their payload lengths and
pointer offsets differ; in particular the Cuevito `_o` bound/vertices are at payload 347936/348416, not the base
offsets. A runtime check should use the relocated pointers and captured extent rather than canonical offsets.

## Guard required before a player-only floor write

A source-backed private path is available, but it must fail closed when its current values cannot be verified:

1. Establish concrete current human-car ownership, its body/sim/cfg, and the exact original refresh caller as
   described in [ground contact audit](ground_contact_audit.md). A shared cfg is not ownership evidence.
2. Read `R = u32[car+0x4C]`. Find exactly one matching live cache entry with `u32[entry+4] == R`; require key
   `u8[entry+8] == 0`, users `u8[entry+9] == 1`, pending `u8[entry+0xA] == 0`, and a non-free model ID.
3. Match the actual physics bound against `u32[R+0x0C]`. Require the captured live payload identity/extent to
   contain the shape header, the entire vertex array, polygon data used by rebuild, and all selected indices.
   Reject arithmetic overflow, uncommitted bytes, unexpected type/count and stale load-generation identities.
4. Check all currently registered/live car roots/bounds/vertex ranges for aliases before mutation. Refuse the
   shared preview key even when its present user count is one; its next matching acquisition is allowed to share.
5. Retain the existing original writer/rebuild and scope/tire-size checks. Source-private street loading does
   not prove underside alignment or the requested scrape/spark result. Run a same-model live pair with different
   then identical cfgs, and confirm the other car's vertices/polygon data stay unchanged through refresh/destruction.

If those private-bundle conditions fail, skip the floor correction. A separately owned native clone would then
need its own pointer, polygon/boundary data, reference and teardown proof; copying a vertex pointer or checking
the pinned `65001` shape count is insufficient.
