# Mud puddle stability and vertical rain admission — 30 September 2026

The owner has deferred manual visual review and authorised continuing with our
best judgement. Appearance preference is therefore future tuning, not a blocker
to automated correctness work. This follow-up starts from `a001b55f`; it retains
the authored material eligibility, weather accumulation, mask, contrast and
reflection choices already installed. The root agent owns serial game captures,
builds, deployment, performance campaigns and the authoritative status ledger.

## Corrected rain-cover policy

The existing admission used `interior_sky_reach`, a diffuse cosine-weighted sky
dome query. With the zenith blocked and its four 50-degree tilted directions
open, reach is `4*cos(50)/(1+4*cos(50)) = 0.71998`. The puddle admission
`smoothstep(0.6,0.95,reach)` consequently retains roughly 27% of the puddle mask.
A canopy can thus admit puddles because light enters from the sides. This is a
source-policy counterexample; it is not a measured native roof image defect.

Production commit `e6c26986` adds `interior_rain_reach` to the shared frame module.
It selects zenith depth layer 0 with an upward receiver normal, reusing the
existing comparison sampler, geometry bias and nine-tap filtering. Terrain uses
this helper only for wet, eligible puddle pixels. The diffuse AO query, five sky
directions, ambient strength and ambient floor remain in existing shading. Rain
query cost falls from five directional kernels to one; this is a construction
bound, not a timing result.

Limitations are explicit: disabling the map returns open; receivers outside its
depth/UV coverage return open; the map border feathers toward open. The map has
a finite camera-centred footprint and cached layers. Its kernel/bias still shape
the roof-edge transition. This cosmetic zenith query does not establish wind-
driven rain, geometry collision, universal roof coverage or terrain hydrology.

The focused GPU test imports the **actual production helper**, reflects the
actual Frame uniform offsets, binds five synthetic directional depth layers and
reads the helper's output. No available adapter means test failure, not success
by skipping. The falsifiers cover:

- Zenith blocked/all four sides open: rain and admission zero while diffuse
  reach remains around 0.72, including strength zero and ambient floor one.
- Zenith open/four sides blocked: rain and admission one while diffuse reach
  is below 0.3.
- Disabled map, outside depth coverage and map border: documented open fallback.

`cargo test -p wgpu_renderer --offline shaders::rain_cover_tests:: -- --nocapture`
passed both actual-helper GPU and composition tests (no skips). The terrain suite
passed all 18 tests, including its real shader composition. Logs are local under
the isolated worktree's `build/terrain-puddle-policy/`. These tests do not certify
that a particular native roof is registered/rendered in the depth map. Root must
build/deploy the correction and run installed smoke before runtime acceptance.

## Controlled camera-return harness

`scripts/Test-TerrainPuddleStability.ps1` launches the installed matched pair in
serial dry, wet and master-off arms. It requires the shared game lock, refuses
pre-existing game processes, creates private profiles, records the deployment
stamp before binary reads, hashes both binaries before and after, records all
TCP requests/replies and validates every PNG. Normal harness exit and shutdown
markers are required; failure cleanup stops only the process object it owns.

Diagnostic controls pin wetness to zero/one while actual rain and fog are zero.
These arms test appearance differences and camera returns, not the independently
validated natural rain/drying lifecycle. The harness freezes simulation time at
100 seconds and sets the same date, disables temporal jitter and adaptive
exposure, fixes manual exposure and user brightness, and restores process
environment values afterwards. These diagnostic choices never alter the owner's
persistent profile. Installed telemetry must confirm the selected uploaded
wetness and frozen time, and the geometry sky map must be enabled.

Default poses use the previously validated native Dirt_02 and negative
BeachGrass locations:

| View | Camera X Z absolute-height azimuth elevation |
|---|---|
| Dirt_02 | `9481.25 2994.25 209.72 0 -18` |
| Overhead sweep candidate | `9481.25 3006.25 249.72 0 -85` |
| Negative BeachGrass/shore | `2756.25 2369.25 10.44 0 -18` |

Each arm captures same-pose static repeats, translates the camera and returns
to the exact initial pose. The overhead view is shifted 25 metres and returned.
The native harness has no source-material query, so the visible boundary is a
candidate pending independent material-identity confirmation; the harness does
not manufacture a proven boundary from an image difference.

The C# helper measures the central 60% of image width/height. Absolute RGB
differences include all causes, not only puddles. A causal wet-minus-dry mask
uses an inclusive maximum-channel threshold of eight display levels. Return
mask IoU is measured between matched initial and returned dry/wet pairs. Empty
masks are explicitly not evidence of stable puddles. Static repeats and
master-off comparisons expose uncontrollable wind, loading or lighting changes.
No PNG hash equality is claimed as an appearance requirement.

Initial diagnostic gates are:

- Same-pose static mean difference at most 0.75 display levels per channel.
- Master-off versus dry mean difference at most static mean plus 0.75.
- Visible Dirt response in at least 0.1% of the selected image ROI.
- Dirt and overhead return IoU at least 0.95, nonempty response and mean
  return image difference at most static mean plus 0.75.
- Initial/repeated/returned negative response mean at most its static mean plus
  0.75, changed fraction at most 0.5%, and return image stability.

These are operational screening tolerances and are recorded with actual values.
Failed or noisy gates are `inconclusive-or-failed`, never a rendering verdict.
Default exit success means captures completed; use `-RequireNumericGates` to
require all gates for process success. Both modes retain `result.json` and all
captures. A passing numeric cut does not establish owner preference, all-frame
glint absence, full-map material correctness or performance acceptance.

Run from root's current checkout, using its current installed pair:

```bash
LOCK_OWNER="puddle stability" scripts/with-game-lock.sh pwsh -NoProfile -File scripts/Test-TerrainPuddleStability.ps1 -Label native-return
```

The pure metric test passed ROI exclusion, inclusive threshold, RGB mean,
identical/relocated/empty masks, dimension refusal and invalid threshold refusal.
Both new PowerShell scripts parse without syntax errors. No installed game
capture was run by this subagent.

## Real roof fixture still needed

Source inspection found no existing validated native roof-over-eligible-dirt
fixture in the available scripts or mission evidence. The prior lighting plan's
synthetic room validates its model-space sky bake, not these terrain pixels.
Concrete building floors also hide the terrain and cannot falsify hidden puddles.
A useful fixture needs a roof/canopy without a floor, known eligible dirt visible
beneath it, an exposed dirt control and evidence that the actual roof enters
zenith depth rendering. An existing retained canopy or authored roof placed over
the validated dirt patch is a defensible next fixture; arbitrary dark interior
stills are not acceptance.

Optional `-RoofCamera`, `-RoofExposedControlCamera` and `-RoofFixtureEvidence`
parameters add all three material arms plus a diffuse sky debug arm. The evidence
document must exist and is hashed. The output stays `captured-not-accepted`
until terrain pixel regions and roof geometry are verified. The greyscale debug
image includes other surfaces and display tonemapping, and visualises diffuse
reach rather than the new vertical rain query. Its brightness statistics are
image diagnostics, never raw rain-cover values. Without a real fixture, output
reports roof acceptance `unavailable` while the authorised automated work continues.
