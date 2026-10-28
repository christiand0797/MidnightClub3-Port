# F1 population and career controls — development evidence

These are new working-source features, not yet installed or released. The original career
save format and save flow remain unchanged. The F1 extension uses title-owned World and
Career tabs; world preferences persist with PC settings, while money is an explicit
one-shot request to the original game thread.

## Population

The World controls multiply the game's authored free-roam densities from 0–4. They do
not allocate larger spawn pools. The original road setter `0x2B9670` and calculator
`0x2B93C0` compute road targets; `0x2B9300` compares those targets with current counts.
Its consumer queues road indices and the ambient free-pool lookup `0x7DFB0` refuses an
empty pool. Pedestrian writer `0x222770` retains its authored regional caps and 0–127
computed-target clamp. These consumer checks support densities above the retail option's
0–1 range without altering pool storage/capacity.

The implementation tracks actual original setter observations, preserves native baselines,
does not compound multipliers and restores unscaled densities outside the validated
human-player free-roam context. A changed manager, road topology or native write retires
the old tracked baseline. Scripted missions and races retain their original rules.

Actual recompiled retail-writer fixtures demonstrate road targets increasing at 2x/4x from
a native density of 1, a spawn request changing accordingly, pedestrian targets 16→32→64,
and unchanged authored/global caps and pool exhaustion behavior. This is not certification
of exact visible actor counts, streaming performance or every city; those need native QA.

## Money

Career singleton `0x557F0C` owns typed career data; its signed balance is at data `+0xACC`.
Original serializer/loader `0x280256`/`0x28301C` use that same `Money` field. The ordinary
garage writer stores it directly, then normal save-needed logic `0x197F0` reads the native
dirty flags. The extension uses those semantics rather than editing save files.

UI code only reads a published host snapshot and queues a bounded explicit amount. The
game update validates loaded career identity, pages, vtables and owned-slot context before
changing the balance and the native dirty flags once. Subsequent purchases/rewards are
not overwritten. Profile reset/load/destruction invalidates pending work. Independent
review identified an additional UI/career-switch race; an opaque career token is now
bound to both the displayed draft and the queued action. Stale tokens are rejected.

The editor supports $0–$999,999,999 and quick additions. That upper bound is a PC editor
policy with signed-integer headroom, not a claim of a discovered retail balance cap.
The game remains responsible for its normal save flow. Immediate garage-wallet display
refresh still needs a native observation; a changed F1 number alone is insufficient proof.

## Integration and remaining checks

Title tabs participate in the normal F1 save-on-close behavior. A separate `save_aux`
callback runs only during an actual PC settings save; logging settings does not rewrite
title-owned preferences. Twelve focused integrated CPU tests now pass, including token,
per-car persistence, actual retail population writers and 480p menu interaction.

Pending: separate candidate build, bounded native population and money/save-reload verification,
then immutable release packaging for the exact tested source/runtime. The sealed R12
cohort and its earlier evidence remain unchanged.
