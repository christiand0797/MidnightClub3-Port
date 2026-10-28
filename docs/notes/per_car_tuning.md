# Per-owned-car PC tuning

The development build adds per-owned-car setups to F1 → Vehicle. This is PC preference
metadata in `config/tuning_profiles.toml`; original career files and their format are
unchanged.

- **Use per-car saved setups** applies individual saved overrides. Disabling it applies
  the global fallback to every car while retaining those overrides for later use.
- **Edit global fallback** chooses the values being edited. Editing the fallback does
  not erase an individual car's setup.
- **Copy global to this car** starts or replaces its override from the current fallback.
  **Use global for this car** disables that override without changing purchased parts.
- Changes save with the ordinary F1 save-on-close/Save now flow. Original profile
  serialization also persists updated PC roster metadata when garage parts change.

Ride height, camber, offset, suspension, brakes, grip, horsepower, wheel tuck and airbag
matching resolve independently for each owned car. Extended garage choices and model fit
limits remain explicitly global because the original garage database shares those limits.

There is no proven persistent unique car ID in the original save. PC-assigned IDs therefore
follow the validated owned roster and actual CarCfg copy lineage, with separate IDs even
for identical models. The original removal/compaction path carries the remaining car's ID
forward. Recycled configurations lose their old bindings. Unknown configurations use the
global fallback rather than guessing by model name or the currently selected car.

Exact unchanged rosters reload their saved setups. An externally replaced/rolled-back
roster that cannot be reconciled gets new identities and global defaults; external save
editing is not certified to retain PC tuning associations. Back up the sidecar together
with the matching career when transferring a setup. Malformed/unknown-format sidecars and
concurrent external edits are preserved rather than overwritten.

CPU tests cover two identical-model cars with different setups, switching back, disk reload,
per-configuration copies, constructor reuse, original compaction, last-car removal,
parts-change → normal serialization → reload, global master disable/re-enable, invalid
values and read-only malformed/external data. Tests execute the original profile-image
copy and slot-compaction instructions. Bounded native checks also retained Bel Air -5.1°,
Golf +2.0°, then Bel Air -5.1° when switching back; disabling/re-enabling the master exposed
and restored the fallback/override. A subsequent process restored the Bel Air setup from
the saved sidecar. The installed runtime includes the same profile implementation.
These observations do not certify every parameter on every car or physical-controller use;
see [exact build and native evidence](feature_native_20261028.md).
