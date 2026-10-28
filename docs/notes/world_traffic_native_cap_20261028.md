# Traffic density: observed setter and native cap order

The R4 native run showed an active career street guard and the requested traffic
multiplier, but zero tracked roads. That proves UI/guard activation; it does not
prove a density change or a visible traffic-count increase. The image is preserved
in the external R4 control's `native-observations-r4/world-free-roam-active-targets.jpg`.

The original USA XBE establishes a missing writer in the observation path:

- `0x1CB290` calls `0x2B9670` for each road, then tailcalls `0x2B8610` at
  `0x1CB372`. The ordinary settings path similarly calls the density setter at
  `0x22A26`, then the cap pass at `0x22A3B`.
- `0x2B8610` computes `float([0x4B4CFC]) * [0x4499F0]` and passes this limit to
  `0x2B9690` for each road. The original XBE values are integer 30 and float
  0.01, giving a nominal density ceiling of 0.3. `0x6A660` can change the integer
  through the original settings path; no replacement global value is installed.
- `0x2B9690` only reduces `road+0x20` when `current - limit > [0x44935C]`.
  That original threshold is float 0.001. A reduction recomputes `road+0x28`
  through `0x2B93C0`. Its argument and threshold remain unchanged.
- The earlier hook observed only `0x2B9670`. A baseline of 0.3 at 2x became
  0.6, then the unobserved cap reduced it to 0.3. The deliberate foreign-write
  guard subsequently discarded the tracked road. This sequence reproduces the
  tracking defect in the actual generated retail bodies; it is not retrospective
  proof of the precise R4 runtime branch.

The added cap hook operates only on an already observed road whose manager,
array position, topology and current density still match the tracked record. It
restores the unscaled baseline through the original setter, executes the original
cap once on the real caller context, records the resulting native baseline, then
uses a copied-context original setter replay to apply the bounded multiplier.
The multiplier therefore applies **after the native density ceiling** and may
exceed that numeric ceiling. Original actor pools, path calculations and other
population rules remain authoritative; visible actor counts are not guaranteed.

Unknown, recycled or foreign-written roads do not gain baseline authority. Missing
scratch before staging preserves the actual native cap call and drops tracking.
A failed final replay leaves the valid native density/target for a later retry.
No player-mode, AI, memory or topology guard is relaxed.

Focused evidence is in `.local/world-native-cap-cpu-20261028-r1`: the original
setter/cap-pass regression fails before the fix and `world_tuning` passes after it.
The fixture uses actual retail cap, setter and target bodies; verifies one native
cap call, full returned CPU state and caller stack bytes; and covers cap increases,
reductions and threshold no-ops, 0x/1x/2x/4x, authored zero, inactive/AI states,
recycled topology, foreign writes and unavailable replay scratch. No native
launch, GPU work, save modification or density-success claim accompanies this fix.
