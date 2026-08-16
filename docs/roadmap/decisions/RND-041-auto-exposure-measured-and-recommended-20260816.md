# RND-041 — Auto-exposure measured: the owner's HDR/iris request is a config change, not a feature

**Date:** 2026-08-16
**Status:** **Measured and working.** Default NOT changed — see the risk section.
**Related:** [RND-039](RND-039-eye-adaptation-exists-and-is-deliberately-off-20260815.md),
[WLD-024](WLD-024-white-roads-are-overexposure-not-a-texture-bug-20260816.md)

## The request

> "when the player looks in the direction of the sun, the iris adjusts and it gets a bit darker,
> because now the sky is often just almost white out when looking at the sun."

RND-039 established the chain already exists and is off on purpose. This record measures it.

## The measurement

Takistan, hour 8, camera 7778.77 / 5661.41 / alt 521.81, az 106.7 el −19.6 — deliberately the
worst-clipping pose measured this session. Same binary, same frame, only env differs.

| arm | mean luminance | ≥250 | ≥240 |
|---|---:|---:|---:|
| `off` (today's default) | 155.1 | **23.17%** | 30.29% |
| `WGR_AUTO_EXPOSURE=1` (engine defaults) | 123.9 | **0.59%** | 1.77% |
| sun-weighted (`SKYWEIGHT=1.0 MIN=0.55 MAX=1.10 TAU=0.4`) | 126.0 | 0.76% | 2.30% |

**Clipping falls from a quarter of the frame to under 1%.** Visually the sky keeps its cloud
structure, the mountains regain shading, terrain regains texture, and the road reads grey rather
than white — which independently reproduces WLD-024 from the other direction.

## Notable: the engine's own defaults were enough

The sun-weighted arm (raising `skyWeight` 0.3 → 1.0 so a bright sky actually drives the meter, with
a narrow 0.55..1.10 range and a real time constant) is **not better than plain
`WGR_AUTO_EXPOSURE=1`** on this pose — 0.76% vs 0.59%. The default metering already pulls the
exposure down enough here, because the *ground* is bright too. The sun-direction work RND-039
proposed is therefore **not needed for this symptom**; it would only matter for the narrower case of
a dark foreground under a bright sky, where `skyWeight = 0.3` would discount the sky.

That is a useful negative: the cheap change does the job, and the expensive one is unproven.

## Why the default is still not changed

Every warning in RND-039 stands, and none of them is visible in a still frame:

1. **Pumping.** `enabled = false` is deliberate — the comment says the 4× range "can flash the whole
   scene when a player turns from dark terrain toward a bright sky or water glint". A single
   settled capture cannot show that; it needs a *moving* camera, and the moving-camera harness
   built this session (`scripts/farfield-movecam.ps1`) is exactly the tool for it.
2. **`rate` is framerate-dependent**, so adaptation speed differs between a 20 fps world and a
   45 fps one. `WGR_AUTO_EXPOSURE_TAU` converts it to `1 - exp(-dt/tau)`; that should be the
   default before any tuning is trusted.
3. **A default is a claim about every world.** CWA and Arma 3 captures measure mean luminance 86 and
   97 with ~0% clipping — already in range. Auto-exposure should be near-neutral there, but that is
   an assumption, not a measurement.

## Recommended next step

Run `WGR_AUTO_EXPOSURE=1 WGR_AUTO_EXPOSURE_TAU=0.4` on a **moving** camera (the 30 m/s and 5 m/s
arms already exist) and score the frame-to-frame luminance delta. If it does not pump, this becomes
a default and closes both the owner's HDR request and the white-roads complaint with one change.
