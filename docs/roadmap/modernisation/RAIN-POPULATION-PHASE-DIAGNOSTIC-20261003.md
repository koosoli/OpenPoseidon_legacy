# Rain population/phase diagnostic candidate

`Test-RainPopulationPerformance.ps1` is an installed-game, source-only runner.
Its self-test passes; no game was launched by the source agent.

It runs three separate ordinary stock Nogova missions with the same private
1280x720 MSAA4 profile and June 21 16:00 weather: ground bed+2m, absolute altitude
117.58m and absolute altitude 300m at X/Z4975/4675. Each is an unpaused stationary
camera, with 15 simulation seconds warmup and 25 simulation seconds measurement.
The physical player remains local/alive; this is not occupied helicopter flight.

Default `Forced` uses the existing `POSEIDON_TEST_RAIN=1` hook (RainBoth, including
legacy overlay), usable on the installed8aca build. `Natural` clears that hook
and needs the newer exact `POSEIDON_RAIN_TRACE=1` diagnostic. Both require measured
actual liquid rain >=.95 and a <=.02 range across all altitude arms. Unsupported
trace, insufficient actual rain, partial trace grammar, missing population or
short sample windows fail rather than producing a performance claim.

The result records every complete RainVolume row and medians/minima/maxima of
live/drawn/spawned/kill/gather counts plus spawn/gather/simulate/draw microseconds.
These timings describe the last update at each once-per-simulation-second log,
not a mean over every frame. Counts accumulate until the diagnostic reset; no
GPU timestamp, overall frame budget or helicopter-motion attribution is claimed.
Installed EXE/DLL/stamp hashes, script/helper/mission/profile provenance, exact
camera poses, actual weather and simulation intervals, and normal exit metadata
are retained. Game ownership, hashes and lifecycle are checked. Forced cleanup
is recorded as failure. Environment is restored.

## Existing unedited native pool drainage evidence

Source record: `build/rain-water-natural/unedited-original-hollows-20261001-062817-9a1851/result.json`.

| Arm | No-input simulation interval | Volume start/end (m3) | Infiltration / evaporation / outlet removed (m3) |
| --- | --- | --- | --- |
| OFF |121.329s|662988.3918024357 /474537.33057307824|23371.653263914915 /11559.011764720839 /153520.1334457628|
| ON |122.230s|530516.1597868708 /378583.8748037167|22514.611172316363 /11123.599466484939 /118294.07437629477|

Both have exactly zero cumulative rain-input change during those intervals and
actual stopped liquid rain0. The losses balance the volume change. This proves
global no-input drainage, not visible local puddle disappearance.

At the candidate X/Z4975/4675, wet local depth was .0060831098817288876m OFF and
.004464090336114168m ON. The ON view instead selects a remote owner at
X/Z4884.375/9046.875, with local depth1.9818910360336304m,
bed197.9876708984375m and head199.9695587158203m. Its held owner pool is genuinely
deep according to the native diagnostic; its grey appearance is not evidence of
a millimetre film. The existing runner does not resample this depth, the candidate
depth or owner-held volume after draining, so their local changes are unknown.

That campaign fixes June21 16:00, stops rain with overcast.6 and sets
`WGR_CLOUD_COVERAGE=0`. It does not record actual sun visibility or the evaporation
rate at the chosen local pool. A sun-controlled local-depth/owner-volume time
series is required before changing drying coefficients or claiming sun drying
is correct. No coefficients were changed in this diagnostic work.
