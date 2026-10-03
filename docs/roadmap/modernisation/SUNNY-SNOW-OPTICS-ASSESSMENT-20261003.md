# Sunny snow distance and foliage appearance: read-only assessment

Actual campaign:
`build/church-snow/sunny-normal-exposure-trace-20261003-202703-3e0c79`, installed
`afffcc53`, stable executable/DLL hashes and normal exit in both master arms.
The campaign uses normal time-of-day grading/default adaptation and
`WGR_EXPOSURE_TRACE=1`; explicit exposure/adaptation overrides were cleared.
No production, material, global sky, exposure or snow colour change is proposed
from this evidence alone.

## Actual images and matching trace

Viewed both `Eden-object0` and `Eden-object1` enabled 30m/500m/top, disabled500,
and object0 near return. The master0 arm correctly leaves roofs/individual trees
uncoated while terrain snow is enabled. Master1 has a readable white roof and
white forest crowns. Terrain at 500m is severely washed out in both enabled
master arms. Disabled500 still has a very bright coastline. This separates the
terrain highlight gap from individual tree receiver/roof eligibility.

Relevant screenshot-pending rows for master1, with simulation time
100.000007629, time of day .666666687, clear weather, HDR/sky lighting enabled,
manual/pushed exposure 3.625, glare1 and adaptation bounds .1..1.1:

| Actual capture | Snapshot | Previously completed adaptation scale |
| --- | ---: | ---: |
| enabled-distance30 | 661 | .321792185 |
| enabled-distance200 | 826 | .100423612 |
| enabled-distance500 | 994 | .779510677 |
| enabled-top | 1169 | .101206355 |
| enabled-returnNear | 1898 | .321421385 |
| disabled-distance30 | 2115 | 1.099407911 |
| disabled-distance200 | 2312 | .127868399 |
| disabled-distance500 | 2503 | .953004181 |
| disabled-top | 2699 | .433226496 |
| restored-distance30 | 3305 | .322147071 |
| restored-distance200 | 3518 | .100097522 |
| restored-distance500 | 3728 | .780561805 |
| restored-top | 3948 | .100318529 |

Every listed row has cameraCount2 and the appropriate repeatable camera matrix
and position hashes. Around enabled500, snapshots991..997 all have cameraCount2
and the same matrix hash8324915090632855936/position hash12751683982750904378;
scale varies smoothly .779322207..780002952. Near and 200m likewise have matching
settled windows. Early camera-less rows exist, but do not explain these captures
as adaptation against a blank scene. Repeat enabled/restored results constrain
simple adaptation-history transients.

`EngineWgpu.cpp` explicitly reads this scale before submitting the current frame;
it is the previous completed frame, not the exact scale consumed by the PNG's
tonemap. Stable matching-pose windows are useful evidence, but are not a
same-frame luminance or HDR readback. The 500m multiplier is substantially larger
than 200m while manual exposure and pushed light remain stable. This supports
checking spatial/geometric-mean metering against bright snow and darker sea,
without proving that exposure alone causes the clipped snow.

## Source mechanisms and separate crossed-card gap

`exposure.wgsl` meters a vertically weighted geometric mean of the pre-tonemap
HDR scene: `(weighted log2 luminance, weight)` is reduced to1x1, then the target
is clamped `key/avg_lum`. This is screen-position weighting, not a snow/sea or
physical surface classifier. A bright upper land region and darker foreground
sea can therefore receive a different target from a close ground-dominated
view. No actual average luminance has been read in this campaign.

`tonemap.wgsl` multiplies scene/bloom radiance by manual exposure and the actual
current adaptation texture before its Hable curve and display grade. Its source
default linear white point is11.2; the runtime compiled constant is not in the
current exposure receipt. Source comments explicitly document highlight clipping
above that shoulder. A PNG cannot tell whether detail disappeared before or
inside that transform. Pushed sunlight/ambient trace values also do not substitute
for the actual fragment irradiance, including directional SH and shadows.

Terrain/roof snow uses approximately .78/.80/.82 linear diffuse reflectance.
Filtered powder bands converge to that mean, and powder normals are bounded;
turning up distant noise or lowering snow colour would mask the issue without
identifying the radiometric mismatch.

Near master1 trees visibly expose triangular/crossed foliage cards and roof-like
wedges. `tree_snow_height` is an authored-height crown envelope with positive
static leaf receiver proof; it intentionally does not treat card normals as
actual upper branch geometry. `tree_snow_albedo` preserves original alpha and
uses the mip-filtered authored luminance for neutral recess/frost relief.
`shading.wgsl` preserves foliage geometry/two-sided lighting, removes snowy
specular and reduces canopy transmission/wrap fill. It does not add branch snow
volume or turn legacy crossed-card geometry into an actual crown shell.

The sunny highlight loss can erase preserved texture contrast and make these
cards more conspicuous; the cards also remain a distinct source geometry/coverage
approximation even with correctly exposed snow. Do not replace the height gate
with a view/card-normal test: that would reintroduce LOD/view-dependent coating
and can suppress genuine foliage receivers. No claim that a colour change can
repair the legacy geometry is warranted.

## Smallest bounded next diagnostic

Existing public readback covers only the completed1x1 adaptation scale and the
final display PNG. No existing final-HDR or meter-log-luminance readback/capture
API was found. The single-sample HDR/upscaled resources already permit COPY_SRC;
the exposure luminance pyramid currently has render/sample usage only. A future
opt-in source diagnostic could reuse screenshot completion staging and add a
bounded meter copy permission, rather than expose a general production editor.

At the exact renderer frame used for a requested screenshot, record:

- the actual last reduction RG values, reconstructed geometric mean and target;
- the actual newly published adaptation value used by that frame's tonemap;
- a few fixed5x5 HDR tiles on land snow, shaded snow/foliage and foreground sea,
  from the exact resolved/upscaled resource consumed by tonemap;
- the bloom term at those tiles, current grade/white-balance/manual exposure and
  actual compiled Hable white point, plus projection/position hashes and frame ID.

Copy after exposure publication and before tonemap, using the same command
encoder/resource lifetime; complete with the existing requested screenshot.
Refuse mismatched frame/camera/resources or unsupported MSAA/resolve rather than
silently read a different HDR target. Restrict staging/readback to requested
captures, with finite values and a small tile count/byte cap; this diagnostic is
not performance evidence. It changes no exposure, sky or snow parameter.

The existing enabled/disabled/restored distance recipe supplies the controlled
poses. These readbacks would establish whether actual snow HDR still contains
powder/lighting contrast before transform, whether it lies beyond the actual
white point after exposure, and how the meter target changes with foreground
sea. Only then select a radiometric correction. Re-examine matched near-tree
cards after highlight control is understood; a separate branch/crown geometry
or source-material detail diagnostic may remain necessary.
