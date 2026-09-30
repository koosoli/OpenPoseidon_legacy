# Covered stock forest foliage reads as neutral frost

Status: source candidate, with installed appearance and GPU checks pending.
Earlier bounded tree/forest checks do not satisfy the owner's requested white
forest canopy from above. This changes the covered foliage material only.

Root's matched installed `d99bf434` campaign
`build/forest-snow/raw-coat-diagnostic-20261001-013212-8df1ae` completed Eden and
Nogova controls with normal exits and weather-map-ready traces. The tree-enabled
top images show widespread grey/white stippling on admitted canopy, with trunks,
non-admitted regions and alpha holes retaining their normal appearance. The
diagnostic is actual final physical coverage before material/lighting; its grey
appearance is not a claim of complete coverage on every visible leaf. Comparing
the ordinary material from the earlier matched campaigns shows predominantly
green/brown canopy despite visible diagnostic coat. Per-fragment highest-surface
coverage can still reject lower leaves; that behavior remains intact.

Offline decoded authored foliage provides a concrete material cause. Among
texels with alpha at least .753, median linear luminance is .0096–.0151 in the
three CWA needle sheets `les_borovice2`, `les_jehlic2`, `les_jehlicnan2`; Nogova
`dd_les_vetve04` is .01055. The old .008–.060 recess blend left 92–94% of those
CWA pixels and 99.57% of those Nogova pixels below its full-frost response.
The estimated fully coated Nogova material mean was only about .044 linear.
These are texture statistics, not a reconstruction of selected installed
triangles, filtered mips or screen-space coverage.

`tree_snow_albedo` now compresses the authored photographic shading into
neutral frost. It uses a .001–.010 recess transition and a monotone square-root
relief curve. Dark recesses no longer keep green hue once physically coated;
black remains exactly black. Luminance order and substantial bright/dark relief
remain, rather than replacing the card with constant white. The existing cool
frost reflectance and filtered metre-scale mottling remain bounded; no specular,
fine noise, new normals, view-facing mask or time seed is introduced.

The surrounding shader still mixes this material by actual physical coverage.
Zero coverage keeps the original source colour exactly. Authored alpha,
prepass, wind, geometry, source atlas/crown crops, bare branch/ground negatives,
owner/LOD proofs, uppermost surface and shelter rules remain unchanged. This
supersedes the earlier neutral-only `7c45c487` candidate; greyscale alone did
not address the much larger recess-strength suppression.

Source verification completed: extracting the production shader coefficients
and evaluating 10,001 luminance samples preserves finite bounded reflectance
and monotone contrast; 7,001 equal-luminance green/neutral pairs agree and exact
black stays zero. Rust parsing and `git diff --check` pass. The actual-helper
GPU fixture now contains 32 samples spanning deep recesses, ordinary dark
leaves, brighter leaves, resolved/unresolved footprints and matched green/
neutral sources. It checks exact unresolved mean, no green recess hue, strong
nonconstant contrast, black preservation and a readable .02-linear coated leaf.
GPU fixture execution and composed colour/depth consumers require root runs.

Installed acceptance remains open until root captures the same Eden/Nogova
near/200 m/500 m/top views with the new material and assesses white canopy,
retained photographic leaf detail, trunks/alpha holes and no white billboard
wedges or temporal glitter. Coverage diagnostic and tree ablation remain
available; this material change does not weaken missing/pending-map closure
to obtain a brighter picture. General unsupported forests/maps retain the
source-admission limitations recorded in the existing forest policy.
