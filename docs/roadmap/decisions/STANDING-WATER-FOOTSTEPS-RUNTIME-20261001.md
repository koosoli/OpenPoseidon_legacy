# Ordinary player standing-water audio fixture

Baseline: `4ee9da4a` on the current streaming-residency source. This adds only
`scripts/Test-StandingWaterFootstepsRuntime.ps1` and this evidence document.
The existing `POSEIDON_WATER_STEP_TRACE` already supplies the needed actual
consumer evidence; no actor, audio, contact or water production source changes
are necessary for the fixture.

Run the installed test serially through the shared game lock from the root
integration checkout:

```powershell
bash scripts/with-game-lock.sh pwsh -NoProfile -File scripts/Test-StandingWaterFootstepsRuntime.ps1 -Label stock-water-steps
```

`-GameDir` selects the existing installation. Optional `-PoolX` and `-PoolZ`
select coordinates to query, never water to seed. Both must be supplied. The
default read-only search covers 121 actual field-grid points near the stock
Noe player site, then checks up to 12 shallow candidates along a 7 m northward
lane with a 1.2 m wide sample footprint. `-SearchX`/`-SearchZ` also move the
initial dry-walk fixture. These are candidate locations, not claims that a
pool or collision-free passage exists in an untested installation. A blocked
ordinary walk or unavailable supported pool fails with retained evidence.

The script requires the lock and refuses an already running game. It reads
`DEPLOYED-FROM.txt` before checking the executable/DLL hashes, uses a private
profile and mission with one actual standard `SoldierWB`, retains serial TCP
requests and replies, validates PNG signatures/hashes and paused capture time,
and requires normal exit zero and `Shutdown complete`. It cleans up only its
own process and verifies the installed pair after the run. No deployment,
foreign-process termination, synthetic stamp, AI `doMove`, forced animation,
forced sound call, or water setter occurs.

The stages establish distinct facts:

1. Fresh dry field volume is zero. An actual four-metre SDL W walk produces
   fresh normal audio contacts, available waves, dry support refusal and no
   water selection. The initial route heads east to avoid the known stock
   ruin north of the sand spawn; actual movement remains required.
2. Real overcast/rain at time acceleration eight creates stored water. The
   particle override is disabled and actual `liquidRain > 0` is queried.
   Read-only field samples must show at least 3.5 mm depth, a bed above sea,
   water above actual terrain and less than 15 cm bed/support mismatch. The
   shallow candidate cap is 15 cm; this is a walking fixture, not swimming.
3. A six-metre ordinary player walk must emit at least four actual
   `standing-rain-water` events with `env=water`, available waves, sole age
   at most 250 ms and the actual support/sole-height admission bounds. The
   `support` value comes from production `RoadSurfaceY`, not the candidate
   terrain-height inference. A return walk can observe a missing alternative
   naturally; the test does not alter random selection or force playback.
4. The verified stock `CampEastC` / `stan_eastc.p3d` fixture is suspended
   three metres above the same actual wet terrain. This preserves its
   floor-free central visual roof while clearing its solid Geometry hull
   from walking, as established by the preceding real mud fixture. A slow
   0.7 m manual walk must still produce fresh supported wet contacts, now
   refused specifically as `sheltered`, with normal non-water waves. Body
   and sole positions must remain in the bounded central roof region.
5. The real roof is deleted and verified absent. Another six-metre manual
   walk must restore actual standing-water selection. A five-second pause
   with a captured image must leave clock, field generation/revision/volume
   and audio row count unchanged.

`People\water_L` and `People\water_R` are the audited inherited retail
`SoundEnvironExt.water` alternatives. The production getter selects them
randomly; their names **do not identify which anatomical foot just contacted**.
The runner requires both names to be observed, but its evidence makes no
anatomical pairing claim. Exact source paths and real model names remain in
each retained event. Rounded trace height tolerances account for the existing
three-decimal height/four-decimal depth log and do not change production gates.

The final passing status deliberately reads
`actual-water-audio-selection-passed-listening-required`. `wave=true` means
the selected existing cached wave was available and its existing Restart path
executed. It does not prove that the listener heard the correct mix. No loopback
WAV is recorded, no gain is changed, and the inherited retail water volume
`0.0000316227706` is retained. Installed listening should confirm the wet step
replaces the normal ground step without an extra overlapping step, at both
internal and external listener positions. Screenshots establish neither sound
audibility nor water appearance acceptance.

Runtime sea wading, dry raised roadway/floor, missing authored water sounds,
AI contacts and map reload remain separate gaps. They must not be inferred
from absent trace rows. The canonical `Test-FootstepWater.ps1` already executes
the actual production admission/event helper and RainWaterField for those
policy refusals, freshness/reset and 500 read-only queries; it also checks the
real animation capture and audio consumer wiring. That is CPU/source evidence,
not installed listening acceptance.

Author checks: PowerShell parser has zero errors; runner `-SelfTest` passes
trace decoding, support/age/variant falsifiers, nonfinite/unknown-field
rejections, pool depth/sea/raised-support refusals and SQF decoding. Canonical
`Test-FootstepWater.ps1` passes with `-Werror`. No game, GPU test, full game
build, deployment or installed audio run was performed by this agent. Root
owns the serial installed run and subsequent result reconciliation.
