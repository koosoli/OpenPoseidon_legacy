# Wet-soil sky reflection audit, 2026-10-03

Read-only audit of the actual installed `25445470` Eden run
`build/original-weather-player/box3d-build-muddy-medium-soil-20261003-181448-b0e77f`.
The audited sky/frame/terrain/reflection files have no diff between that installed
revision and root `1510c984`. No rendering or physical-source change is proposed
as already visually accepted.

## Actual observations and limits

Viewed the contact clear/overcast low normal-return PNGs and bare overcast low
normal-return PNG. Clear return is tan and mostly matte. Overcast has grey clouds,
darker substrate, and conspicuous cyan rims facing the camera. Broad distant
reflections look grey; the cyan is concentrated at disturbed contact edges.

The CPU contact receipt has native supported depth 2.8363168 mm, signed mud
impression -177.4768 mm, pure original `eden/pol.paa` Field admission and queued
wetness approximately .804. The retained-water interface has zero standing-water
Fresnel and zero direct solar coat at depths <=8 mm. Its Beer-Lambert body remains,
but a 177 mm pit does not fabricate 177 mm retained-water depth: support uses the
physical native bed. These facts disfavor blaming a mirror-like retained-water
interface at the queried centre; they do not identify every cyan screenshot pixel.

Actual camera is [6423.81006,142.307526,7175.56396]. Using the actual CPU surface
slopes (-.04076975,.11008172), its centre normal is approximately
[.0404917,.9931803,-.1093310]; the reflected centre ray has world y +.2024997.
The bare-centre receipt gives reflected y +.2872511. Neither centre is a
below-horizon ray. Footprint-edge GPU normals/directions have not been read back,
so the lower-hemisphere mechanism below is a demonstrated source weakness and
an unconfirmed attribution to those particular cyan pixels. The terrain shader
also filters the impression normal by actual pixel footprint, so these CPU
centre calculations do not replace actual GPU-normal evidence.

## Environment source and frame order

`ground_sky_reflection` samples Sky's real 256x128 Rgba16Float equirectangular
radiance map. It is not an atmosphere-only cubemap. `fs_sky_env` evaluates the
same current atmosphere, cloud deck and cirrus field. It uses 12 primary deck
steps, two lighting steps and no pixel jitter. The visible cloud pass instead
has an adaptive maximum of 128 primary steps and four lighting steps with pixel
jitter; both share cloud density, weather drift and cloud lighting code.

The root frame records main sky upload, dirty atmosphere LUTs, current env bake,
SH projection, and main sky rendering before terrain/water rendering. Env is
baked every sky frame, rather than being keyed only by atmosphere changes.
The uniform receives cloud look and runtime offsets from the existing producer
paths. A CPU weather receipt records coverage .95, but explicitly does not prove
Rust acceptance or GPU pixels. There is no source evidence here of a stale-env
weather epoch bug.

Terrain group0 binding19 receives this exact `Sky::env_view`; the retained-water
fallback and original sea environment also share it. Altering that shared bake
would affect accepted sea/ambient lighting and needs broader validation.

## Concrete directional mismatch

`sky_radiance_weighted` deliberately replaces negative ray y with zero for its
atmosphere march, because the visible sky behind terrain should extend the
horizon colour. `fs_sky_env` then marches clouds using the original direction.
`cloud_interval` correctly clips against the planet. A sufficiently downward
ray therefore receives horizon atmosphere while its cloud interval is empty:
under overcast, the lower hemisphere can still contain clear blue horizon light.
This is sensible background padding but is not reflected ground radiance.

The ground moist-soil coat performs a single environment lookup along
`reflect(-view, geometric_n)`. It has scalar sky visibility but no directional
terrain-reflection occlusion. Real impression slopes can turn that lookup below
the global horizon. The broad direct GGX roughness does not filter this separate
single-ray environment lookup. Thus a rough wet-soil surface can show sharply
colored environment rims despite a broad solar lobe.

The 12-step env cloud integral also undersamples the shared high-frequency
cloud density at grazing directions compared with the visible cloud pass. This
is another defensible candidate for blue holes in otherwise grey reflected sky;
its size and actual affected directions remain unmeasured.

## SSR order and its limits

The retained reflection resource is invalidated at begin-frame. When actual
near wet geometry requests it, the current opaque HDR scene is copied/resolved
into a separate texture before the rain-water pass. It is current-frame data,
not a previous-frame replay. The cloud composite runs later, after the sea too.
This means accepted SSR geometry radiance is pre-cloud-composite, but SSR rejects
zero sky depth and the below-water-plane bed. A pure sky miss therefore uses the
cloud-bearing env map. Moving the snapshot after the cloud composite alone does
not repair those sky misses, and would create ordering/self-feedback issues.
The soil coat itself has no SSR path.

## Smallest justified next slice

First measure, without grading RGB: an actual headless env readback at this
weather/pose for upward, horizon and downward directions, and a control env bake
using the full cloud step budget. Record actual reflected direction/normal at
cyan contact-edge fragments or sample their actual filtered terrain geometry.
Compare 12-step and full-step radiance/transmittance, not just screenshots.
A bounded source/normal diagnostic can distinguish downward sky padding from
upward cloud undersampling and from legitimate blue breaks in the actual deck.

If downward lookup is confirmed, keep the shared sky/sea bake and physical mud
unchanged. A terrain-coat-only source-aware terrain occlusion check is the narrow
legitimate correction: refuse unavailable sky radiance when the reflected ray
hits actual terrain. A blanket world-y clamp is not enough to distinguish real
sloped horizons or downhill sky, and using the substrate's own RGB as guessed
reflected terrain radiance would invent a ray hit.

If upward env undersampling is confirmed, consider a bounded env-only increase
or density-filtered integral with actual measured SkyEnv GPU cost and SH temporal
controls. Do not blindly copy the 128-step full-screen march. Rough-soil-only
bounded positive normalized angular filtering may then match broad roughness,
with convex radiance weights and proven terrain occlusion; filtering alone must
not mix unavailable lower-hemisphere sky into a visible coat.

Counters must retain clear-day upward sky, actual cloud holes, night zero-light,
source/snow/shelter/hard-surface refusal, coarse/fine supported depth, and accepted
sea appearance. No water mass, weather calibration, support geometry or material
classification should change as part of this optics investigation. Main wet-mud
appearance remains open.
