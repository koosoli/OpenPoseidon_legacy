# Guarded single-pass prototype, 2026-10-03

This is an offline generated-header experiment. Production
`RainWaterField.hpp` is unchanged. First establish the faithful baseline with
`Measure-RainWaterCoarseReplay.ps1`, then run
`Test-RainWaterCoarseFusion.ps1 -BaselineReceipt <baseline-result.json>`.
Default execution performs correctness checks only. The optional bounded
`-Measure` ABBA run is for a root-authorised quiet window; game presence is
refused before/after timing, and no game FPS or installed acceptance is claimed.

The candidate gathers eligibility during the unchanged source/sink pass:

- spacing in [10, 1,000,000] metres;
- every bed's absolute height <=1,000,000 metres;
- every post-source retained depth zero or finite normal binary32, <=1024 m.

Comparisons exclude NaN/infinity, negative and positive subnormal depths
without clamping or changing a physical value. Unsafe states use the existing
two-pass cached transfer/limiter path. Empty states retain the existing skip.
The conservative bounds intentionally limit dispatch; they are not changes to
rain, water storage or source admission.

Each cell has at most four incident edges. Every raw transfer magnitude is
bounded by donor depth times speed<=5 m/s times the .25-second step divided by
spacing. Thus the exact total outgoing depth is <=.5 donor depth at the
minimum spacing10, and <=.2 at the actual native spacing25. Binary32
round-to-nearest relative error (unit roundoff2^-24), the three outgoing
additions, and conservative subnormal absolute rounding error leave the
maximum below .50001 donor depth for the admitted normal-depth range.
That is far from one; the original donor multiplier is exactly1.

Bed/depth caps keep both free-surface sums and their difference finite
(absolute head difference <=2,002,048 m); the9.81 multiplier, square root,
depth*speed*.25 transfer and flux-to-flow intermediates remain finite in the
admitted spacing range. The speed and head caps remain the original ones.
No fast-math or replacement square root is enabled.

Eligible steps visit the original interleaved row/axis edges once, evaluate
the exact Transfer and write delta/flow in that same order while bed/depth
stay fixed until FinalCells. Multiplication by the original exact1 donor
normalisation is omitted. The existing SourceSink, Clear, FinalCells,
budgets, pending and revision operations remain.

Unused outgoing/raw-edge scratch intentionally differs after a fused step.
Public bed/depth/flow, pending, budgets and generation/revision must match
bitwise. The identical baseline A/B controls retain their stricter all-scratch
checks. Source audit found outgoing/raw-edge reads only in Step: fallback
clears outgoing and overwrites every raw edge before its cached pass.
Fine activation imports depth, budgets and pending, and public capture/query/
snapshot consumers do not read these caches.

Actual Dry/Rain captures passed observable equivalence after every one of
fourteen Advances at Clang O0 and O2. Rain exercised nine fused steps with
zero fallbacks; Dry exercised the unchanged empty skip. Observable fingerprints
match the faithful baseline (Dry ed5f292f9bf3042e, Rain14151e9df61326e1).
Named synthetic controls also passed: spacing2/9.999/10/25/1e6/1e6+1,
depth1024/1025/FLTMAX, overflowing existing AddWater input,
fused→fallback→fused terrain transitions, and simultaneous speed-capped
faces at the minimum admitted spacing with extreme admitted bed/depth, dry
neighbours and sea sinks. The two class sizes also match.

Branch witnesses exist only in the generated correctness header. Optional
timing removes those declarations/increments into a separate header and
records its hash; it changes no class layout or solver math. The proof and
timing sources remain separate artifacts.

Root released a quiet window after game/device/build work stopped. The
four-round counter-free ABBA receipt is
`build/rain-water-coarse-fusion/run-20261003-183545066/result.json`.
All timed output fingerprints agree.

| Actual captured state | Baseline median wall/Step | Prototype median wall/Step |
| --- | ---: | ---: |
| Dry | 1.33692 ms | 1.32206 ms |
| Rain | 5.26248 ms | 4.39578 ms |

The Rain median is16.47% lower. All eight prototype blocks (4.266–4.581
ms/Step) are below all eight baseline blocks (5.098–5.404). Actual
calling-thread accounting is quantised; its median changes from4.8828 to
3.9063 ms/Step. Dry ranges overlap and its1.11% median difference is treated
as noise. No dry improvement or game FPS increase is claimed.

Production adoption still requires root review, appropriate further
exact-state controls and normal build/deploy/runtime verification. Production
solver source is untouched by this prototype commit.
