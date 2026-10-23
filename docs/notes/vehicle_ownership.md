# Driving-car tuning ownership repair — 2026-10-23

The report that tuning appeared only after changing another garage option, then disappeared on leaving the
garage, exposed a concrete ownership gap. The driving car had never been registered for PC tuning. During the
parent's live inspection its human-controller type/backpointer matched the original game, but its wheel tune
and runtime suspension cache still contained the purchased 0.275 m value despite a saved additional +0.5 m.

## Cause: factory stack addresses were rejected

The old leaf-factory hooks required `Plausible(factory)`, which permits heap-style addresses below `0x84000000`.
The native guest stacks begin at `0xD0000000` (`runtime/kernel/memory.h`). The original driving construction
path places the factory on its stack: `0x2C004C` takes `esp+0x6C`, `0x2C0050` calls its constructor `0x2CB270`,
and `0x2C0055..0x2C0059` invokes the complete build `0x2CBC40` from another stack-relative address. Its
vtable `0x476940` slot `+4` is the hooked human component creator `0x2CBDE0`.

The hook therefore ran but rejected the valid factory address before registering its `+0x44` car pointer.
This is an address-validation error; the analysis does not establish a broken native virtual dispatch path.
Both human and AI factory wrappers now check the committed factory prefix with an overflow-safe 64-bit end
calculation, accepting committed stack factories while retaining guest-page checks.

## Concrete recovery during the normal car update

`GarageRefreshPlayerCar(car)` is called by the parent at `0xFFD90` entry. It recognizes driving ownership only
from the actual object relationships below, rather than promoting every car that shares a human's CarCfg:

| Field | Original evidence |
|---|---|
| `car+0x04` | Human driving controller installed by `0x2CB320`, store `0x2CB385` |
| `controller+0` = `0x47684C` | Human controller constructor `0x2C9970`, store `0x2C9995` |
| `controller+0x50` = car | Controller initializer `0x2C89F0`, store `0x2C8A0F` |
| `car+0x5E` = 0 | Human factory `0x2CBE27`; AI factory sets 1 at `0x1FAE57` |
| `car+0x50`, `car+0x64` | Actual instance's vehSim and CarCfg, committed guest ranges required |

`car+0x14` is damage, created by `0x2CB680`/`0x102210`, not driving input. `car+0x18` is the feedback/rumble
object accessed by the input controller at `0x2C9BC3..0x2C9BD1`; neither is an ownership substitute.
AI controller creation uses `0x1FAC30`/`0x1FC8D0`, not the human type. Garage previews continue to have their
explicit selected-garage ownership because they may have no driving controller.

Normal unchanged frames do not rebind or rebase. A new/different concrete owner retires stale simulation aliases
before publishing its binding, including when a repaired factory already registered the new car. A delayed old
destructor cannot then forget the replacement's tuning. Concrete AI ownership remains stronger than shared cfg
fallback, including a previously human instance switching back to AI.

## Baseline and refresh timing

Recovery sets dirty flags `0x81` on the actual CarCfg, then lets the original **same-frame** `0xFFD90` update
refresh the original wheel/suspension state. The original `0x121970` first writes purchased axle offsets;
the existing `TuningCarCfgApplied` hook then adopts those fresh values and applies PC offsets.
Calling that hook directly on already modified values during late discovery could double-add the adjustment,
so the recovery helper deliberately does not do so.

## Verification and limits

The garage regression checks exact controller/backpointer/AI discrimination, both cfg-sharing directions,
unchanged-frame stability, deferred baseline notification, new driving cars, reused simulation addresses,
delayed destruction and factory-registered aliases. On Windows it reserves a sparse 4 GiB guest address range,
commits only the small fixture and one high stack page, and executes the production factory wrapper against
`0xD1234000`. Uncommitted high pages and wraparound factory addresses are rejected. The original generated
profile getters/setter and string-table teardown tests remain enabled and pass alongside these checks.

The parent owns the native game build and street/garage validation. These fixture results do not establish
all-car live road contact, extreme-camber geometry or successful street retention until observed in that run.
