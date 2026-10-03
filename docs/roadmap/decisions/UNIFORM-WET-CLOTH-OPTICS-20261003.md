# Wet textile optical correction candidate

Owner evidence is the actual installed `e81744f4` pair's Eden close views in
`original-weather-player/pinned-box3d-wet-uniform-and-footprint-optics-20261003-191735-542d35/eden/`.
The wet sleeve/trouser layer becomes grey and loses visible camouflage detail,
while the dry picture retains that detail. These are actual appearance evidence,
not pixel-isolated comparisons: the actor pose and surrounding weather differ.

## Source diagnosis and bounded model

The production `uniform_cloth_color` already preserves linear colour ratios
and texture detail while reducing saturated fabric reflectance to 45% of dry.
That helper, retained Person moisture, source identity and atlas masks remain
unchanged. The later shared wet-cloth block adds neutral sky/sun/local glare
independent of the camouflage dye. Its previous coating factor covered the
entire fully soaked surface with an exterior water film, even though absorption
had already darkened the underlying textile. Neutral reflection could dominate
dark camouflage. Its old white-substrate/unit-sky algebra test did not exercise
those real dark substrate colours or the actual production composite.

`frame::sky_irradiance` explicitly returns irradiance divided by PI; for a
uniform sky that is already the correct radiance scale. Dividing that value by
PI a second time would introduce an energy error. Instead the wet layer now
samples the existing actual linear sky/cloud environment, using a broad five
direction kernel with hemisphere weights normalized to preserve a uniform sky.
This does not edit the environment bake or any ground/puddle reflection helper.

The rough porous-fabric model limits exterior water-film coverage to one eighth
at saturation; the rest of the retained water affects substrate absorption.
That fraction is a conservative appearance calibration, not a measured physical
area of this stock uniform. Neutral dielectric reflection remains independent
of camouflage albedo. At normal incidence the effective exterior reflected
share is 0.125 * 0.02 = 0.0025; at the existing rough grazing ceiling it is 0.015.
The footprint-dependent broad sun/local lobe and actual lighting/shadow gates
remain in use, with coverage applied exactly once in the shared composite.
No time noise, micro-normal grain or metallic specular sheet is introduced.

`wet_textile_composite` reallocates the corresponding underlying share and
combines actual available sky, sun and local glare. It returns the original
substrate exactly for dry or excluded atlas pixels. Its response and lighting
are linear; zero lighting cannot produce an illuminated wet garment.

## Concrete acceptance checks and limits

The new GPU case calls the actual production atlas decoder, absorption, textile
response and composite together. It uses four actual stock merged-atlas texels
decoded by the production `PoseidonTools image convert` path. The installed
`DTA/Merged.pbo` member `00007mc_vojakw2.paa` has SHA256
`70f0ac74e5c9904d3140ed4b5eed9c72c627c5b714e501d282b948ad447b328d`.

| Pixel | Actual RGB8 |
| --- | --- |
| 160,128 | 8,16,33 |
| 800,180 | 99,89,82 |
| 389,102 | 49,36,33 |
| 620,280 | 57,60,49 |

The GPU gate requires absorption on those actual cloth colours, at least 85%
of their dry light/dark pattern contrast, unchanged dry/skin/boot/far-atlas
controls, finite HDR and dim-sky linearity, zero-source darkness, real positive
sun/local lobes, a back-facing negative control and neutral glare on zero-dye
substrate. The latter explicitly forbids hiding the defect with an
albedo-dependent reflection clamp. Light coefficients in that test are
representative declared controls, not captured installed-frame illumination.
The existing white-sky check now invokes the actual production composite.

Source/parser checks pass; GPU execution, full composed shader validation and
installed close/night/shelter comparisons remain root's gate. No device/game
test was run by the source agent. Run `uniform_wetness_tests` including
`production_wet_cloth_layer_preserves_real_camo_contrast_and_light_units_on_gpu`
on the root-owned renderer target, then inspect the installed fixed-mission
wet-uniform views. This candidate is not yet an accepted installed appearance.

Coverage, alpha, depth, shadows, source admission, the 128-byte material ABI,
stock skin/boot exclusions and retained moisture integration are unchanged.
Far baked whole-body atlases remain excluded. Unknown assets are not promoted.
