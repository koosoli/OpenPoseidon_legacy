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

### Root installed camera-return diagnosis

All three campaigns below use the clean installed `2a15d74e` pair, normal TCP
exits and unchanged binary hashes. The original gates remain unchanged; failed
campaigns remain failed in their retained `result.json`.

- `zenith-return-20260930-120204-26e7e6`: Dirt return IoU 0.99894 and
  overhead return IoU 0.97797 pass, but master-off and negative controls fail.
- `isolated-return-20260930-121118-ae5923`: excluding grass and pinning wind,
  cloud coverage and LOD range makes both return IoUs 1.0. Master-off and
  negative controls still fail. Bare-ground ROIs also show process differences.
- `fixed-uv-return-20260930-123228-02e4e3`: disabling diagnostic terrain UV
  displacement removes the remaining master-off mismatch (mean 0.252786 versus
  limit 0.750143). Geography seeds its terrain offsets from wall-clock RNG;
  fixing simulation time alone does not fix those detail-texture coordinates.
  Dirt and overhead return masks both have IoU **1.0**, with 58,862 and 44,722
  nonempty response pixels. Static mean difference is 0.000143; Dirt return
  mean is 0.002722; overhead return mean is zero. Dirt response is 17.7415%.
  The negative mean passes, but its changed fraction **1.2792%** exceeds 0.5%.

The negative image contains animated ocean/shore foam. Master-off changes are
confined to sea/shore rows 144..253. Wet/dry also includes a blue ground patch
under a bush (912 changed pixels), so ocean motion alone does not explain every
pixel. Runtime marks BeachGrass ineligible, but the image includes adjacent
source interpolation. A steeper, ground-only camera and source identity check
are needed before treating that image as a pure BeachGrass exclusion test.
No production RNG, texture detail, admission threshold or acceptance tolerance
was changed to make the controls pass.

`ground-only-return-20260930-125154-e4f920` uses the same installed `77299a74`
pair and changes the old shore camera elevation to -80. All unchanged numeric
gates pass: both return IoUs 1.0; master-off mean 0.000656; negative mean
0.261800 and changed fraction 0.49913%. This view removes the ocean confound,
but remains a mixed-source control. A read-only stock terrain-grid audit finds
the original camera above Dirt_01, with BeachGrass 12 metres ahead and Dirt
again beyond it. The tiny admitted dirt patch is therefore not evidence that
BeachGrass itself is wet. A source-backed ineligible cell and queried height
are the next negative fixture; passing this mixed image is not a material
segmentation claim.

`pure-beachgrass-return-20260930-130805-79d16b` passes every unchanged gate on
`77299a74`. The source-backed camera uses 2781.25/2381.25, queried height
5.5625 +5 metres, elevation -85. Its central ROI is inside the audited zero-mud
interior; root inspection confirms visible ground, with a bush partly covering
the upper region and no ocean. Negative changed fraction is **zero**, mean
0.000091; master-off mean is 0.000601. Dirt/overhead masks still return with
IoU 1.0 and nonempty responses. This independently accepts exclusion at that
known BeachGrass cell, without relabelling the old failed shoreline images.
Source/camera bounds are in [the native audit](MUD-PUDDLE-NATIVE-NEGATIVE-AUDIT-20260930.md).

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
