# Tree fog: bounded self-shelter correction

Owner image `Screenshot 2026-10-04 081612.png` shows horizontal dark/clear bands
in nearby foliage and distant trunks remaining dark behind fogged crowns. Source
base is `97d735bf` (installed source `5ee637d7`); this slice does not change fog
defaults, layer forcing, Weather UI or the main status ledger.

The shared layered-fog consumer used physical **rain reach at the surface** to
decide whether to apply the already integrated **air in front of the surface**.
A tree's own canopy blocks that endpoint reach. A tree can therefore receive two
different RGB responses across a rain-depth edge despite unchanged ray transport,
while its covered trunk receives none. That is a concrete source discontinuity
consistent with the reported image, not proof that every visible rectangle has
this single cause. Alpha/depth/geometry artefacts remain distinguishable.

`apply_fog_receiver` is now the one common implementation; generic `apply_fog`
delegates with the plant exception disabled. Direct objects pass the existing
`material.sun_ambient.w` class published from `GCurrentFoliageKind`, assigned from
actual tree/bush/forest/clutter MapType. Retained objects pass their existing
`in.is_veg`, derived from actual instance canopy flags. The role includes opaque
trunks and is independent of cutout leaf shading or a texture's alpha.

Only genuine plants may receive existing local ray transport when their endpoint
is canopy-sheltered. Actual weather-map coverage must still be present; cockpit
and native/cave ambient refusal remain. Non-plant roof/interior receivers retain
the old endpoint rain-reach requirement. The integrated volume's native height,
physical roof, readiness, retained-overflow and camera/source admission are
unchanged. This does not create fog inside blocked ray segments or invent missing
out-of-domain coverage. The conservative volume still treats canopy blockers as
occluders; this first slice does not implement fog transport through a canopy.

No base alpha, discard threshold, MSAA alpha-to-coverage, blend state, solid-blend
prepass, foliage lighting or geometry changes were made. Legacy/unready camera
paths still return the original mixture before layer sampling. The recent final
sky-closure correction and cloud composition remain unchanged.

Focused actual GPU regression compares exposed and self-sheltered plant RGBs with
identical physical transport and confirms equal response. It retains a
discriminating old-gate counterexample, roofed non-plant and missing coverage
refusals, and full/partial/zero output-alpha independence. Source guards prove the
actual direct/retained MapType callsites, native/cockpit condition and unchanged
output-alpha expressions. These alpha checks are helper/source evidence, not a
claim that a stock tree was rasterized in this fixture. Existing actual volume
roof/native/overflow negatives and final sky-closure tests also pass.

Root must build/deploy the paired candidate and compare the owner's scene with
Legacy/New at the same camera/weather, inspecting near card bands, crown/trunk
continuity, distant forest and alpha silhouette. Any residual density/froxel or
material alpha/depth fault remains open until that installed comparison; no
visual acceptance is inferred from helper tests or composition.
