# Rainwater reflections of actual visible world surfaces

The simulation-owned standing-water renderer previously reflected only the
sky/cloud environment. A puddle beside a building could therefore retain its
physical depth and ripples while showing no reflection of that building.
This source candidate adds bounded screen-space reflections specifically to
RainWater. Installed appearance, GPU correctness and performance acceptance
remain pending root execution.

## Resources and pass order

`rain_water_reflections.rs` owns a private single-sample HDR texture, a small
readiness uniform and group 2 colour/sampler/depth bindings. No public renderer
ABI, physical RainWater data or shared Frame layout changes. The snapshot is
allocated lazily only when an actual selected wet-cell box intersects the
main camera frustum within 60m. The selection uses existing actual wet cells
and their bounded triangulated bed/depth range; it never advances hydrology.
Offscreen, distant, dry or ablated frames record no new snapshot.

The narrow `lib.rs` hook records after first-world colour and before the
RainWater draw, in the same encoder. At 1x it copies HDR colour; with MSAA it
records a colour resolve. The existing far-sample scene depth resolve is
refreshed at that point and shared read-only. RainWater does not write depth.
Existing ocean, monitor, underwater and downwash snapshot/render sequences
remain unchanged. Neither the active colour target nor RainWater's own colour
is read as reflected radiance.

Readiness is cleared each prepare/frame, including a frame without a valid
main camera. It becomes valid only after the current snapshot is recorded
before its consuming draw; previous allocated contents cannot authorize a
new frame. Snapshot size and depth-resource generation rebuild its binding;
the shader independently rejects unequal colour/depth dimensions. Resizing,
MSAA changes and render scaling therefore use actual scene-target dimensions.
HDR-disabled operation retains the original sky reflection.

This first-world colour snapshot can contain a pre-water transparent composite
on some legacy paths. Depth hits are still the actual depth-writing world set.
It is not a claim of a separate, complete all-world opaque capture, and cannot
reflect objects outside the current screen or geometry absent from its depth.
First-person/cockpit ClearDepth presentation segments occur later and are not
part of the first-world snapshot. Legitimate nearby external actors remain
eligible; the ocean's blanket 12m foreground exclusion is not copied here.

## Bounded reflection query

The authored water-field surface normal supplies the physically reflected
view ray. Twenty-four nonuniform steps favor nearby objects over a maximum
25m ray, followed by four binary refinements only at a valid depth crossing.
The shader requires finite projection/unprojection, in-bounds UV and forward
clip depth, actual non-cleared reversed-Z depth, a front-to-back crossing and
a tight camera-relative world-position agreement. The exact sampled depth
texel centre is unprojected; a discontinuous depth jump alone is not a hit.
The final tolerance is 6cm plus 0.6% of ray distance, capped at an extra 15cm.
The bed beneath the local water plane and immediate self-surface hits are
excluded. Walls/objects missing from the snapshot, offscreen rays, depth gaps
and unproven hits fall back to the existing sky environment.

Confidence fades near the screen edge, at 30–60m receiver distance, with fine
normal variation and unresolved world-pixel footprint. Black reflected pixels
are valid at night; validity is never inferred from RGB brightness. This only
replaces the reflected-radiance input of the separately authored physical
Fresnel/Beer-transmission helper. It preserves liquid depth, water support,
rain/shelter/snow admission, geometry, ripple normals and final blending.

## Controls, cost and checks

`WGR_RAIN_WATER_SSR=0` restores the original sky-only reflected radiance and
skips the extra snapshot/interest scan. `WGR_RAIN_WATER_SSR_TRACE=1` prints the
actual policy and bounded current-snapshot rows including dimensions, sample
count and `source=pre-rain-world selfFeedback=false`.

One full-resolution Rgba16Float snapshot is approximately **15.8MiB at 1080p**,
plus its copy/resolve bandwidth on admitted frames. MSAA depth may require the
existing full-screen far resolve when it was not already current. Fragment
work is bounded but still scales with visible near-water pixels; this is not
a measured performance claim. Root must compare fixed scenes with SSR on/off,
including a near flooded fill case and a dry/offscreen case, before acceptance.

Offline cargo check and CPU near/frustum/finite policy, actual shared-helper
composition and full RainWater shader composition pass. GPU test source uses
the actual production reflection functions, snapshot recording and readiness
buffer: 1x copy, 4x resolve, resource resize, nearby wall positive, valid black
radiance, absent depth, foreground/beyond-range walls, rough/far/underside/
offscreen/nonfinite refusals and previous-frame readiness clearing. These tests
are compiled without execution here; root owns actual GPU testing.

Installed gates require an actual stock building beside visible simulation
water, same camera/depth/time on/off controls, physically plausible reflected
building pixels distinct from sky/glints, movement/return without stale
images, dry/removed-water reset, raised/covered negative controls and unchanged
ocean/water-downwash regression checks. Thin unseen geometry is a documented
sky fallback, not fabricated screen-space reflection.
