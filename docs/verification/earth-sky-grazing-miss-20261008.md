# Earth grazing sky miss interval

Source checkpoint based on `0be06dbd`, isolated branch
`codex/earth-sky-horizon-20261008`. Parent installed source equivalent
`a0959fa3`/receipt `build/earth-streaming/20261008-111433` still showed a dark
stepped horizon band in both the 1000 m Coast and 9000 m retirement capture.
This source fix has CPU checks only; no game/GPU/build/deploy was performed.

The production `ray_sphere` function returns **(1,-1)** for a miss. The Earth
atmosphere branch incorrectly treated any positive first value as a forward
planet intersection, limiting its atmosphere march to one metre for a real
grazing miss. Steeper upward rays can have both roots behind the camera and
therefore still received a full atmosphere march. This makes an altitude-
dependent dark band around the geometric horizon. At 1000 m its grazing
half-angle is 1.0145 degrees; at 9000 m it is 3.0420 degrees, consistent with
the widening band in the saved images.

The Earth branch now requires an ordered forward interval: **near > 0 and
near <= far**. The identical invalid-interval issue in the Earth sun/moon
disc gate is corrected with the same helper. Native sky disc gating retains
its existing condition. No other sky/LUT predicates, color, fog, light,
geometry, depth, source elevation, receiver, layout or budget was changed.

`Test-EarthSkyHorizon.ps1` reads the actual WGSL predicate expression and
evaluates it as the same scalar Rust boolean expression. It checks the real
miss sentinel, behind-camera intervals, forward intersections and a positive
tangent. It verifies both Earth consumers use that helper, shows that a true
horizontal grazing miss retains the full available atmosphere path rather
than one metre, and composes/validates the complete actual production sky
shader through the existing composer. All pass; `git diff --check` passes.

Geometry/source separation evidence: comparing the saved 105311 and 111433
1000 m captures, every channel in rows 100..119 differs by at most one RGB
level after the Earth opaque lighting correction, while the lower-right
opaque land region changes by roughly 31 levels on average. The band is
therefore distinct from the corrected Earth albedo lighting. Legacy finite
terrain is skipped by the Earth Landscape branch, and legacy horizon/sky/
cloud drawing is suppressed when the procedural sky is active. No skirt or
land polygon was removed speculatively.

The lower-right polygon is still opaque source-backed land rendered with
coarse NASA imagery. This patch targets the proven sky interval defect; it
does not claim that polygon, coastline resolution or remaining sea appearance
is visually accepted. Parent owns the same Coast/9000 m installed retest.
