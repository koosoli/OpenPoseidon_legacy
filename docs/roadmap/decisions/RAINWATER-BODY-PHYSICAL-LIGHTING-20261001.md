# Physical rain-water body lighting

This source correction is pending root GPU/full-build and installed visual
gates. The real fine pool is not accepted as a finished appearance.

Installed `3353770c` captures at
`build/rain-water-fine/bounded-rain-native-crater-20261001-042056-ab9f3d`
show an opaque pale blue/gray hollow from both low and top views. In the fixed
top ROI `(480,300)..(580,380)`, mean RGB is `(191.90,208.85,215.37)` with fine
water and `(34.88,28.95,24.05)` in the fine-OFF control. This is a descriptive
same-pose comparison, not exact independent weather/SH history. Fine-OFF still
has `WGR_RAIN_WATER=1`, so its rectangular background patches are the legacy
coarse physical-water presentation, not a no-water control. The full campaign
result remains failed. Both private profiles retain default brightness 1.6;
that grading confound is recorded rather than silently corrected mid-pair.

The production straight-alpha optics and `ALPHA_BLENDING` agree: reflected and
scattered radiance are normalized once by alpha, then transmission uses the
existing lit destination bed. The SSR resource snapshot occurs before this
water draw, with same-frame readiness; there is no obvious self-feedback or
premultiplication error in that path. The water top normal is near vertical,
so Fresnel is about .025. Yet the actual scattering input is not the unlit
`(.045,.060,.052)` coefficients used by the earlier standalone optics test.

The fragment multiplied those coefficients by the legacy flat ambient plus
20% sun diffuse, even on the physical sky path. Captured rainy ambient
`(22.1267,24.138218,32.184288)` and diffuse
`(1.0355144,.9062767,.60597956)` produce body radiance
`(1.00502,1.45917,1.67989)`. At the selected .54075m local depth, Beer transmission
is .19745, scatter weight .78248 and transmitted bed weight .19252. Body alone
therefore adds approximately `(.78641,1.14177,1.31448)` HDR radiance, before sky
reflection and tonemapping. This explains a concrete bright-body input that the
constant-body test never exercised. It does not measure the actual SH values
or establish that this is the only remaining appearance issue.

`rain_water_body_radiance` now uses the terrain lighting contract on physical
sky scenes: `sky_irradiance(n) * frame.sun_ambient.w` plus cosine-weighted direct
sun. Its low body reflectance coefficients are unchanged. Actual legacy scenes
retain the old flat-ambient/20%-sun calculation. No new hard dark grade, alpha
floor, saturation, extinction coefficient or Fresnel change is introduced.
SSR, actual depth, horizontal fine heads, native support/sea/snow/roof guards,
clipping, ownership masks, ripple/flow and fog stay unchanged. The new SH use
adds the existing camera binding9 to the required production raster fixture;
it adds no production texture, ABI or frame lane.

The ground-wet/puddle helpers contain no analogous body-scattering expression.
Terrain replaces flat ambient with physical SH before its wet film, and the
object shading physical branch does so too. Consequently this patch does not
globally darken film/mud or conceal coarse geometry boundaries. Those still
need separate actual controls.

Tests compose the real fragment and require its real SH/body-helper calls.
The source-derived actual GPU helper probe replays the captured legacy
ambient/direct vectors alongside explicit known SH fixtures for overcast,
direct sun and night. It verifies that physical output is independent of
legacy flat ambient, legacy output retains its old contract, finite HDR stays
finite/nonnegative, and real Beer/Fresnel/coverage behavior remains unchanged.
The supplied SH fixtures are deliberately not claimed as recovered mission SH.
The existing full raster tests now bind a real isotropic SH resource. Their
original coarse controls retain legacy lighting; fine flat/crater/slope and
all fine source/geometry/mask negatives exercise the actual physical fragment
with captured rainy ambient/direct vectors and known isotropic SH. Root must execute the probes,
build and inspect the matching installed low/top pool before any nice-water
claim. A remaining bright pool requires actual SH/reflection/body isolation,
not another arbitrary palette reduction.
