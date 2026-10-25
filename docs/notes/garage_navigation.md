# Garage navigation cadence

2026-10-24. Original garage direction navigation now has a short pulse cadence to prevent a held D-pad or
left stick from racing through adjacent choices. The first direction press responds immediately. Holding it
waits 300 ms before repeating, then repeats every 150 ms with a 35 ms pulse. Left-stick navigation enters at
about 65% axis travel and releases below about 45%, so threshold noise does not produce repeated new presses.
Direction reversals and the other axis respond immediately; simultaneous opposite digital directions cancel
only their own axis.

The original garage update renews a separate **100 ms navigation lease** through `GarageTuningActive()`. Its
existing 750 ms visible-button hint is unchanged and does not gate controls. A concrete human street driver
calls `EndGarageNavigation()` immediately; the lease also expires if garage updates stop. The input path
requires garage activity, foreground focus and a closed PC settings GUI. Leaving that scope, losing focus,
disconnecting/changing the first controller or a clock rollback resets the pulse state.

Hardware controller, keyboard, timed-script and live-script inputs are still merged. The complete raw merged
pad is stored before shaping for F1/F2/Back+Y GUI navigation. Only retail garage direction navigation is shaped;
driving inputs, action buttons, triggers and the right-stick camera retain their previous mapping. Left-stick
navigation is expressed once through the D-pad output to avoid duplicate stick-and-D-pad events.

The existing `input_mapping` test passes in an isolated CPU build. It checks exact first/held/repeat boundaries,
1/8/16/33 ms polling grids, diagonal/axis reversal and conflict behavior, analog hysteresis, focus/garage/device
reset, slow frames without queued catch-up, time rollback and complete physical/keyboard/script merges. These
tests use supplied timestamps and no sleeps or desktop input. Native menu feel remains a bounded playtest for
the next idle candidate; the currently active human game was not touched.
