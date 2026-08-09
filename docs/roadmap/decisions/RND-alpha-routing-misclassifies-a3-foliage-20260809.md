# Arma 3 foliage is routed to the translucent pass, and that is one bug behind four symptoms

**Date:** 2026-08-09
**Related:** MAT-030 (TreeAdv), and the owner's 2026-08-09 reports on the new tree assets
**Status:** Root cause established. Fix not yet implemented — it needs a decision about
where the shader-family lookup lives.

## The reports

1. Shadows do not work on the new A3 assets.
2. At night those assets stay bright while the CWA trees beside them go black.
3. Detail is too low when they are viewed from a distance.
4. (Earlier) dark patches on canopies that change with distance.

## The mechanism

Alpha routing is decided from the decoded texture's pixel histogram alone, in
`ClassifyAlpha` (`engine/Poseidon/Graphics/Textures/PAADecoder.cpp`):

```
if (pctPartial >= partialThreshold)  kind = Blend;    // default threshold: 2%
else if (pctClear >= clearThreshold) kind = Cutout;
else                                 kind = Opaque;
```

`Shape::HasBlendSections` and the per-section filter in `ShapeDraw.cpp` then pull every
`Blend` section out of the opaque/cutout pass and into a **back-to-front pass with no
depth write**.

Arma 3 foliage is authored as an alpha-tested cutout with *anti-aliased edges*, and those
soft edges are partial alpha. The shipped pine's leaf texture is **30.3% partial**; the
Enoch birch's is 5.8%. Both are far over a 2% threshold, so both canopies are drawn as
translucent geometry that writes no depth.

Every symptom follows from that one routing decision:

- **No shadows** — the sections write no depth and are not in the opaque set, so they
  neither occlude nor cast.
- **Bright at night** — dozens of overlapping translucent cards each blend their
  contribution instead of one winning the depth test, so the canopy accumulates. Measured:
  A3 pine region mean **16.6** against a whole-frame mean of **4.1** in a 01:00 mission.
- **Low detail at distance** — no alpha test means no crisp leaf edge; the canopy washes
  out instead of resolving.
- **Unstable dark patches** — back-to-front sorting of interpenetrating cards is
  order-dependent, so patches move with camera distance and angle.

## Why only the new assets

`ClassifyTextureAlpha` short-circuits: `oneBitAlphaFormat` (DXT1 and friends) is *always*
`Cutout`, never blend. CWA foliage is 1-bit, so it lands on the correct route by
construction. Arma 3 foliage is DXT5, so it goes through the histogram — and loses.

The engine is not wrong about CWA content. It has simply never been asked about content
whose cutouts are anti-aliased.

## Denominator

50 `_ca` textures extracted from `plants_f.pbo` and classified with the shipped rule:

| classification | n |
|---|---:|
| `BLEND` (back-to-front, no depth write) | **44** |
| `CUTOUT` (alpha test, writes depth) | 6 |

So this is not an edge case in Arma 3 vegetation; it is the common case.

## Fix direction

The histogram is the wrong authority. The material's shader family is the right one, and
it is now available: `TreeAdv`, `TreeAdvTrunk`, `TreeAdvTrans` and `Grass` are cutout
families, and the corpus even contains `SuperAToC` — alpha-to-coverage stated outright in
the family name. A section whose RVMAT names a cutout family should route to the
alpha-test pass whatever its histogram says.

What still needs deciding is **where that lookup lives**. `ShapeDraw` has `sec.surfMat` in
hand at both call sites, but parsing an RVMAT per draw is not acceptable; `EngineWgpu`
already keeps a path-keyed translation cache with negative caching, but it sits on the
renderer side of the interface and the routing decision is taken before it. The options
are a shared engine-side cache, or resolving the family once at shape load and storing the
answer on the section.

Deliberately not done here: raising the global `partialThreshold`. That would change
routing for all content on a number with no evidence behind it, including content where
partial alpha really does mean translucency (glass), and it would fix these trees by
accident rather than on purpose.

## Not explained by this

Whether the four safe RVMAT render-state terms should also be wired through
(`MAT-020-rvmat-render-state-unread-20260809.md`) is a separate defect with a separate
cause. It is likely a contributing factor to the night brightness but cannot be the whole
of it, and this routing bug is the larger term.
