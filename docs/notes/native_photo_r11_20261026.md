# Native loading-photo and startup handoff — 2026-10-26

The isolated r11 executable completed three HD-enabled starts and normal exits. The startup gate
held each process before game initialization, then released it after the existing media worker
acknowledged its cooperative pause. All tests used independent private saves/settings and two
independent San Diego replacement files. Canonical r9 and public release assets were unchanged.

## Exact build and captures

- Source implementation: `5b5e2d7`; r11 EXE SHA-256
  `732796098f4c8d4d1fff94fa3255fca59166deebb558db353973a1833f0f434f`.
- Shared private settings SHA-256 remained
  `0f967f679854fd55ebb8d2cd222b64fd939441062b8ef1b16d7b24e7b3e28895`.
- BASE1 PID7900: four successful 1920×1080 BMPs at matching ordinals 1/4/8/16.
  Every image still showed the outgoing Career menu/circular wipe. These are rejected as a
  visible-photo baseline despite successful readback and both texture identities being observed.
- Candidate PID5344 and fresh BASE2 PID19860 used `30,60,90,120`. Both produced readable
  1920×1080 BMPs at 30/60; later requests did not fit the bounded matching episode. Ordinal30
  shows the portrait through a strong circular wipe; ordinal60 exposes the face and hands.
  The raw BMPs, metadata and closed logs remain under
  `.local/native-photo-qa-r10/{captures,evidence/startup-r11}`.
- The two ordinal60 images have different residual wipe arcs and shirt masks. Compare the
  unoccluded face/hands and relevant boundaries; do not describe these as identical whole-frame
  phases or normalize exposure to conceal the mismatch. Texture sampling alone proves neither
  visible contribution nor improved appearance.

The startup context records file hashes **at process spawn**. An unreleased gate has not loaded the
pack. The independent variant switch then runs before release; the matching `.released.json`
pins the intended BASE/candidate file bytes before normal initialization. Both records and the
actual gate log are required. The first candidate therefore correctly records BASE at spawn and
candidate at release; BASE2 records the converse. Do not relabel either earlier observation.

Root run proof: `.local/native-photo-r11-review-20261026/run-proof.json`, SHA-256
`1491d52f144ca4db4a05d14d965f47dc0e5b36263501bc505527c9883dd1a856`.
It pins all eight BMPs/metadata, context/release receipts/tokens, archived closed logs, exact EXE/
build manifest/settings and all five private save files in each of HDD6/HDD7. Both save sets still
match their initial independent copy. Separate visual decisions are not implied by this run proof.

## Accepted bounded San Diego comparison

Root and an independent texture reviewer inspected the actual BASE60/candidate60 BMPs. Both
leave the face, hands and relevant join visible. The joined candidate reduces coarse skin/finger
speckle and the discontinuity between independently processed panels while preserving the pose,
silhouette, dark brows/beard and hand placement. Both reviewers accept these two San Diego panels
as a local relative improvement. Small eyes/fingers/tattoos remain limited by the original image
and model; this does not establish photorealism or recovered anatomical detail.

The different outer wipe arcs and small central shirt disc are explicitly excluded from this
judgment. Whole-frame phase equality is false. An unchanged CUSTOMS-footer anchor has99.844961%
identical raw pixels, maximum channel difference2; that checks local framing, not whole-frame
exposure or an image-quality score. No normalization was applied.

- BASE60 BMP SHA-256:
  `30792de0c901e2d029da2e9fd71de386fd5d59173a0efd65f19acf2cf9302698`.
- Candidate60 BMP SHA-256:
  `ebf8b64eaabc3165ae7a577caa30edc5b0ecc74814835eb912a9b47e6219bffa`.
- Independent matched review: `.local/native-photo-r11-review-20261026/matched-review.json`,
  SHA-256 `9f3840a0abad7e12371a939fc841d17e5d1e5d5bb12432ca1e4b50fbfb2f45f3`.
- Parent accepted proof: `.local/native-photo-r11-review-20261026/native-proof.json`, SHA-256
  `4c44b4dcbf345d24fb95713eb03dcb11cab846c4ccd04df0736e646420211eb1`.
- Independent handshake review also passed14 isolated acknowledgment cases and actual same-worker/
  encoder retention. Its receipt SHA-256 is
  `5e33004f54a91d53fbc75e5c1467fe622c0f6c6b4a838648868231229e08ab0b`.

The new `.local/reviewed-san-diego-proposal-20261026-r2` pins this accepted proof under the current
planner. A fresh full source audit still verifies7,081/7,081 eligible original-derived replacements.
Candidate chain and native review pass. Installation is correctly blocked by the live coordinator/
movie/inference workers. No canonical file or ledger was changed, and that check was not bypassed.

The r2 plan SHA-256 is `fe63c428f49f6dd7433aad97f7e044d33cf4c3eea46c0d0ba178b888a30ab852`;
its proposed ledger is `f8ed40b44fbdeabfb4586e630e795a6481fa63e28905293b7d51e6fcce08d9cc`.
Frozen r1 remains unchanged (plan `73c2cd88...`, ledger `0e0b3035...`). Readiness must be checked
again immediately before any actual transaction.

## Scheduling and preserved state

Coordinator9824, video child18460 and its encoder survived the entire pause. The in-flight
`story/sqtuner.bik` remained at24/996 while QA owned the GPU. After the last owned window closed
normally at06:15:53 MDT, the coordinator removed its own unchanged private pause marker. The
same child/encoder logged resume and advanced to32 and later frames. No shared user stop request
was created or cleared, and no second inference worker was started.

An account-usage interruption left the candidate on its menu and the media paused for several
hours. That delay is real; this was not uninterrupted media processing. The continuation first
checked physical inactivity and session ownership, finished bounded captures and closed all owned
games. Never leave a released QA session holding the queue after finishing available checks.

The live coordinator snapshots its `waiting` list at the first pause transition, so an overlapping
owned gated process can retain the same safe pause while that list still names its predecessor.
The local handoff checker additionally requires a known released predecessor, a fresh heartbeat,
the original coordinator/video PIDs, the worker's actual pause acknowledgment newer than its last
resume and no live NCNN invocation. A requested pause or a stale waiting-list entry alone is
insufficient evidence.

The actual desktop shortcut had pointed at an earlier private r7 QA executable. Its exact original
bytes/fields were independently backed up before restoring the normal canonical r9 target.
The repaired shortcut remained SHA-256
`9902d5c92272ebeb07ce0e78975d5ee2abadc6c6c1dd9e46036f84f93e854bf5` throughout
all new private runs. Existing saves were preserved.

## Remaining gates

The accepted native San Diego proof and fresh r2 proposal are ready for later revalidation.
Applying it still requires coordinator/media/game exclusion checks and the tested transaction
path; a paused child remains a live worker. Prefer a published-movie boundary for any necessary
paired coordinator/worker restart, also activating the tested byte-identical source reuse then.
Do not discard the current movie merely to apply this ready proposal. Other city portraits retain
their separate native-review requirement. Full-body pedestrian walking/footplants and all-car
road contacts/sparks remain open.

The original-derived texture pass remains7,081/7,081 complete. Strict movie coverage was38/106
with zero failures at this checkpoint; it continues on the retained worker. Final audits and an
all-file verified project backup still precede new offline PBR generation. Runtime PBR is unchanged.
