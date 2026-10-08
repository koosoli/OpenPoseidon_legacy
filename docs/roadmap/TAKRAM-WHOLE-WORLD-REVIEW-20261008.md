# Takram reference review for whole-world rendering

Reviewed 2026-10-08 at Oliver's request. Reference:
[`three-geospatial`, b012ad06d858fc035d88aacfd73f092f93c994e4](https://github.com/takram-design-engineering/three-geospatial/tree/b012ad06d858fc035d88aacfd73f092f93c994e4).
This is a source/documentation review, not a performance comparison or an
integrated dependency. No upstream implementation, datasets or textures were
copied into Open Poseidon Engine.

## Findings

The [main project](https://github.com/takram-design-engineering/three-geospatial/blob/b012ad06d858fc035d88aacfd73f092f93c994e4/README.md)
is a modular Three.js/TypeScript rendering toolkit. Its atmosphere and clouds
packages are beta, core is alpha, and the WebGPU cloud rewrite is unfinished.
The separately named `three-geospatial-webgpu` repository only hosts the demo
site; the implementation is in the main repository. These are references for
our native C++/Rust/WGSL renderer, not libraries we can directly link.

| Reference | Useful application here | Limit |
| --- | --- | --- |
| [Atmosphere WebGPU documentation](https://github.com/takram-design-engineering/three-geospatial/blob/b012ad06d858fc035d88aacfd73f092f93c994e4/packages/atmosphere/WEBGPU.md) | Consistent atmospheric attenuation and scattered light across terrain, sky and objects; shared geographic frame and explicit metre-to-kilometre conversion | Additional shadow-length/light-shaft processing has separate cost. Their optional scattering raymarch uses temporal noise; this is not a free solution to our movement artifacts. |
| [Origin/frame example](https://github.com/takram-design-engineering/three-geospatial/blob/b012ad06d858fc035d88aacfd73f092f93c994e4/storybook-webgpu/src/atmosphere/AerialPerspective-WorldOriginRebasing.tsx) | A local frame derived from geographic position keeps atmosphere aligned with nearby geometry | Updating the atmosphere's reference transform alone does not rebase legacy gameplay, physics or every renderer resource. |
| [Shared tile renderer example](https://github.com/takram-design-engineering/three-geospatial/blob/b012ad06d858fc035d88aacfd73f092f93c994e4/storybook-webgpu/src/components/SharedTilesRenderer.tsx) | Share bounded caches and download/parse/process queues when multiple source layers are active | This wraps the separate `3d-tiles-renderer` library. It supplies neither our elevation provider nor worldwide OFP assets. |
| [Clouds documentation](https://github.com/takram-design-engineering/three-geospatial/blob/b012ad06d858fc035d88aacfd73f092f93c994e4/packages/clouds/README.md) | Global weather coverage and cloud/atmosphere composition provide useful visual and architectural references | Multiple raymarch/resolve stages; documented ghosting, disocclusion errors and cube-sphere seams. Do not replace our current clouds wholesale. |

## Comparison with the installed prototype

`engine/WgpuRenderer/rust/src/sky/mod.rs` already maintains transmittance and
multiple-scattering LUTs, keyed to atmospheric parameters rather than camera
motion or time of day. `sky/sky.wgsl` already has spherical atmosphere
intersections and Earth-specific orbital presentation. Adding a second full
atmosphere implementation would duplicate work and introduce conflicting
lighting. Takram is most useful for checking the existing model's consumers,
coordinate conventions and transitions.

Our `EarthCoordinates.hpp` uses a spherical presentation with a fixed local
geographic origin. The elevation window uses local tangent coordinates and a
curvature drop; the overview globe uses spherical normals. It is not a WGS84
ellipsoid or a globally rebased simulation. Takram's ellipsoid altitude
correction must not be transplanted into this spherical model without first
making terrain, ocean and atmosphere use the same shape and reference frame.

The latest coastal smoke test still shows a near-ocean/overview transition and
shading mismatch. Atmospheric haze could conceal this at some views, but it
would not repair ocean coverage or depth composition. Preserve that explicit
visual failure in the integrated ledger.

## Recommended bounded implementation order

1. Finish coverage, depth and matching near/far ocean shading in the current
   renderer before increasing atmospheric complexity. Keep the fixed terrain
   triangle budget and asynchronous stale-generation rejection.
2. Audit the existing atmospheric consumers against one planet centre, radius,
   geographic frame and length unit. Prototype any correction in Earth mode
   first; reuse the current LUTs and existing passes where practical. Keep
   local weather fog separately adjustable, so geographic haze does not silently
   double its extinction.
3. When expanding beyond the current moving-window model, use geographic
   double precision for durable state and camera-relative floats for rendering.
   Share resource budgets across elevation, imagery and feature layers. A
   screen-space-error hierarchy needs its own design; Takram's demo wrapper
   is not an implementation of that hierarchy for our engine.
4. Consider temporal cloud reconstruction only after stable depth and motion
   data exist. Keep it optional until motion, camera jumps and disocclusion
   tests beat the current implementation at a measured GPU cost.

Acceptance for a future atmosphere slice: identical cameras and settings at
ground level, 30km, 120km and orbital altitude; daylight and night; translation
and rapid geographic relocation; retained/direct terrain and objects; near/far
water. Capture GPU timings including LUT updates separately from steady-state
cost. Compare against the current build; do not assume that a WebGPU label or
a LUT makes a port faster. No added default pass without measured justification.

## Licence and data boundary

The project declares MIT, but package notices also enumerate component-specific
terms. The [atmosphere licence](https://github.com/takram-design-engineering/three-geospatial/blob/b012ad06d858fc035d88aacfd73f092f93c994e4/packages/atmosphere/LICENSE)
includes Eric Bruneton's three-clause redistribution terms. The
[core licence](https://github.com/takram-design-engineering/three-geospatial/blob/b012ad06d858fc035d88aacfd73f092f93c994e4/packages/core/LICENSE)
identifies an Apache-2.0 screen-space-shadow component; the
[clouds licence](https://github.com/takram-design-engineering/three-geospatial/blob/b012ad06d858fc035d88aacfd73f092f93c994e4/packages/clouds/LICENSE)
contains further attribution notices. Before adapting any particular file,
preserve its applicable notices and inspect its dependencies. Demo imagery,
external services and city datasets require their own source/rights review;
the software licence does not license the whole world's content.

Outcome: useful reference, especially for atmosphere/frame consistency and
shared streaming budgets. No engine defaults changed by this review, and no
whole-world content or performance parity is claimed.
