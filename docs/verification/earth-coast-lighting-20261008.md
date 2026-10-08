# Earth coast lighting scale

Source checkpoint based on `e5b34ac0`, branch
`codex/earth-coast-opaque-20261008`. Saved installed images from
`build/earth-streaming/20261008-105311` show the rectangular water seam gone,
but land/overview still very dark beside pale water. The same dark source
imagery remains visible in the 9000 m retirement view. This correction has
CPU composition checks only; no GPU/game/deploy/full build occurred here.

The opaque Earth early-return branch used raw legacy material light from
`SubmitEarthHorizon`, while the camera frame's physical HDR sun had already
been converted to atmosphere-derived radiance. Earth therefore bypassed the
lighting scale used by ordinary physical materials. Textured Earth now uses
the current frame's physical sun plus directional SH sky irradiance whenever
physical sky lighting is enabled. The existing legacy branch and untextured
slot-zero fallback tint remain. No new light floor is added, so darkness at
night remains a result of the actual sun/sky lighting.

Earth Tidewater also used a different ambient source: `frame.sun_ambient.rgb`
is explicitly constructed as legacy ambient multiplied by sun intensity and
sky ambient, documented by its producer as non-physical fill. Earth water
now uses `sky_irradiance(spherical_normal) * frame.sun_ambient.w` on the physical
path, matching Earth land and the physical opaque materials. This feeds the
existing water-volume and foam lighting equations. Native OFP Tidewater and
legacy Earth lighting retain their existing inputs. No water tint, foam,
roughness, depth, geometry, budget or fog parameter was adjusted.

Source imagery is independently dark: the cached original 5400x2700 NASA
June image's nearest Guernsey texel at 49.45,-2.55 is RGB (32,36,37); nearby
49.43,-2.53 is (7,11,12). That coarse image cannot resolve the detailed
coastline represented by elevation samples. The capture has NASA imagery,
not Guernsey orthophotography. This change does not substitute imagery or
claim that every dark polygon or the horizon band is repaired. Actual water
depth/normal/foam debug views would be needed before attributing remaining
pale pixels to one of those mechanisms. An initially suspected unbounded
foam AA width was ruled out as a sufficient cause: the existing footprint
blend already switches to averaged foam coverage at this camera altitude.

Checks passed:

- Existing `Test-WetTextileShader.ps1`: complete actual direct and retained
  shaders compose with the production composer, including the modified Earth
  branch; existing wet textile helper/source-admission tests also pass.
- `Test-EarthWaterShader.ps1`: all four actual production water, wake,
  breaker and shore shader modules compose.
- `Test-EarthWaterCoverage.ps1`: eight production CPU selector tests pass.
- `git diff --check`.

The parent owns the next installed same-camera Coast/9000 m captures. These
checks establish legal production shader composition and preserve selection
contracts; they are not visual acceptance.
