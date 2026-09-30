# Nogova boot-contact fixture

`scripts/Test-MudFootprints.ps1` owns only a fresh locked installed process, a private profile and the authored `tests/perf/missions/perf_mud.noe` mission. It never deposits or stamps mud through the harness. Start it with the existing `scripts/with-game-lock.sh`; a foreign OpenPoseidon/ColdWarAssault process blocks launch. Normal exit, shutdown marker, unchanged deployment stamp and both EXE/DLL hashes/timestamps/sizes are mandatory. Both console streams are retained, and absent inherited environment values are removed explicitly on restoration (including PowerShell 7.6/.NET 10).

The audited X4975/Z4675 `o/pole2.paa` candidate remains a candidate until installed probes confirm exact world/source admission. A stock WB AI soldier walks through ordinary `doMove`, following the established SnowCrawl movement path. Fixture placement between arms does not count as contact evidence. Dry walking must move without changing mud history. An outdoor, 20-simulation-second snow-particle override must wet neither soil nor uniform when actual landscape rain is dry. The stock CampEastC roof then covers the central walking segment while liquid particles wet the global soil history. Covered walking must actually move without depressions or cloth wetting. Removing the roof permits exposed walking, which must produce a changed revision/chunks, actual source-identifying `MUD_STEP` rows, and increased `triUniformWetness` with actual landscape rain still dry.

Before walking, a fixed 12.5cm grid samples physical `GLandscape::SurfaceY`, physical gradients and signed `MudField` values. After exposed walking, support-height differences must equal signed mud-offset differences within 0.06mm; gradient differences must equal mud-gradient differences within 0.0001. At least one depth exceeds 1mm and at least one physical gradient changes. Samples deliberately avoid cell centres so slope checks exercise interpolation rather than a trivially flat cell. A paused camera excursion/return must preserve clock, weather authorities, actor transform/pose, cloth state, mud history and every physical sample exactly. PNG dimensions, hashes, camera strings and sampled state are recorded. Images need root's installed inspection; successful CPU queries alone do not accept rendered geometry or appearance.

## Diagnostic contract

### Installed fixture corrections

The matched installed01e8c4fb attempts are retained under `build/mud-footprints`.
`noe-current` failed on an unsupported `enableAI` operator. The engine exposes
`disableAI`, so each control now recreates a fresh actor with explicit setPos.
`noe-walking` failed placement; `noe-placed` failed because a subordinate followed
player formation. A separate leader group fixes ordinary dry walking: the
`noe-independent` actor travels4.220m on the actual Nogova pole2 source without
deforming dry soil. That run then fails its covered movement control.

Read-only inspection of actual stan_eastC ODOL7 reveals a solid Geometry hull,
including a bottom quad, despite the visually floor-free central roof. At its
original placement the soldier is inside the hull and is pushed outside while
wetting. This does not test sheltered boot contacts. Mud now requests3m of
optional roof lift; other roof captures retain original placement. The authentic
overhead roof stays within the30m production rain probes, while its hull clears
the soldier. The first lifted `noe-overhead` run keeps the actor on the exact
grounded start through wetting, but its1.6m doMove destination is too close to
induce walking. The corrected control requests the full lane and pauses after
actual measured0.7m; its covered endpoint remains independently bounded.

Every sampled actor state now queries physical terrain at the actor's actual
coordinates and rejects support gaps above3cm. Production UNIFORM_WET tracing
is enabled. Captures and actual movement are still required; no failed run is
accepted as proof of wet contacts or volumetric appearance.

- `dev_mud {action:"state"}` reads enabled, wetness, chunks, revision and rejected count. `action:"sample",x,z` additionally reads signed offset, mud/physical gradients, physical height and actual world/texture/surface metadata. Coordinates must be finite and bounded to ±1,000,000m. `sourceEligible` means the exact terrain source admission only; it does not assert slope, road, snow, roof or actual boot contact acceptance. No mutation action exists.
- `weather_particles {action:"state"}` is read-only. `action:"set"` accepts mode `off|legacy|particle|both`, finite density in [0,1], and Boolean snowflakes; at least one field is required, and the complete candidate validates before either rain producer is changed. These are private-game diagnostic controls, not a gameplay weather preset or synthetic wetness setter.
- `weather_visibility` retains existing fields and appends `particleDensity`, `particleSnowflakes` and `liquidRain` from the shared `UniformWettingDensity` contract. Actual landscape rain remains independent. This exposes the Nogova owner gap where visible liquid particle overrides previously failed to wet uniforms.

`scripts/Test-MudFootprintHelpers.ps1` extracts and tests the actual runner functions without loading the installed game or opening process/network/GPU resources. It exercises finite/type/range rejection, missing source admission, physical height/gradient agreement, exact paused state, actual trace grammar and stamp/pair changes. These checks do not establish installed contact admission or rendered appearance. No installed run is claimed by this source-only fixture commit. Hard road/grass/border and real snow-ground controls remain explicit installed gaps; no suitable audited hard-ground pose was available for automatic inclusion.
