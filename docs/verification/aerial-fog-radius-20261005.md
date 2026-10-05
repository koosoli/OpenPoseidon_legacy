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


The first installed candidate c6384bde passed the numerical columns but did not
remove the owner-scene ring. Its 13-pose compare is recorded in
build/fog-flight/everon-high-radius-after-20261005-163137; this is not visual acceptance.

Second candidate: actual current-frame ocean nodes authorise the actual producer
sea level as the atmospheric floor over submerged native cells and off-map ocean.
The renderer already draws ocean outside the WRP field. Its seabed cannot displace
mist below water or its native field boundary remove air above visible ocean.
Absent current ocean nodes retain native-only admission; atmosphere below the mean
water plane stays excluded. Native validity and roof/weather gates remain required.
Internal uniform grows by16 bytes, no external ABI change. GPU volume dimensions,
four-step two-half integration, incoming-light grid and texture budgets are unchanged.
17 actual layered-fog GPU tests pass including translated nonzero water plane,
submerged native cells, off-map ocean, absent ocean and underwater exclusion.
Same installed scene image/timing comparison is pending. No performance gain claimed.
