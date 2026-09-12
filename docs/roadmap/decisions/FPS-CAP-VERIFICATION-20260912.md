# FPS cap versus unlimited: initial installed verification

Installed/source stamp: asset-obs-001 10515826, 2026-09-12 18:52.
No engine changes or deployment in this investigation. Foreign Cargo.lock untouched.

## Code and settings

- Core/Game/GameLoop.cpp::PaceToFps uses microsecond budgets and SleepUs;
  nonpositive cap skips pacing. Background/debug limits use a separate branch.
- UI/Settings/GraphicsApply.cpp applies fpsCap to gUserFpsCap, with explicit
  POSEIDON_FPS_CAP diagnostic override. GraphicsPage exposes Unlimited and
  30/60/90/120/144/240. This is distinct from Scene's adaptive LOD target FPS.
- Installed traces confirm the effective 60/120/0 caps. Unlimited sleeps zero
  frames; 120 sleeps approximately 1%, because this scene cannot reach 120 FPS.
- Existing PoseidonTests [GraphicsConfig]: 28 cases, 157 assertions passed.
  This covers configuration logic, not interactive menu acceptance.

## Initial measurements, not a controlled quality-equivalent optimisation A/B

scripts/pace-series.ps1 -Label cap-audit-20260912 -Caps @(60,120,0)
  -SettleSeconds 15 -StaticSeconds 15 -MoveSeconds 25 -LoadSeconds 45
  -Width 1600 -Height 900 -Msaa 1 -Shadows 3 -Repeats 1

Installed-game lock used; own processes cleaned up by harness. Scratch user
settings only. Everon perf_field.eden, identical scripted route, native resolution,
DLSS off, VSync off, terrain/object/grass tier 4, render scale 1. Owner settings
and game installation unchanged. Captures, CSV traces and analysis JSON reside
in .tmp-movecam/out/cap-audit-20260912-cap{60,120,0}.

Moving phase (approximately 24.5 seconds each):

| Cap | Mean FPS | Median period ms | p95 ms | p99 ms | Max ms |
|---|---:|---:|---:|---:|---:|
| 60 | 57.9 | 17.03 | 17.30 | 29.84 | 38.9 |
| 120 | 67.9 | 14.71 | 16.83 | 18.74 | 45.2 |
| Unlimited | 69.3 | 14.50 | 16.74 | 30.52 | 62.4 |

Mean consecutive-period difference: 0.66 / 1.19 / 2.34 ms respectively.
Limiter sleep overshoot median about 0.3 ms. Main simulation mean roughly
1.06 / 0.56 / 1.86 ms; the coarse phase cannot identify the underlying cause.
At high cap/unlimited the producer mainly waits on the renderer and the renderer
on image acquisition/GPU back-pressure. These are engine timing measurements,
not external displayed-frame measurements (no PresentMon).

Important limitations: one run per arm, fixed order, no clock/thermal control,
adaptive LOD NOT frozen. Final snapshots have different main triangle counts
(647535 / 662291 / 823827) and are outside the moving analysis window. Thus
identical settings do not prove identical rendered work. RAM/VRAM peak analysis
and repeat/reverse-order runs are absent. Do not claim a performance gain or
blame the limiter, terrain, timer resolution or interpolation from these runs.

## Result and next smallest experiment

Lower mean FPS when unlimited was NOT reproduced. Less even frame delivery WAS
observed in this sample. That distinction matters for the community report.
No speculative production change, increased fog, reduced visibility or LOD
quality reduction was made.

Before choosing an optimisation: repeat 120 versus unlimited in reverse order,
record per-frame LOD and actual render costs, isolate slow sim:step subphases,
and capture displayed frame times if tooling permits. Also test native/DLSS and
VSync separately rather than changing several knobs together. Timer-resolution
differences between capped/uncapped are a hypothesis, not a diagnosed defect.
Use a CPU-light scene capable of exceeding 144 FPS to verify the high caps;
this GPU-limited scene cannot establish their deadline accuracy. User-menu
changes and persistence should additionally be exercised without the env override.

## Additional community observation

DavidArmstrong007 reports that the issue appears above 90 percent GPU utilisation
and disappears below it. This is a tester observation, not a controlled local
measurement or proof of a universal utilisation threshold. It strengthens the
case for testing GPU saturation, queue/back-pressure and displayed-frame pacing
separately from limiter deadline accuracy. Compare the same scene and quality
with a cap below sustainable throughput versus unlimited; log GPU load/clocks
and frame times, not only an FPS overlay. Do not reduce image quality as a fix.
