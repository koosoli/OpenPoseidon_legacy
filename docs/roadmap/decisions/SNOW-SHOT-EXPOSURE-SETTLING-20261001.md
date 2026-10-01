# Snow-shot appearance controls

Source candidate changes only `scripts/Test-SnowBulletRuntime.ps1`. Production
snow, bullet kernels and impact effects are unchanged. Installed rerun pending.

The actual `first-actual-20261001-020422-cac09d` d99bf434 campaign stored a real
M16 terrain impact with a maximum 7.623 cm deficit. Its overall status is
failed; those partial images are evidence for visual diagnosis, not full
lifecycle acceptance. Before/after paused capture clocks were 10880/11135 ms.
The low view already showed the dry-calibration impact presentation, and the
positive view showed a second grey plume. Effects were frozen roughly a
quarter second after the projectile, preventing bowl-only interpretation.

The clear foreground ROI `[x100:1100,y400:700]` in deposited-before-low has mean
RGB `(254.850,253.918,246.760)`, with red255 in88.734% of pixels and green255
in45.211%. This is highlight/channel clipping, not adaptive exposure pumping:
the fixture explicitly set adaptive OFF and manual exposure1. The manual pin
also disables normal time-of-day grading in EngineWgpu. The logged physical
sun diffuse is approximately `(20.708,18.129,12.130)` with upward cosine.591,
while the Hable curve's default white point is11.2. The saved brightness1.6
profile is standardized here, but physical-sky lighting replaces the legacy
brightness-scaled sun, so brightness is not established as the blowout cause.

The owner VBS reference `Screenshot 2026-09-30 223502.png` shows fine powder
relief and blue ambient shadows while retaining warm white sunlight. Existing
snow already uses near-neutral linear reflectance.78/.80/.82, deterministic
world-space filtered powder bands, and a9.1-degree normal-tilt bound. The
2.2m top camera should resolve the fine detail. Preserve those until a clean
headroom control establishes any remaining material problem; increasing grain
or globally darkening albedo to counter a clipped exposure would be premature.

## Corrected source fixture

The existing fixed arm remains the default, explicitly labelled
`legacy-fixed-manual-exposure1-adaptive-off; highlight-headroom-not-guaranteed`.
Use `-AutoExposure` for the normal time-of-day grade with the engine's default
adaptive policy. It removes inherited `WGR_EXPOSURE`, `WGR_AUTO_EXPOSURE`,
`WGR_TONEMAP` and `WGR_HDR_ENCODE` pins; it does not silently force adaptive
eye exposure ON (the current default is OFF). White-point/desaturation
overrides are removed in both arms. Brightness1 is present in the version3
private profile and confirmed through actual `triSetBrightness 1`.

Every shot now requires both the actual terminal terrain event and at least
2000ms of real simulation after mouse-up before pause. This includes dry
calibration before depositing snow. Result metadata records mouse-up time,
pause time and settling duration; PNG entries identify the exposure mode.
No smoke, decals or impact effects are deleted. Persistent authored decals
can still confound a pure geometric bowl interpretation and must be stated.

Actual Fired source, one-ammunition consumption, single terminal event,
source/roof admission, 12.5cm kernel correspondence, unchanged outer cells,
paused persistence/camera return, roof lift6 and normal quit/pair hashes remain
required. Environment restoration removes previously absent keys explicitly
for PowerShell7.6/.NET10. CPU self-tests exercise the actual settling and
environment-policy helpers, including pin contamination and <2-second refusal.
Both PowerShell5.1 and bundled PowerShell7 pass. Two pre-existing PS5.1
compatibility problems are also corrected in the owned fixture: extracting
numeric deficits before `Measure-Object`, and decoding JSON arrays through an
object property to preserve empty/single-element arrays without `NoEnumerate`.
These change neither the peak-deficit acceptance nor array/scalar semantics.

Run under the shared game lock with separate labels, for example:

```bash
LOCK_OWNER='settled snow normal grade' scripts/with-game-lock.sh \
  powershell -NoProfile -File scripts/Test-SnowBulletRuntime.ps1 \
  -AutoExposure -Label settled-normal-grade
```

Compare the settled top/low bowl, foreground channel clipping and powder
contrast with the fixed control before changing BRDF coefficients. A future
material test should exercise actual helper diffuse/normal variance at resolved
3.5cm/12cm bands, continuous attenuation to exact distant mean, finite bounded
normals, dry/sheltered exclusions, and day/night/zero-light response. Avoid
specular grain and arbitrary self-lit blue or dark crater shading.
