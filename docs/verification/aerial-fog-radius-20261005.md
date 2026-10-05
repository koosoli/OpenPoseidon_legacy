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


The installed sea-level candidate50ff8d57 still exposes the finite far-span edge.
Its12 timing samples: fog median0.6205ms/mean0.6405ms versus original234795e7
0.6472/0.7380ms; total median10.0163ms/mean10.0044ms versus10.1878/10.1752ms.
These are nonblocking published GPU timings from1280x720/MSAA4, no grass,
13 moving-camera poses and fixed noon/overcast0.15/fog1.0. They are scene-specific,
not a general performance promise. Ring visual acceptance remains pending.

Aerial grazing rays now extend to the certified lowest active fog plane rather
than another larger sphere. Producer, opaque receivers and clouds share the same
ray-dependent slice mapping. The final centred32-slice texel is placed beyond the
full column endpoint. Ground cameras retain their mapping; native/roof admission,
fixed steps/slices and solar grid remain unchanged. Actual ocean-column GPU tests
at oblique slopes0.1/0.2/0.45 now verify complete Beer-Lambert extinction beyond the
geometry range, alongside the17 passing actual shader/resource/receiver tests.


Installed a9683298 complete-ray compare (13 poses) passed runtime and budget:
fog mean0.6883ms/median0.6118ms; total10.0402/9.7802ms. Distant dark patches
remain visually present; disabling world patchiness also retains them. Numerical
column correctness does not establish owner-scene visual acceptance.

Final candidate removes the remaining explicit solar-cache discontinuity:
fog_cached_solar previously returned1 outside its camera-centred XZ field,
dropping physical incoming extinction at that boundary. The outer two cache cells
blend into the analytic certified local column; outside uses that column without
ray marching. Interior cache hits return immediately with their old sample budget.
This distant local column approximates terrain variation along the sun path;
actual CSM/cloud/terrain visibility still owns occlusion. No additional passes,
ray steps, slices or texture resources. Actual encoded equal flat columns inside
and outside the cache agree within2%; all17 fog GPU tests pass. Final owner-scene
runtime and budget comparison pending; do not infer full ring removal from tests.
