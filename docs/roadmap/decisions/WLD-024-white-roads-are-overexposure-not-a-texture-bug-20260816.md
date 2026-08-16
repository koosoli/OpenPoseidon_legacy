# WLD-024 — "Arma 2 roads are white" is overexposure, proven by A/B

**Date:** 2026-08-16
**Branch:** `feat/texture-streaming`
**Status:** **CLOSED as a texture/material bug.** Reclassified as an exposure/tonemapping problem.
**Related:** [WLD-023](WLD-023-arma-2-roads-both-leads-dead-and-the-real-mechanism-20260816.md),
[RND-039](RND-039-eye-adaptation-exists-and-is-deliberately-off-20260815.md)

## The result

Takistan, hour 12, camera 6770.69 / 6099.78 / alt 312.86, az 230.7 el −6.8, 1600×900. **Same
binary, same pose, same frame number. Only `WGR_EXPOSURE` differs.**

| arm | mean RGB | mean luminance | ≥250 | ≥240 | the road looks |
|---|---|---:|---:|---:|---|
| `base` (default) | 175.1, 167.5, 148.8 | 167.7 | **19.36%** | 31.94% | **white** |
| `WGR_EXPOSURE=0.50` | 103.1, 94.6, 78.3 | 95.2 | **0.00%** | 0.00% | **grey asphalt, verges visible** |
| `WGR_EXPOSURE=0.25` | 78.4, 70.7, 57.2 | 71.4 | 0.00% | 0.00% | grey, darker |

At default exposure a fifth of the frame is clipped, the terrain reads as near-white sand and the
road is a white ribbon — the owner's complaint exactly. At half exposure the identical geometry and
the identical texture render as grey asphalt with dust verges, and the terrain regains its detail.

**The road was never wrong. The frame was.**

## Why this looked Arma-2-specific for so long

The Takistan road albedo is **(125, 120, 111)**; Arma 1's is **(68, 69, 68)** — 1.8× brighter. Under
the same overexposure the dark Arma 1 asphalt still lands in-range and reads grey, while the pale
Arma 2 asphalt clips to white. **A general exposure problem presents as an Arma-2-only road bug**,
which is why "roads are correct in Arma 1 and DayZ, only Arma 2 is broken" was true *and* misleading.

It also explains why WLD-023's material-side census came back completely empty: the face texture is
the only albedo a Takistan road can get, it is identical in kind to Arma 1's, and
`asf1_tak_ca.paa` decodes to correct grey asphalt. Every one of those findings was right; they were
just at the wrong layer.

## What this does NOT say

- It does **not** identify why the default exposure is too high on this world. The default path is
  the per-time-of-day tonemap presets (`UpdateAutoTonemap`), which were presumably tuned on CWA
  content. Whether Arma 2 desert is simply brighter than anything they were tuned against, or
  whether something in the Arma 2 lighting path adds gain, is **not established here**.
- It does **not** license lowering the exposure default. That would be a claim about every world,
  and CWA and Arma 3 captures measured at mean luminance 86 and 97 — i.e. *already* in range. A
  global exposure cut would crush them.
- It does not close the *see-through* half of the road complaint, which is a genuine depth problem
  with its own diagnosis in WLD-023 and is entirely unrelated to this.

## The fix this points at

The engine already contains the mechanism that solves exactly this: **auto-exposure**, a full
luminance-pyramid → `fs_adapt` → exposure-scale chain (RND-039). It meters the scene and pulls
exposure down on a bright desert automatically, which is precisely what these three arms did by
hand. It is `enabled = false` by default and has **no env lever**, so it cannot even be A/B'd from
a capture harness today.

That makes it a single change with two payoffs — this bug and the owner's separate request for an
iris that closes when he looks at the sun. But RND-039's three warnings stand: the default is off
deliberately (its 4× range can flash the scene on a camera turn), `skyWeight = 0.3` is tuned to
*discount* bright sky, and `rate` is framerate-dependent so any tuning is tuning against one
world's framerate.

**Next step:** add `WGR_AUTO_EXPOSURE` (and the tuning fields) as env levers, then measure whether
auto-exposure lands Takistan in range without the pumping. Do not change the default until that
run exists.

## Method note

This took one 3-arm capture and no rebuild, because `WGR_EXPOSURE` is read at startup. It was
available the entire time the material-side investigation was running. **When a symptom is "the
wrong colour", measure the frame's clipping before reading the material path** — the pixel
statistics (`≥250`, mean luminance) took seconds and would have redirected the whole effort.
