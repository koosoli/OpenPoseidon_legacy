# Snow surface material and weather follow-through

Oliver asked to finish snow appearance and cover roofs and objects, after the
grass/mud batch. His follow-up also requests uniforms becoming wet in rain.
The baseline is `codex/streaming-residency` at c9024bb4; the installed matched
pair before this work is 8b3ae13c. Earlier snow terrain/contact/flake changes
remain part of that baseline, rather than new work in this batch.

## Powder material

The shared snow material uses subtle world-fixed diffuse powder variation and
tangent normal relief. Three rotated value-noise bands fade against the projected world pixel
footprint and converge to the existing mean colour and normal. There are no
moving random seeds, glitter grains, narrow specular lobes or new textures.
The existing linear mean palette and 28% compacted-track darkening are retained.

The first sine-band implementation produced visible diagonal ridges in the
automatic-exposure Eden capture on 6a388c62. It is superseded by cc43fffb:
integer world-cell hashes, continuous interpolation and analytic gradients,
with no time-dependent seeds. Beyond a 1.5m pixel footprint the helpers return
the exact mean/base normal before doing hash work. The actual GPU powder probe
passes bounded colour, finite unit normals and resolved-versus-distant detail;
the final integrated material and installed pictures still need verification.

Terrain colour and the normal prepass share powder normal evaluation. Track
slope differences use at least a pixel's world footprint, avoiding fixed 12.5cm
differences aliasing into distant normals. Deposit, drift, deficits, vertex
displacement and road-depth evaluation are preserved. Snow-free ground keeps
its normal maps; snow detail is applied only in proportion to snow coverage.
`WGR_SNOW_POWDER=0` is a same-build appearance control retaining the powder mean.

The production GPU helper test passes: finite bounded colour, unit normals,
close resolved relief, exact distant mean, compaction, partial cover and
dry/wall/underside/sheltered rejection. Both default and powder-disabled entry
shaders compose. These are helper/compile checks, not installed visual acceptance.

## Installed baseline

`build/snow-crawl/acceptance/crawl-20260930-160904` tests actual 1m cover with
walking and refill on 8b3ae13c. The owned process exits normally. The upright
soldier creates a continuous channel; refill reduces stored chunks and renewed
walking creates new tracks. Root inspected before/trail images: the channel
is preserved but its mesh edges are visibly faceted. This powder shader does
not claim a new collision surface or smooth geometric snow displacement.

## Acceptance still required for this batch

Rooftop/object colour coverage, material exclusions, actual stock roof shelter,
camera movement and reset controls need the new installed matched pair and
runtime captures. Uniform wetness additionally needs actual rainfall/time,
clothing versus skin/proxy admission, pause/drying and shelter controls.
Record these results here after runtime verification. Neither a passing helper
test nor an older snow capture completes these requests.
