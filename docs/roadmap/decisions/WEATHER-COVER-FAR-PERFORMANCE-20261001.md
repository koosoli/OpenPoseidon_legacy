# Far physical-weather cost fixture

`scripts/Test-WeatherCoverFarPerformance.ps1` is an installed-game measurement
candidate, not a performance or visual acceptance result. It adds no renderer,
world, shared profile-helper or existing forest-capture changes. Actual device
and installed execution belong to the root's serial runtime campaign.

Run through the existing lock with the exact installed deployment commit:

```powershell
scripts/with-game-lock.sh powershell.exe -NoProfile -File scripts/Test-WeatherCoverFarPerformance.ps1 -ExpectedCommit <installed-commit> -View Eden500
```

`-View Top` measures the second fixed scene; `-View Both` measures both serially.
Defaults are three fresh-process repeats per OFF/ON/ON/OFF block, 35 wall seconds
of warmup, native 1280x720, MSAA4, DLSS disabled, vsync disabled and uncapped FPS.
Warmup is bounded to 35..45 seconds. Each launch has exactly one screenshot and
metrics readback. Multiple scheduled screenshots are unsuitable for timing;
`farfield-bench.ps1` marks that mode performance-incomparable.

The cameras are `5025 3725 144.54546 0 -8.53` (the audited Eden 500m view) and
`5025 4220 127.5 0 -86` (top). The stock `perf_field.eden` mission and installed
`Worlds/eden.wrp` are used through the mission's original-map loader. No
`--test-world` substitution is passed: that route calls `LoadOprwModern`, which
supports OPRW revisions 18/20/24/25 and refuses original OFP worlds. Camera and
hour options are passed directly through farfield's `ExtraArgs`; positive
`WarmupSeconds` still selects its wall-clock timed capture. Each block gets a new private profile through
`Write-BenchmarkGraphics.ps1`, with brightness 1; the original authored mission
seed/date are retained and CLI hour is 16. Simulation is not frozen by this
wrapper, so these are fixed startup conditions and camera poses, not an exact
cross-process weather/animation history guarantee. Zero wind, cloud coverage,
particle rain, terrain UV jitter and temporal processing are explicit. Grass,
cosmetic puddle film and physical rain-water rendering are disabled. Normal HDR
time-of-day grading and the installed default adaptation policy are used;
inherited exposure/tonemap pins are removed and restored afterwards.

Startup snow is supported without a separate developer harness: in
`World::InitLandscape`, `POSEIDON_SNOW_TEST_DEPTH=0.18` resets and enables SnowField,
disables falling snow, and deposits the specified depth. The runner requires an
actual `wgpu object snow: enabled=1 deposit=0.1800` upload with altitude snow
disabled, plus retained populated near-map ready/submitted trace evidence.
ON additionally requires the far map's actual 2048/1600 ready/submitted trace;
OFF refuses any far-map publication row. These bounded startup traces prove
activation, not publication state in the final timed frame or selected owner/LOD
identity. Fixed cameras keep the source's first-32-or-changed-view trace bounded
and ordinarily outside the warm measurement interval. A timing regression should
still be rechecked with tracing disabled after a separate activation proof.

Near precision remains 640m half-extent at 2048 resolution. The far pass adds a
2048 Depth32Float allocation: exactly 16,777,216 texture bytes by source. This is
not a measured total VRAM delta; indirect buffers, allocation overhead, transient
resources and residency require separate accounting. A 1600m square half-extent
also does not establish coverage of every configured 2000m view-distance ray.

Every result is attributed to SHA256 hashes of the installed EXE, DLL and full
deployment stamp, plus the wrapper and reused helpers. The pair is checked before
and after every block. `Assert-CaptureLifecycle.ps1` requires normal exit 0 and
`Shutdown complete`; timeout/forced cleanup, contention, validation/device errors,
missing timestamps, unsettled profiler rings and absent source activation fail the
campaign. The existing farfield process harness owns all launching, waiting and
cleanup. The wrapper has no alternate process harness and never upgrades a forced
exit to success. Environment restoration removes an originally absent variable
through the environment provider, including bundled PowerShell 7/.NET10.

Counters must be valid/populated and registered instances, main instances, main
records and main triangles must match exactly across the four blocks for each
view. Differences remain a failed comparison needing diagnosis, not a relaxed
threshold. The retained JSON contains whole-frame GPU times, all CPU frame-phase
data, object counters, lifecycle metadata, PNG hashes and per-view ON/OFF medians
with raw samples. Both order reversals and process repeats are retained so drift
and between-launch spread can be inspected rather than hidden by one difference.

There is no dedicated weather depth-pass timestamp today:
`render_interior_sky_pass` calls both physical-weather passes before
`timers.begin(InteriorSkyDraw)`. Thus the artistic interior-sky region excludes
their depth work. The reported quantity is the measured **GPU frame total**
differential, together with CPU phase/cull/submission evidence, not an invented
far-map regional cost. High camera poses also do not prove near-camera fine snow
mesh refinement or geometry-only cost. The runner imposes no fabricated shipping
budget and does not accept forest or roof appearance; those remain the separate
actual installed visual controls.

Source helper checks passed under Windows PowerShell 5.1 and bundled PowerShell
7. They reject contention, multiple readbacks, population mismatch, unavailable
GPU timestamps, NaN, missing/incomplete far publications and unexpected OFF-arm
publication, and check null-safe restoration and summary medians. No game, GPU,
profile mutation or runtime measurement was used for these helper checks.

The initial installed `6cf9973e` campaign at
`build/weather-cover-performance/far-cover-cost-20261001-040659-62773a`
failed. All three first-OFF launches exited normally before the 35-second
capture, with no PNG/metrics. Their logs explicitly report `Cannot load
--test-world`, zero registered/visible objects, and the freefly player grounded
at -98.2m on the failed empty landscape. This is a stock-loader routing bug,
not a measured far-pass cost or evidence of a too-long warmup. The wrapper now
uses the same ordinary original-map route as `Test-ForestSnowRuntime.ps1`.
Argument self-tests preserve the exact camera/hour/VD and reject incomplete or
nonfinite poses. The failed campaign is retained; corrected installed timing
remains pending.
