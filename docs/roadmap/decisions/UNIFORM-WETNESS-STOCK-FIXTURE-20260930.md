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
and strict actual WB atlas/kind/encoded ON/OFF telemetry. These pure checks do
not substitute for the installed results below.

## Installed acceptance, 2026-09-30

**SHIPPED for the bounded stock SoldierWB slice.** Oliver delegated current
appearance judgement and deferred his own fine tuning. Root built/deployed the
matching `codex/streaming-residency e0bb256a` pair and ran both locked private
profile controls with normal exit code 0 and `Shutdown complete`. Before/after
installed hashes and stamps match in both result records:

- OpenPoseidon.exe SHA256 `2081E39256E02D92CAAAFFCC3BD2EBDA06568EEFBF495022BD86C3875DBDCED6`.
- wgpu_renderer.dll SHA256 `7C26F82D6DDAA05AEC4E0C10DBB3B0121872929B6C2953245F3792DE07655728`.
- [Renderer ON result](../../../build/uniform-wetness/eden-fixed-on-20260930-171900-5c0375/result.json).
- [Renderer OFF result](../../../build/uniform-wetness/eden-fixed-off-20260930-172111-de3158/result.json).

| Actual recorded quantity | ON | OFF |
|---|---:|---:|
| Initial exposed / covered wetness | 0 / 0 | 0 / 0 |
| Wet paused simulation time | 35.428 s | 36.176 s |
| Actual paused rain density | 0.8394246 | 0.8112407 |
| Exposed wetness after more than 20 simulation seconds | 0.380630 | 0.391330 |
| Covered CampEastC actor wetness throughout | 0 | 0 |
| Wet pause and camera-return scalar/rain | Exactly retained | Exactly retained |
| Actual rain during drying | 0 | 0 |
| Drying duration | 122.944 s | 126.055 s |
| Exposed drying start → end | 0.378732 → 0.310226 | 0.389432 → 0.317373 |
| Final WB producer row wet / encoded | 0.3102 / 2.3102 | 0.3174 / 0 |

Both runs retain the same verified actor positions, `civil` pose, 180-degree
heading and cameras. The material rows establish exact primary stock model
`data3d\mc vojakw2.p3d`, `merged\00007mc_vojakw2.paa`, kind 2 and the effective
renderer switch. OFF preserves rain history and shelter while removing only
the encoded material effect. Four actual CPU integrator/admission test cases
and the actual shader's 28 GPU probe cases pass, including piecewise sRGB
linear hue/brightness, hand/face-atlas exclusions, pelvis continuity,
minification and the unchanged 128-byte material tail layout.

Root and the reviewing agent inspected the wet exposed pictures and sheltered
controls. Cloth darkening is subtle and consistent with retained moisture;
the captures show no visible face/hand/weapon repaint or plastic shine. The
production effect changes RGB only, reducing fully wet cloth's reflected
linear light by at most 22%; **no wet gloss, specular or roughness change was
added**. Coverage, alpha, geometry, depth and the shadow/prepass are retained.

This is qualified appearance judgement, not exact pixel isolation: independent
natural runs paused at slightly different times, rain densities and stored
wetness. Authored state/world transforms match; live affine palettes are not
certified by the fixture. Dry-versus-wet pictures additionally include real
weather lighting changes. No whole-frame difference threshold, unchanged-face
pixel certificate or broad texture acceptance is inferred from these images.

## Failed controls retained and corrected

The earlier native terrain control failed RPC JSON parsing before it established
the required state. The first classic Eden attempt timed out; a later attempt
failed the authored stationary-pose gate. These remain failed runs rather than
being converted to passing evidence by relaxing gates.
The successful fixture uses the existing `civil` pose with verified ASL/heading.

`eden-on-idle-20260930-170647-c709f4` exposed a real shelter defect: the CampEastC
actor reached about 0.226 wetness while the outdoor actor reached about 0.218,
and production traces reported `sheltered=false`. Installed `Stan_eastC.p3d`
has an explicitly empty FireGeometry LOD (7e15, zero vertices); its ViewGeometry
LOD (6e15) has 80 vertices and seven components, including a central roof at
model Y 1.689765. An empty authored fire LOD suppresses the engine's absent-LOD
fallback. The Person correction preserves positive fire-ray hits, then checks
the same upward segment against view geometry when fire misses, still ignoring
the soldier. Bullet-permeable tents and glass can therefore stop rain without
changing global bullet collision semantics. The installed ON/OFF controls above
prove the corrected CampEastC actor remains dry.

## Scope retained

Wetness is cosmetic and mission-local. New/reset Persons start dry; the first
non-unit-status load pass clears reused history. It adds no wetness setter,
save/network fields, cloth physics, gameplay modifiers or new production ABI.
Pause/repeated rendering does not advance history. Actual simulation seconds
drive gradual wetting and drying; roof exposure is sampled at most once per
simulation second with the actor excluded from both collision queries.

WB lifecycle and delegated appearance are installed accepted. Exact EB atlas
and named pilot/tank/resistance cloth admission have source/helper coverage,
not an equivalent installed campaign. Unknown/wounded replacement textures,
alternate/modded models and other LOD atlases remain unchanged unless explicitly
admitted. Far baked whole-body atlas regions are deliberately excluded to
protect their baked skin. No universal asset support, save/network persistence,
all shelter geometry or general performance claim is made.
