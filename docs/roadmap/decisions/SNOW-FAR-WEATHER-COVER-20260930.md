# Physical weather coverage for distant snow receivers

Source candidate based on `5644755a` plus the standalone tree runner. Installed
acceptance and GPU execution are pending. This record does not mark the roof or
tree feature visually accepted.

## Problem and bounded implementation

The five artistic sky maps cover a 64 metre half-box. A roof at 200 or 500 metres
cannot derive physical snow exposure from their missing data. Increasing those
maps' extent would discard the window/interior resolution and conflate lighting
with physical coverage. Their tilted rays, ambient strength, and ambient floor
also do not establish vertical rain access.

`gfx3d/weather_cover.rs` owns a separate 2048 square Depth32Float vertical target,
640 metre half-extent and 1024 metre vertical half-height. It records depth from
the current registered retained meshes and the current main-view direct opaque
or cutout submissions. The direct source excludes cockpit, monitor and
on-surface overlays. Current vertex functions preserve conform, authored pose,
compute skin bake and vegetation wind; dedicated fragments preserve material
cutout thresholds and texture alpha holes. The depth projection alone switches
to the physical absolute-world view. Ordinary pipeline overrides default to
zero. Depth writes use forward Z and LessEqual, with no surface shading.

This source is **not every world collider or every off-screen dynamic roof**.
The direct list contains only current main-view submissions; transparent direct
glass does not belong to that opaque/cutout set. Registered retained geometry
uses its existing frustum/LOD/section admission. A missing authored far roof LOD
cannot be reconstructed by this pass. Simulation rain shelter continues to use
the existing collision queries independently.

The physical view occupies independent cull index 6. GI retains index 5; the five
AO view publications, cached counts, geometry-pass facts and batching retain
their existing layouts. The new private Frame tail is a matrix plus two vec4s (96
bytes) and CameraGroup binding 18 is one depth texture. No public C ABI changes
or new fragment storage buffers are required. The pass binds a dummy at binding
18 while writing the physical target, preventing attachment/read aliasing.

## Lifetime and admission

Each active frame starts unready with a new frame identity, current instance
epoch, and target generation. Retained coverage requires a recorded cull for
this independent view. Source resources and direct index ranges must be valid
before the depth pass is recorded. Only a complete recorded depth pass can set
current-frame readiness before main colour. Submission and commit remain
separate checks; abort or mismatched epoch invalidates readiness. No old matrix
is used with a rewritten target, and no pending/aborted map is classified open.

The dedicated cull's actual opaque/cutout argument counts and total record
reservation cursor are copied by the encoder into the final uniform lane before
colour/prepass shading. Exposure rejects argument or record overflow against the
actual per-variant capacity. Camera buffers already permit COPY_DST; cull
counters permit COPY_SRC. Both copies and uniform offsets are four-byte aligned.
No synchronous CPU readback is required. The CPU readiness trace certifies
recording/submission; the separate GPU counter gate certifies that the emitted
retained output did not overflow. Direct-only maps leave the counter lane zero.

The map refreshes each active frame rather than introducing a motion-sensitive
cache in this first slice. Existing planar reflection is submitted before this
map is recorded and sees unready physical coverage; main colour and its depth
prepass consume the current physical map. This limitation needs a separate
reflection scheduling change before claiming identical reflected coverage.

The frame requests work only for finite deposited snow, an enabled altitude
snowline with positive depth, or positive ground wetness. A dry frame drops the
large target and requests no weather cull or weather render pass. The shared
camera layout and tiny dummy remain allocated. `WGR_WEATHER_COVER=0` disables
this new pass for comparison, retaining the existing near-map/baked behavior.

Physical comparison uses an 8 centimetre vertical bias and a 1.5 texel validity
margin. At 0.625 metre texels, roof lips, thin overlaps, steep slopes and moving
cutout holes are still resolution limits, not exact collision boundaries.
The valid far map replaces missing near coverage; blending inside the near-map
feather first removes its artistic OPEN fallback. Ambient strength/floor and
tilted directions do not control this query. Outside both valid maps, an active
physical query returns closed; existing independent baked material fallbacks
remain governed by their prior producer/receiver admission.

The target costs 16 MiB plus one retained cull argument/record/counter view at the
existing capacity. Every active frame adds one retained cull and one depth pass,
including the direct source replay. This cost needs measurement on real large
forests/maps before any cache or resolution tuning. No dry-world ray scanning
or CPU traversal of all map objects is added.

When artistic AO is disabled, the existing indexed sky-view allocator still
reserves entries 0–5 in order to preserve the dedicated slot 6. Its generic
prepare currently allocates output buffers for those unused entries as well.
The AO-off comparison can therefore have higher memory overhead than the
default AO-on path. Moving physical cull storage into a separate sparse view is
a possible later optimisation; no additional AO culls or AO passes are enabled.

## Evidence and remaining acceptance

Offline Rust type checking passes. CPU tests cover exact frame/epoch/image
identity, cull/depth/submit ordering, failed source/abort, direct-only lifetime,
dry/off/invalid activation, and the actual production orthographic view at 200
and 500 metres with camera-relative cull equivalence. Actual direct and retained
WGSL compositions pass; Naga-derived Frame member offsets prove the appended
96-byte tail. Existing camera-tail upload checks include all shifted weather,
material, render-scale, fog and GI lanes.

`weather_cover_actual_gpu_far_roof_overhang_hole_underside_and_unknown` is an
explicit pending GPU check. It renders actual depth geometry with a partial
overhang and fragment-discard opening, then executes production coverage and
snow-admission functions. It checks exposed top and sheltered point at 200/500,
20cm below a thin roof, partial cover, hole, downward surface, requested/unready
map, map/height limits, removal of the near OPEN feather, and all three overflow
conditions through an actual buffer-to-uniform encoder copy. It is a shader
probe, not acceptance of a stock roof or complete production submission path.

The parent must run that GPU test and existing snow/rain/tree regressions,
build/deploy the combined game and retain its normal exit, exact binary/source
provenance and validation logs. Enable `WGR_WEATHER_COVER_TRACE=1` to retain up
to 32 actual commit lines with frame, instance epoch, target generation,
resolution, extent, retained count, direct count, readiness and submission.

Installed captures must use real stock roof geometry at near, 200 and 500 metre
cameras, move/return, positive deposit versus zero deposit, and physical-pass
ON/OFF at otherwise fixed weather/exposure. Require exposed roof snow, protected
interior upward floors, downward ceilings and walls, partial external cover and
cutout holes, with artistic AO both enabled and disabled. Retained and direct
paths, native and CWA maps, reset/removal and a camera recenter are separate
checks. Verify tree wind/holes/near-far crown continuity and no distant glitter.
Measure active/dry GPU and CPU cost without overlapping test campaigns. Stock
roof silhouettes, edge leakage and appearance remain visual review items.
