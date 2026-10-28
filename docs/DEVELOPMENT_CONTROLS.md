# Development controls

These controls are in the tested local development build installed on 2026-10-28.
The older downloadable development release has its own feature list.

Open **F1** for settings, or **F2** for Vehicle tuning directly.

## Setups for each owned car

In **Vehicle**, enable **Use per-car saved setups** and leave **Edit global fallback**
off to edit the current owned car. Set its values, close F1 or choose **Save now**, then
switch cars through the game's garage. Each owned car keeps its own setup.

To share a starting point, enable **Edit global fallback** and set those values.
**Copy global to this car** gives the current car a saved copy. **Use global for this car**
returns that car to the fallback. Disabling the per-car master applies the global values
while retaining the individual setups for later use.

Purchased parts still use the game's original garage/save flow. PC tuning is stored in
`config/tuning_profiles.toml` beside the matching game instance. Keep that sidecar with
the matching career when moving a setup. See [per-car details](notes/per_car_tuning.md).

## Traffic and pedestrians

In **World**, choose **Empty streets**, **Original density** or **Busy streets**, or
adjust each slider from 0x to 4x. The targets apply when your car is active in career
free roam. The garage, races and other missions keep their original population rules.

1x uses the game's chosen density. 2x requests twice that target; original spawn pools,
region rounding and authored limits still affect what appears. Existing actors do not
necessarily disappear immediately at 0x, and a target does not promise an exact visible
actor count. These are PC preferences saved through F1's normal settings flow.

## Career money

Load your career and enter the garage or streets, then open **Career**. Enter a new
balance and choose **Apply balance**, or use one of the **Add** buttons. Each action
changes the balance once; purchases and race rewards continue normally afterward.

Use the game's **original save flow**, such as saving while leaving the garage, to keep
the balance change. F1's **Save now** saves PC preferences; it does not replace a career
save. No automatic starting balance is installed and the original save format is unchanged.

## Current visual work

PBR materials, BRDF and geometry helpers are still in development. This build keeps the
existing renderer; it does not yet enable a complete PBR lighting path. Native controller
checks, broader vehicle visuals and the remaining media work continue separately.
