# MAT-035 — Arma 3's shadow LODs classified as ordinary visual LODs

- **Ticket:** MAT-035
- **Date:** 2026-08-11
- **Prompted by:** an owner report of a bright white orb near (2062, 5572) on Stratis
- **Status of that report:** **NOT explained.** See "What this does not fix".

## The defect

`ClassifyLodResolution` catches every special LOD with `resolution >= 1e5`. Arma 3
authors shadow geometry into LODs at **10000/10010** (shadow volume) and
**11000/11010** (shadow buffer), which are *below* that cut-off — so they classified
as `Visual`: ordinary LODs with a 10 km viewing distance.

`IsNormalLevel` could not catch them either. It rejects a level by comparing it
against the handful of *indices* `ScanShapes` assigns, and there is one index per
purpose. Nothing has ever held a shadow index, and a model carries two shadow
volumes anyway, so the comparison had nothing to compare against.

Measured over sampled models:

| | carries a 10000 LOD | carries an 11000 LOD |
|---|---:|---:|
| Arma 3 structures (n=45) | **36** | **36** |
| CWA / OFP (n=120) | **0** | **0** |

So this is a shape the original engine never met, and correcting it cannot regress
stock content.

A second, smaller case came out of the same investigation: A3 models carry a `4e13`
LOD this table does not name. It classifies as `Unknown` — which was neither
drawable nor geometry-only, so a consumer asking "is this geometry?" drew it. That
is where `a3\data_f\penetration\{metal_plate,concrete}.rvmat`, the last two
unresolvable materials from MAT-032, were coming from.

## The fix

`ShadowVolume` and `ShadowBuffer` are now classified as bands (not single values),
`IsGeometryOnlyLod` covers them, and a new `IsDrawableLod` states positively which
purposes may be drawn — so `Unknown` is excluded by construction rather than by
omission. `IsNormalLevel` asks the purpose before consulting its index list.

## What it demonstrably changed

On Stratis, materials reaching the colour pass from a non-visual LOD:

| | before | after |
|---|---:|---:|
| `penetration\*` materials resolved | 4 | **0** |
| materials with no albedo | 293 | **291** |

That is the whole of the measured effect, and it confirms the fix does what it says:
geometry that is not visual no longer reaches the draw path.

## What this does NOT fix — three claims I made and had to withdraw

The same camera and mission, before and after, differ by **19 pixels above a
threshold of 20**, scattered as antialiasing noise. The frames are otherwise
identical. So:

1. **It does not explain the owner's white orb.** I never reproduced the orb — three
   camera positions at noon and at dawn showed nothing like it — and this change
   alters nothing visible at those coordinates.
2. **It does not explain the flat green boxes** on the distant hillside. I asserted
   it would; they are unchanged. They are something else, still unidentified.
3. **The mechanism I proposed is unsupported.** I reasoned that a distance-based LOD
   chooser would reach a 10000-resolution level for far objects and draw the
   silhouette. The chooser walks resolutions ascending and stops at the first level
   too coarse for the required detail, so in practice it does not get there. The
   defect is that these levels were *registered as drawable at all* — wrong, and
   wasteful of the retained set — not that they were being drawn here.

The RND-alpha-routing note's standing hypothesis ("the shadow is not missing, it is
being drawn in the wrong place as visible geometry") is now half-answered: shadow
LODs really were reaching the drawable set. Whether that produced its night-glow
symptom is untested and remains open.

## A correction to MAT-033's verification note

MAT-033 recorded that captures are not frame-deterministic, on the evidence of two
runs of the same camera differing by mean 67/255, and concluded a pixel diff cannot
attribute a visual change. That is true **of the training mission**, which has live
AI, an animated player and moving cloud, and it is the mission those two runs used.

The `dev-missions/devtest*.abel` missions have one stationary soldier and no scripts,
and two runs of one differ by **mean 0.054/255**. A/B by pixel diff is therefore
available after all, provided the dev mission is used. That is how the "19 pixels"
number above was obtained, and it is the better harness for any future look question.

## 2026-08-11 follow-up â€” independent causes found and fixed

The two reports above were subsequently reproduced in the installed-game Stratis
dev mission, but neither was related to shadow LOD classification:

1. **White orb:** WGPU's experimental procedural lens-flare pass added a
   sun-strength screen-space flare whenever its ray saw open sky. This is separate
   from point/spot illumination and from emissive tower lamps. It is now disabled
   by default (`SkySettings::lensFlare = 0`) until its sun-direction and occlusion
   contract is validated. WGPU still owns the procedural sky, so the old legacy sun
   decal is also suppressed on that backend rather than stacking two solar flares.
2. **Flat green vegetation cards:** the CPU WGPU draw path classified binary
   DXT1/PAC alpha as cutout, but retained GPU-driven registration did not. Imported
   foliage sections marked opaque in the P3D therefore drew their texture's dark
   background as full cards. `ClassifyGpuSection` now applies the same
   `AlphaStats::Cutout` -> `alpha_ref >= 0.5` rule as `DrawSectionTL`.

Both changes were rebuilt, passed `run_preview0_wgpu_check.ps1`, and were captured
with `--render=wgpu`, `WGR_GPU_DRIVEN=1`, `@a3stratis`, `devtest.abel`, world position
`2062 5572`, frame 400. The resulting installed-game capture contains neither the
white flare blob nor opaque green cards. The red tower lamps remain on their model/
emissive path and are unaffected.
