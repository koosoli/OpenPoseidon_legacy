# Contact-hardening CSM prototype

Baseline: asset-obs-001, 7aea321f. Owner explicitly prioritized this shared
renderer experiment over native material work. On September 13 the owner
visually accepted the 8-degree adaptive result and explicitly authorized its
promotion to the default; mode 0 remains the fixed-PCF A/B control.

## Implementation

Shared shaders/shadow.wgsl serves direct meshes, retained meshes, terrain and
grass. Existing alpha-tested caster generation is unchanged. A bounded blocker
search estimates mean blocker/receiver separation, converts normalized ortho
depth to metres using the cascade VP depth row, multiplies by tan(sun angular
radius), then converts metres to UV with each cascade's X/Y row lengths.
Camera distance and cascade index do not determine the physical radius.
Existing cascade coverage fallthrough and overlap blending remain intact.

Current implementation: mode 0 preserves the fixed-filter branch. Mode 1 uses
17 blocker depth fetches (including the centre) and 16 plane-corrected bilinear
filter taps. Mode 2 uses 9 blocker fetches and 8 filter taps, refined to 16 when
visibility is mixed, on every cascade, not a softness-changing far-tier switch.
Contact/subtexel penumbrae retain tent reconstruction. The sample disk is fixed,
without frame noise or temporal rotation. Historical measurements below explicitly
identify their earlier implementation commits.

Dev tools / Shadows: contact mode 0/1/2 and sun angular radius in degrees.
Reset restores mode 2 at 8 degrees. Startup diagnostics:
WGR_CONTACT_SHADOWS=0/1/2 and WGR_SUN_ANGULAR_RADIUS (clamped 0..8). These use the
previously unused W lanes of camera-forward and sun-direction in the shared
shadow block; no FFI layout growth or new bindings. Local lamps are unaffected.

## Explicit prototype limits

- Search/penumbra estimate capped at 100 m light-direction separation, constant
  in world space. Extremely tall/distant casters can be missed by the search.
- Sparse blocker search and mean depth approximate multi-layer foliage; no
  guarantee of exact area-light visibility. No-blocker search retains the centre
  comparison rather than assuming thin cutouts are absent.
- Existing receiver bias and finite shadow resolution remain limitations.
- Stable sample orientation removes deliberate temporal noise, but does not
  prove absence of motion/cascade aliasing. Runtime acceptance is required.
- Not ray tracing, not native Reforger material repair, not terrain heightfield
  soft-shadow replacement. No new global-default quality claim.

## Validation

Eleven shader tests pass, including all receivers and coalesced-PCF composition.
New scalar test verifies metric radius invariance over several ortho widths,
depth ranges and caster separations. This is not visual acceptance.

Measure-ContactShadows.ps1 uses existing installed-game capture/lifecycle and
frame-trace mechanisms. Modes 0/1/2, native 1x, MSAA4, DLSS Quality; fixed exposure,
same scene, resolution and LOD settings. contact_shadows.eden places matching
Jeeps at 0.05/3/12 m and includes town buildings. Reference option reuses native
Reforger trees at the owner's camera. Installed OFP runtime results follow;
the optional Reforger reference fixture has not been run for this feature.

Receiver shading is fused into colour passes. Report matched differences in
receiver-containing pass timestamps as incremental receiver cost, not a fictitious
standalone absolute shadow-receiver timer. Report caster-pass GPU time separately
and full GPU frame time; capture readbacks are excluded from frame-trace statistics.
Repetition and motion/DLSS visual checks precede any recommendation/default change.

Installed 434aee7e: full game build passes, 1,891 assertions in 48 existing
ShadowMath cases pass. OFP town images show all three fixture Jeeps and building/
foliage shadows. First captures labelled native were actually MSAA4: startup
accepts graphics.cfg msaaSamples=0 for OFF, not 1. Those labels are invalid as
native-1x evidence; the harness now maps 1x to configuration 0 and asserts actual
MSAA, DLSS activity and output dimensions from each capture.

A separate pre-existing route problem was exposed: DLSS with MSAA1 reports
missing depth/velocity/exposure inputs and falls back to bilinear. Mode0 run
mode0-dlss-radius0.266-matrix is rejected as DLSS evidence. Actual DLSS with
MSAA4 works in mode1/2; the DLSS comparison therefore uses MSAA4 in every arm.
Fixing that unrelated input-production route is not bundled into this shader
experiment. No claim of DLSS1x compatibility is made.

## First installed measurement matrix

1600x900 output; scripted camera has letterboxing, so this is not a full-screen
pixel-cost extrapolation. OFP Everon town, June 21 15:00, clear weather, exposure
0.15 fixed, governor range 1, three Jeeps at the specified heights. Every static
arm has 52,507 registered / 14,492 main instances and 1,205,361 main triangles.
DLSS Quality renders 1072x603 with MSAA4 and reports active=true in accepted arms.

GPU frame medians/p95 over a 15-second settled trace window ending two seconds
before shutdown/readback tail (more than 1,000 frames each), milliseconds:

| Route | Fixed median/p95 | Full median/p95 | Budget median/p95 |
|---|---:|---:|---:|
| Native 1x | 11.46 / 12.79 | 13.27 / 14.43 | 11.62 / 12.52 |
| Native MSAA4 | 13.82 / 14.72 | 14.65 / 15.59 | 13.53 / 14.41 |
| DLSS Quality + MSAA4 | 13.04 / 14.22 | 13.65 / 15.05 | 12.18 / 13.05 |

Single ordered runs, NOT repeated confidence intervals; budget below fixed is
not proof of a general FPS gain. PCSS can early-out for subtexel penumbrae, but
other passes and clock/load variance also affect the full-frame result.

Separate captured pass timestamps, milliseconds (fixed / full / budget):

| Route | Receiver-containing colour passes | Caster cascades + shadow cull |
|---|---:|---:|
| Native 1x | 1.927 / 2.213 / 2.140 | 0.998 / 0.969 / 1.539 |
| Native MSAA4 | 2.588 / 2.726 / 2.628 | 0.986 / 0.977 / 0.985 |
| DLSS Quality + MSAA4 | 1.700 / 2.770 / 1.695 | 1.889 / 0.995 / 0.988 |

Receiver-containing sum includes Terrain main colour, Objects colour solid and
alpha-test, and Grass colour. It includes ordinary shading, not just shadow
receiving; its matched difference is only an incremental cost estimate. Casters
are unchanged by the prototype; the varying caster snapshots illustrate why
these single samples cannot be treated as precise isolated shader timings.
No dedicated absolute receiver-only or complete shadow-related timer is claimed.

Artifacts: build/contact-shadows/mode{0,1,2}-native-radius0.266-verified,
mode{0,1,2}-msaa-radius0.266-matrix, mode{1,2}-dlss-radius0.266-matrix,
mode0-dlss-radius0.266-verified. Each contains run-01.json/log/png/meta and
settled-pace.json; trace CSVs are in the parent directory.

Additional fixed/full/budget native images at radius 2 degrees pass lifecycle
and retain identical geometry. Eight-frame DLSS camera sequences at radius 1
degree are visual diagnostics only; readbacks invalidate timing comparison.
Broad static images retain leaf cutout silhouettes and sharp vehicle contact;
the cheaper/full difference is small at this view scale, not proven negligible
at all angles. Movement/cascade stability remains an acceptance gate rather than
an assumed consequence of the stable sample disk. Keep both modes opt-in.

Initial motion inspection raised a suspected lost Jeep road shadow in budget
mode. Closer image-region inspection did NOT substantiate disappearance (mean
luminance in the sampled road-shadow region about 41/40/40 for fixed/full/budget).
Camera samples differ slightly in progress; do not present this as a confirmed
bug/fix. A follow-up nevertheless caps
mean blocker separation by the centre blocker's separation when one is known.
This preserves the local contact constraint in mixed-depth searches; scalar
regression covers near-contact/3/12 m blockers surrounded by 80 m blockers.
The measurement matrix above is PRE follow-up and must not be relabelled as
final performance acceptance. Re-test the same motion and static fixtures.

Installed follow-up 6c32bfc9 passes the eleven shader tests and the budget DLSS
motion fixture; its final frame retains the road shadow. That does not establish
a causal fix to the earlier unconfirmed observation. Final same-build matrix
uses labels mode{0,1,2}-{native,msaa,dlss}-radius0.266-final.

## Current result: installed 6c32bfc9

All nine final arms passed capture/lifecycle and actual AA-route assertions.
Same static scene and geometry counts as above; one run per arm, not a promotion
campaign. Settled GPU frame median/p95 in ms:

| Route | Fixed | Full | Budget |
|---|---:|---:|---:|
| Native 1x | 12.85 / 13.62 | 12.95 / 13.88 | 12.47 / 13.49 |
| Native MSAA4 | 13.93 / 15.09 | 14.82 / 16.53 | 14.23 / 15.48 |
| DLSS Quality + MSAA4 | 12.62 / 13.50 | 13.17 / 13.66 | 12.50 / 13.22 |

Receiver-containing colour / caster-and-cull snapshot sums, fixed/full/budget:

| Route | Receiver-containing ms | Caster ms |
|---|---:|---:|
| Native 1x | 2.11 / 2.69 / 2.14 | 0.98 / 1.19 / 0.97 |
| Native MSAA4 | 3.31 / 3.28 / 2.57 | 0.98 / 0.97 / 0.96 |
| DLSS Quality + MSAA4 | 2.37 / 1.73 / 1.69 | 1.01 / 0.96 / 0.98 |

Single pass snapshots do not track the settled full-frame differences reliably.
Do not interpret a lower colour snapshot as negative PCSS cost. The earlier
matrix also shows appreciable run variation. Prefer budget for further visual
evaluation, not default promotion; full's extra samples have not demonstrated
enough visible benefit in this town view to justify choosing it unconditionally.
Final native full/budget images retain the three vehicles and cast shadows;
their small difference at this scale is not a universal quality-equivalence test.

Current status: owner visual acceptance FAILED (see follow-up below), default-OFF
prototype. Still required before
promotion: balanced repeated runs, baseline-versus-candidate-OFF equivalence,
close-up and moving cascade-boundary acceptance, wind/overcast cases, and
cross-content visual review. DLSS1x remains explicitly unsupported by the tested
input route. No claim that community frame pacing or Reforger materials are fixed.

## Alignment with VBS4 exploration v2 and clouds

Read the owner's root-checkout OpenPoseidon_VBS4_Feature_Exploration_Plan_v2.md
(12 September, historical audit SHA 573dd972) without modifying that checkout.
Its A1 screen-depth sunlight detail and A2 reusable screen-space CSM resolve are
proposals, not a replacement for CSM and not evidence of a better implementation
already shipping. Current shadow.wgsl still evaluates shared CSM visibility in
receiver shading. This prototype changes that reusable visibility filter only.

- A1 could later supplement missing fine contacts, but must not harden every
  valid broad PCSS penumbra with an unconditional binary detail mask. Its
  confidence, cutout, off-screen-caster and direct-light composition gates remain.
- A2 could reuse this filter if it proves equivalent receiver normals, derivatives,
  MSAA surfaces and fade/fog inputs. A resolve cannot remove caster work, and
  moving one evaluation to a texture is not inherently cheaper. No A2 work is
  authorized merely by reading the plan; no new mask/history/G-buffer added here.
- Existing material/foliage defects must stay separate from shadow softening.

Cloud audit: frame.wgsl::cloud_sun_shadow already samples local cloud (red) and
smoke (green) transmittance; shading.wgsl multiplies remaining direct sunlight
by their product, separately from terrain/CSM occlusion. Thus clouds already
reduce the contrast of direct cast shadows, rather than attenuating ambient and
local lighting again. The PCSS angular radius itself is currently cloud-independent.

Cloud-driven penumbra broadening is NOT implemented or accepted yet. Scalar
transmittance does not describe the angular distribution of scattered skylight;
using global overcast or the cloud-times-smoke product as a blur radius would be
an unjustified shortcut. A bounded optional approximation, if pursued, should use
cloud-only local transmission, retain zero-separation contact and clear-sky
radius, preserve existing direct-light extinction, and be compared in clear,
thin-cloud, opaque-cloud and smoke-only negative controls. This remains an
explicit follow-up, not a silently completed checkbox or a second shadow engine.

## Owner fullscreen rejection and reconstruction follow-up

Owner tested 6c32bfc9 in ShowcaseLab.Intro, full PCSS, solar radius 0.266 degrees.
Log verifies DLSS Quality 2305x965 -> 3441x1440 with MSAA4 and cascade splits
61/129/271/1200 m. Reports: blocky shadows, player-shadow flicker in motion,
insufficient height-dependent softness, and no cloud-dependent softening.
This supersedes any implication that the distant static town images established
acceptable visual quality. Cloud broadening was indeed absent, not a user error.

Source-confirmed flaw: subtexel/no-blocker PCSS returned a single bilinear
comparison, dropping baseline 3x3 reconstruction. The follow-up retains the
baseline tent (16 unique fetches in the unclamped-gradient domain, exact
plane-corrected fallback otherwise) and continuously blends to the wider PCSS
filter over 0.5..1.5 shadow texels. Physical penumbra math, cloud composition,
caster animation, centre-depth cap and default-OFF setting stay unchanged for
this isolated experiment. It does not create missing geometric detail or prove
the player's temporal defect fixed. Reconstruction has an additional cost that
must be measured; no free-performance claim.

Installed reconstruction follow-up 5bdd25dc: game build and all eleven shader
tests pass. Native1x and actual DLSS Quality+MSAA4 captures pass lifecycle with
unchanged geometry counts. Native image inspection shows a filtered elevated
Jeep shadow edge instead of the prior hard steps. This is reconstruction, NOT
proof of a larger physical penumbra or fixed player motion. Settled 15-second
GPU median/p95: native 13.99/15.32 ms; DLSS 13.40/13.90 ms. Artifacts:
build/contact-shadows/mode1-{native,dlss}-radius0.266-reconstruct. Single ordered
runs, no isolated-cost/promotion claim; native cost is higher than the previous
full run (12.95/13.88), requiring a cheaper reconstruction evaluation.

Player flicker remains undiagnosed. SceneShadowPass already uses RenderTransform
for the caster and proxies, and EngineWgpu::AddShadowCaster copies its own bone
palette. Do not claim that reapplying the old interpolation fix solves this.
Await owner fixed-mode comparison and reproduce a close moving-player fixture.

## Priority revision: source size and standard blocker estimate

Owner reports no meaningful difference between fixed and prototype after the
reconstruction follow-up; player flicker is explicitly lower priority now.
Prioritize soft, gradually lighter penumbrae with low cost, not arbitrary
distance fading or additional special-case patches.

Remove the unvalidated centre-blocker minimum introduced in 6c32bfc9. Standard
mean blocker separation drives the shared PCSS footprint again. This removes a
discontinuous cap that could collapse the footprint on a near/self depth sample;
it is not yet proven to be the cause of the owner's observation. Retain the
reconstruction filter independently of physical emitter size.

At the physical solar radius 0.266 degrees, a 10 m light-space separation gives
about 9.3 cm full penumbra width; the shadow-map reconstruction can obscure that
difference. Compare 0.266 degrees with an explicitly artistic 1..2 degree emitter
using the existing source-size setting. Do not pretend this larger value is the
astronomical solar radius or alter the global default without acceptance.
Normalized visibility filtering makes a finite shadow lighter as the source
becomes partially visible. Large fully occluding surfaces retain an umbra:
fading those purely by distance would incorrectly leak direct sunlight.

Close walker fixture contact_player.eden / -PlayerMotion passes both fixed and
full native capture/lifecycle (16 frames per arm, labels player-near2). The first
fixture used unsupported object-to-array distance and was rejected, not counted.
The subject crosses a building shadow, so these sequences are not a quantitative
temporal-flicker verdict. Tests use an AI walker, not actual user input. No claim
that the player's reported problem is reproduced/fixed. Keep that work separate.

Installed cd5631bd passes game build and eleven shader tests. Four static native
arms, labels mode{1,2}-native-radius{0.266,2}-mean, pass lifecycle and retain
identical geometry. Settled GPU median/p95, ms:

| Mode | 0.266 degrees | 2 degrees (artistic) |
|---|---:|---:|
| Full | 14.47 / 15.81 | 15.61 / 17.04 |
| Budget | 13.19 / 14.87 | 13.17 / 14.18 |

One ordered run per arm, not confidence intervals or isolated shadow timing.
Budget is the preferred evaluation path; wider source has no demonstrated
additional full-frame cost in this pair, not a guaranteed free cost. Full's
larger source is costlier in this scene. Wide town images are insufficient for
owner acceptance; contact_soft.eden / -Close isolates the elevated vehicle's
ground shadow at closer range. No distance multiplier on shadow darkness added.

## Wider area-source evaluation, installed 8287fef0

The 2-degree close view still did not demonstrate enough visual separation.
Extend the existing source-size control consistently (UI/startup/packing/harness)
to 8 degrees, explicitly labelled artistic above the 0.266-degree physical sun.
Default remains OFF / 0.266 degrees. No new renderer, pass, texture or ABI field.
Blocker search uses radially concentrated points (r -> r squared) with the same
fetch count, covering near blockers as well as the outer search region. Filtering
still uses the original area-distributed disk. This is a bounded PCSS sampling
approximation, not exact area-light transport or new geometry.

Build and eleven shader tests pass. Close MSAA4 budget arms at 0.266/8 degrees
pass lifecycle/AA and have identical geometry. GPU median/p95: 14.96/15.43 versus
15.48/16.12 ms (15-second settled window, one ordered pair). This is +0.52 ms
versus the same prototype with small source, NOT total overhead versus fixed PCF.
At 8 degrees the elevated Jeep shadow visibly spreads and lightens around its
edge while a darker core remains. Sparse budget filtering has visibly uneven
coverage in this stress view; do not call it fully smooth or promote by timing
alone. Artifacts: mode2-msaa-radius{0.266,8}-close-wide.

Cloud-driven source spread remains OPEN and independent of this explicit
source-size evaluation. Player flicker is OPEN but lower priority per owner.
The physically correct wide-occluder umbra is not artificially distance-faded.

Full 16-point 8-degree close reference passes at 17.83/18.44 ms median/p95,
versus budget 15.48/16.12; visually more even, but +2.35 ms is not a cheap
global setting. Next revision keeps mode 2's nine-fetch blocker search and
eight filter taps, completing the sixteen-point reference disk only for mixed
visibility footprints. This is adaptive PCF within the same receiver function,
not another pass/history/blur layer. An all-lit/all-occluded sparse disk can still
miss a thin feature: adaptive sampling is not guaranteed reference equivalence.
Sample-union test ensures the refinement does not duplicate even-index taps.

Installed adaptive revision d6892d6e: game build and eleven shader tests pass.
Same-build fixed/adaptive MSAA4 close arms at 8 degrees pass lifecycle and have
identical geometry. GPU median/p95: 13.46/14.79 fixed, 15.77/16.87 adaptive,
one settled 15-second window per run. Adaptive broad shadow looks closer to the
full reference but retains a small uneven dark core. +2.31 ms versus fixed in
this scene is material, not negligible. Full's prior 17.83 ms was a different
run/build; do not turn that comparison into a guaranteed saving. Keep default
OFF and evaluate a moderate source size before any quality/cost recommendation.
Artifacts: mode{0,2}-msaa-radius8-close-adaptive.

Moderate 4-degree adaptive close arm passes at 14.76/15.43 ms median/p95,
versus fixed 13.46/14.79 and broad 8-degree 15.77/16.87. Same geometry and
settings except source size/mode, single runs; approximately +1.30/+2.31 ms
versus fixed, not universal costs. Four degrees retains a substantial dark
umbra in the close Jeep image; eight shows the owner's requested much broader,
lighter falloff but costs more and is an artistic area-source approximation.
Both require owner visual/cost judgement before promotion. Keep 0.266 physical
default and mode 0; no quiet global preset change. Latest adaptive implementation
is not yet motion/DLSS/cloud accepted. Artifact: mode2-msaa-radius4-close-adaptive.

## Owner-authorized default promotion

The owner visually accepted the exact mode 2 / 8-degree image from
`mode2-msaa-radius8-close-adaptive` and requested it as the default. The engine
and Reset-to-defaults state therefore use mode 2 and 8 degrees. This is a
deliberate artistic quality preset rather than the physical solar radius.
Mode 0 remains available in Dev tools and through `WGR_CONTACT_SHADOWS=0` for
immediate A/B comparison or fallback.

The measured +2.31 ms median receiver-path cost in the close MSAA4 fixture is
retained as a material risk; it is one ordered pair, not a universal cost.
This owner decision supersedes the earlier prototype-only default statements
above but does not claim that cloud-dependent source broadening or player-shadow
motion flicker is solved.
