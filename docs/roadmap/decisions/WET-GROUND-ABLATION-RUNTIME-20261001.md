# Real-weather wet-ground material comparison

Source runner: `scripts/Test-WetGroundAblation.ps1`, based on the installed TCP
and real-rain lifecycle in `Test-RainWaterRuntime.ps1`. Runtime and appearance
acceptance are pending. This candidate changes no production material.

The user reports pale or milky wet ground. Current cosmetic puddles already
reflect directional sky/cloud radiance with Fresnel, rather than adding a
constant white lift. Moist soil uses bounded broad reflection. The physical
rainwater material independently reflects sky or its private pre-rain SSR
source, with depth-dependent Beer absorption. One concrete source risk is
overlap: physical water can transmit terrain that already reflects a cosmetic
puddle interface. That could exaggerate reflection. It is a hypothesis to
test, not an established cause from earlier captures.

## Explicit serial controls

The default `-SingleArm All` runs these five arms on one unchanged installed
EXE/DLL stamp/hash pair, with a fresh private profile for each process:

| Arm | Terrain soil+film | Physical rainwater | Physical-water SSR |
| --- | ---: | ---: | ---: |
| Baseline | 0 | 0 | 0 |
| SoilFilm | 1 | 0 | 0 |
| PhysicalSky | 0 | 1 | 0 |
| CombinedSky | 1 | 1 | 0 |
| CombinedSSR | 1 | 1 | 1 |

`WGR_TERRAIN_PUDDLES=0` disables both moist-soil shading and cosmetic puddle
film. There is no existing film-only switch. `SoilFilm` is deliberately named
for both effects. `WGR_RAIN_WATER=0` disables only rendering; real CPU rainwater
still accumulates and drains. SSR OFF retains the physical material's sky
reflection. Explicit parameters set the child process flags, rather than
discarding inherited flags as the older lifecycle runner did.

Run one audited Noe campaign first, under the shared game lock:

```bash
LOCK_OWNER='wet ground Noe matrix' scripts/with-game-lock.sh \
  powershell -NoProfile -File scripts/Test-WetGroundAblation.ps1 \
  -ExpectedCommit <installed-commit> -World noe -Label real-rain-matrix
```

Individual named arms use `-SingleArm SoilFilm`, etc. For separately scheduled
arms, pass the **same actual selected coordinates** from the first result as
`-TargetX ... -TargetZ ...`. Omitting them selects a pool independently in each
separate campaign and cannot establish a same-location comparison. Custom
controls require `-SingleArm Custom -Film 0 -Water 1 -SSR 1`; flag parameters
are refused with named arms so an apparent override cannot be silently ignored.

World labels cover original Abel/Malden, Cain/Kolgujev, Eden/Everon and
Noe/Nogova. Default Noe soil coordinates `4975/4675` require live `dev_mud`
source eligibility and the exact `o\pole2.paa` texture. Other worlds or changed
soil coordinates are explicitly **unverified naked-ground film controls**.
They establish neither soft-soil source admission nor all-map appearance.
Supply land coordinates and pool search coordinates appropriate to those
maps; a missing supported positive wet target fails rather than inventing one.

## Measurements and refusal conditions

Actual landscape rain, not particle density or forced wetness, creates the
water. The runner disables particles/snow, clears inherited rain, wetness and
Reforger overrides, sets June 21 1985 at 16:00 and starts simulation time 100.
It advances real simulation by a requested 160 seconds of rain, then 128
seconds without rain and 128 seconds in clear-day drying. The actual start/end
times and liquid-rain readings are retained; polling can overshoot slightly.
The fresh initial field must be dry. Rain must actually stop, budget mass must
balance, and retained global volume must decrease while rain volume stays
unchanged. Sunny evaporation must increase.

Exposure, brightness, wind, terrain UV jitter, temporal sampling and grass
are controlled. Profiles use `Write-BenchmarkGraphics.ps1`, 1280x720, MSAA4,
DLSS OFF and a 60 FPS cap. Those controls reduce noise; different processes
can still have different actual weather and field state. **Cross-process PNG
differences are descriptive, not pixel-perfect causal proof.** Compare the
weather, depth, time and field metadata before attributing a difference.

The first arm searches real runoff samples near `2675/5125` and binds the
deepest positive supported pool. Its world coordinates are retained in all
later arms. Every wet arm must have positive water at that same point. Every
PNG receives fresh local samples before and after capture, including actual
`GLandscape::SurfaceY` support, depth, water height, flow and revision. A support
mismatch of 15 cm or more fails. Soil cameras derive height from a fresh
physical terrain sample. No synthetic pool or footstep stamps are used.

At each stage the runner captures soil close/top/grazing and water
oblique/top views, with immediate repeats and camera returns. Paused time,
field budget/generation/revision/pending time and local water must remain
exactly unchanged. PNGs retain SHA256, camera and local state. Later locally
dry pools are explicitly labelled `localWaterPresent=false`; global retained
volume does **not** prove water remains in that photograph.

Process stdout and stderr are both redirected, alongside the engine log.
Source proof requires actual terrain accumulation and matching uploaded
wetness (zero in OFF, matching the accumulator in ON), positive accepted grid
uploads for water ON, matching explicit SSR policy, and a ready pre-rain SSR
source for combined SSR ON. Disabled water intentionally emits no positive
grid row: its proof combines exact flag provenance with absence of active
uploads, not a fictional OFF announcement or fragment counter. SSR readiness
does not prove a hit at every pixel.

The runner refuses an existing game, uses only its owned process, requires
normal exit through `Assert-CaptureLifecycle.ps1`, and checks the installed
pair before/after every arm and campaign. Failed campaigns remain failed.
All controlled environment values are restored; previously absent variables
are removed explicitly for PowerShell 7.6/.NET 10 null semantics.

## CPU checks and remaining acceptance

`-SelfTest` runs before lock/process/profile/network access. It exercises the
actual runner's arm selection, mass budget, exact pause state, local support,
source admission, terrain/grid/SSR trace refusal paths, invariant camera
formatting, and null-safe environment restoration. PowerShell 5.1 and bundled
PowerShell 7 passed 30 helper checks; no game or GPU was used.

The installed five-arm campaign, photograph review, and any subsequent
production fix remain pending. If combined sky reflection is visibly paler
than either interface independently under suitably similar actual states,
the next bounded candidate is to suppress duplicate cosmetic film under a
positively supported physical water layer while retaining wet-soil absorption.
Do not compensate blindly with global tint or saturation.
