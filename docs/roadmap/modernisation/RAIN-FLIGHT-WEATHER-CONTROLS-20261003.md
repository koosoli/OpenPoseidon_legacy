# Occupied flight weather controls

The installed `actual-occupied-rain-flight-20261003-125917-b0128c` campaign
failed its Default-Dry readiness gate: actual rain reached 0.2071214765 after
warmup. `0 setRain 0` does not hold native weather dry under full cloud.

`Weather::SetRain` stores a clamped target and transition speed. In
`Weather::MoveClouds`, overcast 1 gives `maxRain=0.5`. Whenever target and actual
rain differ by less than `1e-6`, the native weather chooses another random target
and random transition speed. An already completed zero transition can therefore
start raining on the next ordinary simulation step.

The repaired runner first pauses, requires actual native zero liquid rain in
the dry arm, and starts an ordinary noncompleted stock transition to `1e-5`.
The target remains safely beyond the native equality threshold. The duration is
derived from the actual native map header and the unchanged `1e-6 m3` rainfall
budget tolerance, rather than an arbitrary enormous scalar:

- `RainWaterField::AlignedSourceGrid` bounds the source to 513 nodes per side.
  Actual Nogova range 2048 and spacing 6.25 yield stride 4 and spacing 25.
- Including all padded equal-area edge nodes bounds the area to
  `513² × 25² = 164,480,625 m²`, treating even underwater cells as rain receivers
  for a conservative upper bound.
- The actual coarse solver uses `0.00012 m/s` at rain density 1.
- The whole arm is limited to 360 actual simulation seconds, enforced in the
  physical state reader as well as the existing wall deadline.
- Tenfold budget margin requires duration at least
  `area × rate × target × 360² × 10 / 1e-6`, rounded up to a power of ten.
  The actual Nogova recipe is `1000000000000 setRain 0.00001`.
- Native float transition speed is approximately `1e-17/s`. Maximum density
  remains below `3.96e-15`, including a 10% float-accumulation allowance.
  Whole-arm input is bounded below `2.82e-8 m³`, also allowing one 0.25-second
  runoff step. The existing dry measurement budget remains `1e-6 m³`, with an
  additional unchanged-tolerance whole-arm check.

This is a bounded stock weather-transition dry control, not acceptance of random
natural dry weather and not a zero-water-physics control. Its tiny positive
actual liquid rain initializes the coarse runoff domain. Both dry and rainy arms
must have the actual expected aligned domain before measurement, and matching,
unchanged domain dimensions through their measured windows. The initial,
premeasurement, and final domain and rainfall-budget receipts are retained.
The rainy arm retains its ordinary `0 setRain 1` transition and must satisfy the
existing real wet-weather, rendered-drop and positive-liquid-input gates at
every relevant sample; random variation remains visible in the record.

Particle authority is checked separately. `weather_particles` must report mode
`particle`, actual `densityOverride=-1`, and no snowflakes. Its effective,
native-weather, and liquid density fields must agree within that single query.
`weather_visibility.particleDensity` reports **effective density**, not the
override parameter; its effective/native/liquid fields must likewise agree.
The two separate queries are recorded without pretending their clocks are the
same. Real rainy effective density is positive, while actual dry liquid input
must satisfy the unchanged dry rain and mass gates. No override, synthetic
trajectory, weather renderer bypass, shader change, or particle suppression is
introduced.

CPU `-SelfTest` checks the actual native float dry recurrence over all 21,600
steps at 60 Hz, source-grid/duration/mass derivation, particle authority and
effective-density falsifiers, actual domain mismatches, and all existing flight,
timing, capture and route-overlap guards. Self-tests and diff checks pass. Root
owns the actual installed-game four-arm benchmark and interpretation of costs.
