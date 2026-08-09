# Arma 3 foliage is routed to the translucent pass (and that did NOT turn out to explain the symptoms)

**Date:** 2026-08-09
**Related:** MAT-030 (TreeAdv), and the owner's 2026-08-09 reports on the new tree assets
**Status:** Misclassification confirmed and corrected in code. The causal claim below was
WRONG — see "Correction" at the end.

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


---

# Correction (same day, after implementing it)

The misclassification is real: 44 of 50, and the shipped pine's foliage material now
resolves `cutout-family=true` through the new override, traced in a live capture. But the
causal story above is **not supported**, and it should not be repeated.

With the override demonstrably firing on the pine's foliage section:

- the 01:00 night capture is **byte-for-byte unchanged** — tree-region mean 16.60, max 35,
  frame mean 4.12, identical to the run before the change;
- the daytime capture is visually indistinguishable, with no shadow appearing and no
  regression.

So routing foliage out of the back-to-front pass changes nothing observable here. Every
symptom this document attributed to it — the night glow, the missing shadows, the soft
distant canopy, the moving dark patches — remains **unexplained**. Three separate
candidate causes had now been eliminated for the night brightness: the normal-map
binding, the RVMAT render state, and blend routing.

Two readings remain, and they have not been separated:

1. the blend pass split (`GSectionFilter`) is not actually reached for objects on this
   draw path, so demoting a section changes which bucket it is put in but not how it is
   drawn; or
2. it is reached, and the translucent route was never what produced these symptoms.

The next step is to determine which — by instrumenting where `GSectionFilter` is set for
this object rather than where it is read. Until that is known, the change below is kept on
its own merits (Arma 3 foliage *is* an alpha-tested cutout, and the histogram *is* the
wrong authority) and is claimed to fix nothing.

---

# Investigation state (same day, after tracing the pass split)

The two readings above have been narrowed but not closed, by instrumenting the section
loop in `ShapeDraw.cpp` rather than reasoning about it.

What the trace showed, at 01:00 with the pine placed and `WGR_GPU_DRIVEN=0`:

- The section loop **does** run, and `GSectionFilter` is `OpaqueAndCutout` (1) there, so the
  pass split is genuinely in effect on this path. Reading 1 as originally worded is wrong.
- Every section observed early in the frame reports **`surfMat = <none>`** — no authored
  material at all. Those are CWA world objects, which is expected, since CWA content has
  no RVMATs.
- A probe restricted to sections whose material path contains `polyplane` — the pine's
  foliage material — **never fired**, across the whole frame, for any filter value.

That last point is the live question and it is not yet explained. `EngineWgpu::DrawSectionTL`
demonstrably receives the pine's `surfMat` (it logs the RVMAT path and resolves its slots),
and `ShapeDraw` is what passes `secSurfMat` down to it. So the material reaches the
renderer, but the section carrying it appears not to pass through the filter site being
instrumented.

The most likely explanation, untested: the A3 tree is submitted through a different section
path than the one carrying the filter — the instanced run, the GPU-owned section skip
(`GSkipGpuOwnedSections`), or a separate entry point in the same file — so the routing
decision for it is taken somewhere the override does not sit. Confirming that means tracing
from `EngineWgpu::DrawSectionTL` *backwards* to its caller for this object, rather than
forwards from the loop.

Until then the override remains correct and inert on this object, and none of the four
reported symptoms has an established cause.

---

# Night brightness: located to a draw path, not to a material (same day)

Traced by instrumenting `EngineWgpu::DrawSectionTL` directly, then reverted.

**In daylight**, both of the pine's sections reach the material-aware path:

```
TRACEROUTE mat=...t_pinusp3s_f_lod1_noatlas.rvmat   alphaClass=0  filter=1
TRACEROUTE mat=...t_pinusp3s_f_lod1_polyplane.rvmat alphaClass=2  filter=1
```

Two things fall out of that line alone. The foliage really is `alphaClass=2` (Blend), so
the misclassification at the top of this document is confirmed at runtime; and it is being
drawn during the `OpaqueAndCutout` pass (`filter=1`), which is only possible because the
cutout-family override demoted it. The override works.

**At night, no Arma 3 section reaches `DrawSectionTL` at all** — not the pine's, not any
other. A probe widened to match any material or texture path containing `a3\` logged
nothing for the whole frame.

The tree is nevertheless drawn, and is the brightest thing in the frame. Removing
`--test-model` from the same night capture drops the tree region's mean from **16.60 to
0.60**, so that grey mass is unambiguously the pine.

Eliminated along the way: the software-transform path (`EngineWgpu::DrawSection`) fires
identically in both captures — the same three calls of 42,096 / 32,808 / 52 faces — so the
tree is not being routed there.

## Where that points

Something draws the tree at night through a path that binds no material, which is exactly
consistent with every earlier null result: no material means the default white `_curMaterial`,
which is why zeroing `emmisive`, setting it to bright red, disabling the normal map and
demoting the alpha route all changed nothing at night.

The remaining candidate is the **shadow path**. `Shadow.cpp` draws shadow volumes through
`GEngine->DrawSectionTL` under `IsShadow`, with no texture and no `surfMat` — which is
precisely the shape of section every probe here was filtering out. A shadow volume rendered
as visible unlit geometry would look like what the capture shows: a uniform grey silhouette
with the tree's outline, brighter than an unlit night scene.

If that is what is happening, it is the same defect as the owner's first report seen from
the other side — the shadow is not missing, it is being drawn in the wrong place as visible
geometry.

**Next step**, and it is a small one: re-run the same trace filtered on sections with *no*
texture and *no* material, comparing day and night, and check the `IsShadow` spec bit. That
distinguishes "shadow volume drawn as geometry" from "some third path" in a single capture.
