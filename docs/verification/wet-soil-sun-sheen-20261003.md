# Admitted wet-soil sun sheen, 2026-10-03

The calibrated installed Eden campaign
`calibrated-rain-soil-water-views-20261003-164853-3b21d9` passed the functional,
paused-camera and lifecycle guards. Root reported about 6.638 mm of genuine
standing water at the contact, admitted wetness about .803, and much less
blue mirror coverage than before calibration. The retained wet soil still
looked tan and matte under clear sunlight. This motivates an optical change;
it does not establish missing rain or a simulation/source admission defect.

The direct moist-soil GGX slope width now varies continuously from alpha .4
at zero admitted wetness to .22 at full admitted wetness. The actual geometric
normal's pixel derivatives still widen that minimum before evaluating the
existing normalized dielectric Fresnel/GGX/Smith response. This gives genuinely
wet soil a more readable broad sun sheen without a brightness multiplier,
albedo-grain normal, position/time noise, SSR or a new smooth-water layer.

Dry, hard, snow-covered, sheltered, unproven and backfacing controls keep their
existing zero response. The direct lobe still uses actual sun/cloud/shadow
visibility and receives dielectric Fresnel exactly once. Hydrological rates,
source flags, soft-surface deformation, clothing, dry albedo and HDR handling
are unchanged.

The sky audit found one mip in the existing 256x128 directional sky bake.
Sampling a higher mip would not implement a roughness prefilter. The available
SH9 sky-irradiance lookup is a wide-lobe approximation, and prior installed
evidence led to replacing that pale coating with directional sky radiance.
This focused change preserves the existing sky lookup and rough Fresnel cap.
It does not claim to implement a new environment prefilter.

The focused actual WGSL device probe has been extended with independent
normal-incidence and matched-view/light peak oracles, continuous wetness and
width endpoints, partial/full wet hemispherical energy quadrature, actual
visibility scaling, geometric derivative broadening, and the existing negative
source/shelter/snow/backface controls. Its previous quarter-wet linear
assumption is intentionally removed because wetness now also changes width.

CPU Naga composition of the complete production terrain, rainwater, direct
and retained object shaders passes using the existing host dependencies.
Device probe execution, full game build/deployment and the same actual Eden
normal/diagnostic capture comparison remain with the root agent. This source
checkpoint does not claim installed visual acceptance or complete mud quality.
