# Aerial fog radius follow-up — 2026-10-05

Owner position: original Everon freefly[4526.06,3977.46,1197.58], az198.3,
el-28.1, weather fog1.0. Installed234795e7 reproduced the dark flecked radial
edge with the actual moving camera. Same source, scene and camera with layered
fog disabled removes the flecked edge; ordinary gradual distance cover remains.

The old fog view volume ended at the geometry/weather range3068.6m. From an
airborne camera this sphere slices the terrain-following slab before its whole
column is integrated. Synthetic far cover then receives an incomplete/dark
transport column, producing a camera-centred contour.

Aerial view transport now spans the farthest corner of the certified native
source field and the lowest active fog bound. It is admitted smoothly above
the highest active fog bound over its authored feather. Ground cameras retain
the previous range. Solar path cap and world-grid resolution still use the
original range; geometry/object culling and legacy far closure are unchanged.
Volume resolution and step count are unchanged; this is not a blanket quality
or speed increase. Large aerial steps retain the existing native linear-half
approximation and analytic thin-slab integration.

Actual GPU downward-column sweep includes altitude400m beyond geometry fog
range200m; the whole40m layer remains integrated. All17 layered-fog tests passed.
Full Build.ps1 matched-pair build and installed after comparison pending.

Raw before evidence:
- build/fog-flight/everon-high-radius-before-20261005-161432
- build/fog-flight/everon-high-radius-legacy-20261005-161747
