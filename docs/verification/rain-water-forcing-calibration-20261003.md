# Conserved rain input calibration, 2026-10-03

The ordinary rain slider now supplies at most 0.000025 metres of water per
simulation second (90 mm per game hour), shared by the coarse and native-cell
solvers through `RainWaterForcing.hpp`. The previous maximum was 0.00012 m/s
(432 mm/game-hour). This is a presentation calibration intended to reduce
widespread flooding during ordinary rain; it does not claim that the visual
weather slider represents a measured meteorological rainfall rate.

The installed 56549210 Eden receipt campaign
`actual-water-depth-and-bare-soil-20261003-161012-8194fd` measured genuine
standing water: 42.371 mm at the wet mud contact, about 42.368 mm at the source
centre, and 15.273–78.472 mm over the bounded 3x3 coarse neighbourhood. At the
centre the native bare terrain and the simulation bed agreed within micrometres.
The prior fragment diagnostic classified the broad visible mirror as retained
water. These observations support reducing conserved rain input rather than
hiding that water in a shader. They do not establish a fragment rain-reach bug
or prove that every map's water support is correct.

Infiltration remains 14.4 mm/game-hour. Evaporation remains 7.2 mm/game-hour
under fully overcast, still conditions, increasing to 28.8 in full sun, plus
up to 3.6 from wind. Runoff, conservative transfers, sea/outlet accounting,
source generation, tile ownership, pause/time partitioning, and separate mud
and clothing wetness histories are unchanged. Sustained rain can still fill
hollows. Signed boot depressions are still a separate soft-surface field;
this change does not add their volume to the native hydrological bed.

Source/CPU validation:

- `scripts/Test-RainWaterField.ps1`: pass, including the independently
  specified 90 mm/hour ceiling and equal-area coarse/native rain, sink and
  mass budgets, pause and dry zero-input controls. The sustained coarse
  catchment test uses 600 seconds and retains its existing >8 cm hollow gate.
- `scripts/Test-RainWaterFine.ps1`: pass for ownership, conservative flow,
  geometry, crater storage/spill, source/sea, budget, reset and time partitioning.
  Its original 120-second native crater still exceeds the existing 3 cm gate.
- The midpoint fixture now evaluates flux bounds against the actual
  post-forcing/preflux volumes and native volume-to-head curve. Existing
  tolerances are unchanged. Both net-draining and net-filling cases pass;
  level-lake controls derive their balanced slider strength from the shared
  calibration.
- The independently owned donor-guard prototype passed the pinned coarse
  reference proof at Clang O0 and O2 with explicitly matched forcing. Only
  the reference's known input literal/include was normalised after verifying
  the original pinned source hash. This calibration commit changes no
  transfer/limiter code.

The combined game build, matched-pair deployment and repeated actual Eden
receipt/capture comparison belong to the root agent. This source checkpoint
does not claim that the new rate is installed, that its visual result is
accepted, or that wet mud/uniform optical quality is complete.
