# Stratis cutout-ray comparison, 12 September 2026

## State

Satmap work completed first within its documented static-albedo scope.
Then tested the existing `WGR_EARLY_LATE_OPAQUE=2` experiment on installed
c00bf1eb. This is NOT default-enabled. No global foliage brightening, fog
increase, shorter view distance or new shader-lighting model was introduced.

The candidate replays only already-prepassed opaque cutouts before the
atmospheric composite, instead of letting their late colour overwrite it.
Cockpit, monitor, blended glass and non-prepassed draws remain in place.
Water/cloud order is unchanged. Draws move between passes, not duplicate.

## Tests

Stratis [1949.76,5046.07,32.37], az74.4/el-1.3, 06:03. Native 1280x720,
MSAA4, DLSS off, auto exposure off, LOD bounds fixed. Fresh isolated profiles.
Installed c00bf1eb, executable version string 49238fe (built before the
default-satmap commit); all compared runs use the same installed EXE/DLL pair.

Under `build/foliage-rays/acceptance/`:

- `stratis-control-c00bf1eb-rays1.*`: original order, exposure 1.
- `stratis-cutout-c00bf1eb-rays1.*`: cutout replay, exposure 1.
- `stratis-cutout-off-c00bf1eb-rays0.*`: replay, rays disabled.
- `stratis-control-e8-c00bf1eb-rays1.*`: original order, exposure 8.
- `stratis-cutout-e8-c00bf1eb-rays1.*`: replay, exposure 8.

Exposure 8 is a diagnostic comparison, not a changed shipping exposure.
The harness now accepts `-Exposure` explicitly. Every run exited cleanly.
The original-order rays-off visual reference from d6f60ff0 remains an older
source reference, not a same-build numerical control.

Matching native control/replay draw counts:
2075 main instances, 1180865 main triangles, 699 colour instances,
840361 colour triangles, 10 direct calls, 57 direct-indirect calls.
No objects/geometry were removed to obtain the contrast change.

Built with `cargo test --locked -p wgpu_renderer --profile rwdi -j 2`:
`cutout_ray_replay_preserves_solids_blends_and_cockpits` passed. Subsequently
`atmospheric_replay_only_moves_prepassed_objects` and
`late_surface_scattering_is_per_instance_and_excludes_other_segments` passed.
Cargo.lock retained its foreign SHA256
9A4E74D791B26A4303E064867DA67B862C1439D0635DF3E19ED5A97AADF31A2E.

## Visual result and correction of initial impression

The candidate removes the black crown contrast, but makes distant crowns
very low contrast against the slopes. The first visual impression was that
the slopes darkened. Pixel checks do NOT support that general claim:

At exposure 1, control/candidate RGB is identical at (860,290) = (56,46,34),
(1015,345) = (19,14,8), (450,380) = (21,16,9), (700,430) = (35,28,19),
and sky control (720,100) = (117,103,87). In the hill band y=200..479,
sampling every second pixel: 10012 brighter, 351 darker, 79237 unchanged
within 3 summed RGB code values. This is image analysis, NOT linear-light
photometry or proof that every pixel must match; wind/time-varying effects
can differ across captures. It corrects the unsupported broad-darkening claim.

Visual preference requested with both exposure-8 images: retain clearer crowns
or accept the strong atmospheric blending. Do not claim the whole Godray issue
finished before that decision and remaining coverage tests. The original order
remains installed by default meanwhile. No FPS claim: endpoint Godray GPU times
0.1546 / 0.1823 ms are not representative frame-time distributions.

Before default activation: assess crown readability, then check MSAA1/DLSS,
near foliage, glass/signs/cockpit and multiple world types. Existing unit filters
guard classification, but do not replace these visual/runtime regressions.
