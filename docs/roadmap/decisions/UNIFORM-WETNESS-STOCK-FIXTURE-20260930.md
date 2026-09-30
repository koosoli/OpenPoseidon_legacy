# Stock uniform wetness installed fixture

`scripts/Test-UniformWetness.ps1` checks actual Person state through the readonly
`triUniformWetness <object>` scalar. It creates two real nonplayer SoldierWB
actors: exposed at 9475.25/3018.25 and covered beneath the audited floor-free
CampEastC tent at 9486/3006. The shared stock roof helper verifies actual runtime
terrain heights, stock class/model, ASL placement and deletion. Each soldier is
placed explicitly, stopped, movement/target AI disabled and put in the existing
`civil` fixture pose, facing 180 degrees. Every sample verifies alive,
actual ASL within 2 cm, heading and `getMove`; cameras derive from actual ASL.
This checks authored state and world transform, not a live affine-palette proof.

The runner uses no wetness setter or injected rain override. It removes
`POSEIDON_TEST_RAIN`, applies ordinary SQF weather, and reads the actual
GLandscape density via `weather_visibility`. New dry actors must return at most
0.001. Exposed wetness must exceed 0.2 after at least 20 actual simulation
seconds with actual rain at least 0.3. The independently covered actor must stay
at most 0.001; a merely dark indoor screenshot cannot establish this state gate.
Unknown/invalid query values fail; objNull and non-Person roof must return -1.

After wetting, the runner pauses simulation, captures both actors, moves the
camera between them and returns. Actual wetness and rain must remain exactly
equal, with simulation time within 1 ms. Rain is stopped through ordinary SQF,
and actual rain must reach at most 0.02. At least 120 further simulation seconds
of dry weather must lower exposed wetness by more than 0.02 while retaining a
positive value. This observes gradual history, not an immediate material reset.
The simulation defaults to 4× acceleration and has bounded wall-clock deadlines.

`POSEIDON_UNIFORM_WET_TRACE=1` must produce actual simulation and material traces.
The material gate requires the precise stock WB owner/atlas/mask:

```text
UNIFORM_CLOTH model=data3d\mc vojakw2.p3d texture=merged\00007mc_vojakw2.paa kind=2 enabled=true wet=0.2001 encoded=2.2001
```

Paths are normalized only for slash direction/case. The trace's kind must remain
2, its enabled flag must match the renderer arm and encoded must equal 2+the
traced wetness on, or zero off. The producer logs wetness decile transitions,
so the row must be positive and in the authoritative paused scalar's current
band, allowing only its four-decimal textual rounding. It does not falsely
require the transition row to equal a later exact paused scalar. The encoded
value is checked against the value actually printed in that producer row.
The scalar query separately establishes the exact captured simulation state.

`-WetOff` sets `WGR_UNIFORM_WET=0`, affecting clothing rendering while preserving
the real rain/history/shelter/query checks. A normal run explicitly sets it to
1. Each run records actual time/weather/wetness, all source traces and pictures;
independent natural runs can stop at slightly different wet values. Before
accepting a visual ON/OFF difference, compare actual recorded parameters and
poses. Dry/wet screenshots alone include real weather lighting changes and do
not establish cloth-only shading or protected face/gun appearance. Those require
the source atlas mask/owner exclusions, its numeric shader tests and actual
matched clothing/face/weapon regions in the ON/OFF captures. No semantic cloth
ROI or speculative pixel pass threshold is added here.

Private profile, lock, installed stamp read first, binary hashes/timestamps,
owned normal exit/shutdown marker and unchanged installed files follow the
other installed runners. Snowline/object snow, grass, temporal AA, clouds,
wind, terrain jitter and auto exposure are pinned; puddles are disabled.
Actual rain/fog remain validated. Actors and tent are deleted before normal
exit; only the runner's own process can be stopped after failed graceful exit.
Environment is restored and evidence retained under ignored
`build/uniform-wetness/<label>-<time>-<id>/`.

Root owns integration, build, deploy and serial installed execution:

```bash
LOCK_OWNER="stock wet uniform ON" scripts/with-game-lock.sh pwsh -NoProfile -File scripts/Test-UniformWetness.ps1 -Label wet-on
LOCK_OWNER="stock wet uniform OFF" scripts/with-game-lock.sh pwsh -NoProfile -File scripts/Test-UniformWetness.ps1 -WetOff -Label wet-off
```

`-LegacyCwa` supplies an optional classic Eden campaign with the same runtime
height/identity/state gates. Capture success is reported as
`state-verified-captures-not-visually-accepted`, not appearance acceptance.

Pure checks extract the runner's actual helpers through the PowerShell AST:

```powershell
./scripts/Test-UniformWetnessHelpers.ps1
./scripts/Test-TerrainPuddleStockRoof.ps1
```

They exercise normalized scalar/type/nonfinite/refusal cases, SQF literal
backslashes and doubled quotes, ASL arrays, actual rain/fog bounds, stationary
pose/heading/position checks, culture-independent cameras, exact paused state
and strict actual WB atlas/kind/encoded ON/OFF telemetry. This contribution
contains scripts and documentation only. No runtime build, game/GPU launch,
deployment or visual acceptance was performed by this subagent.
