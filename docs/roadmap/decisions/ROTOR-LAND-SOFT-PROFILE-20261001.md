# Land-only outward dust and powder profile

Source candidate only; installed appearance, full compilation and runtime
shelter/lifecycle gates remain pending. The initial `6cf9973e` capture at
`build/rotor-land/clear-canopy-neutral-hover-20261001-040004-68426b`
showed almost no dust body and isolated snow flecks despite eight emissions per
pulse and 48 live cloudlets. It also failed the roof negative: two pulses had
seven blocked probes and one exposed emitter. That failed record is retained.

At its approximately 9.7m hover and full RPM, the unchanged source policy gives
density about .458. Previous maximum dust alpha was about .110 before texture
alpha, fading and global smoke expansion thinning. Its 1.9/1.4 values were shape
scales, not measured puff radii. Larger overlapping particles now have a private
metre radius and optical envelope: dust grows 1.8 to 3.1m, powder 1.6 to 2.7m,
over .65 simulation seconds. Maximum alpha is .35; density remains actual
source-policy strength. Growth conserves projected radius-squared optical mass,
with a .12s smooth arrival and .65s smooth final disappearance. Dust/powder live
1.9/1.7 simulation seconds. Warm dust and off-white powder retain weather-lit
cloudlet RGB and the original soft sheet animation, with no emissive/light,
ground shadow or volumetric-smoke substitution.

This is isolated to `RotorLandPuff` and `EmitLandRotorRing`; water downwash,
ordinary smoke and global `Cloudlet`/`SmokeDisperse` settings are unchanged.
`Object::DrawDecal` projects its object scale directly, whereas object culling
uses authored `BoundingSphere()*Scale()`. The private draw projects the desired
metre half-width explicitly; its culling scale divides the full square's
bounding radius by the actual `cl_basic` sphere. Thus a small authored shape
cannot silently shrink the puff or under-report its culling bound.

Each broad puff requires center plus four perimeter support/source probes.
All five physical rays preserve explicit source, water, roadway and shelter
checks. An additional bounded conservative object-sphere refusal covers the
entire lifetime corridor, including roofs between those rays. The existing
`Object::Intersect` narrowphase does not expand convex planes or sphere rejects
by the provided ray radius, so a large `ObjectCollision` radius cannot establish
that proof. The private refusal uses the same `ObjRadiusRectangle`/resident
object index, ignores only its own aircraft and temporary effect objects, and
fails closed after 256 visited object slots per footprint. Object bounding
spheres include empty space: nearby rocks/trees can refuse emission even when a
precise swept intersection would have passed. It also inherits the index's
25m object-center search padding; this is no new all-map completeness claim.
The proof
radius is sqrt(2)*3.1m plus 3.5m permitted advection, about 7.88m. Every probe
must remain inside the actual map, above sea, within 1m of the center terrain
height, on the same entrainment type, with no unsupported raised surface or
physical overhead obstruction. Dustness, actual remaining snow, accumulated
wetness, actual liquid rain and physical standing-water gates remain mandatory.
This finite five-point terrain/source sampling does not establish continuous
classification of every texel between taps; the enclosing-object refusal is
the conservative object-cover proof for queried resident objects. Broad footprints close to structures,
material transitions and steep slopes can deliberately refuse emission.

The existing global budget remains 32 probes per .25 simulation seconds and
the global live cap remains 128. Clear ground admits at most six complete
footprints per pulse; an incomplete proof publishes no puff. The first of eight
sectors rotates across pulses. Initial motion follows the actual outward wake
and wind, then is bounded to 3.5m from the proven source point; outward velocity
is removed at that boundary. The billboard's complete footprint consequently
stays inside the conservatively admitted object corridor. Existing room containment and physical
terrain floor remain. Nonfinite/zero simulation time does not advance position,
appearance or lifetime; there are no persistent ground edits or gameplay random
stream changes. Actual rotor/off/dead/height/tilt/camera/visual-effect controls
remain upstream. Trace rows append `profile=soft footprint=<metres>` while
preserving the existing actual emission/count/weather fields.

Focused CPU regression checks source negatives, hydrology, bounded probe use,
metre/culling units, smooth finite opacity/radius bounds and optical-mass
conservation before disappearance. These checks cannot accept a convincing
ring. Root must inspect low/top real hover and wind movement, confirm stopped,
wet/sea/road/high-altitude/tilt controls and complete covered footprints, and
check normal exit, pause and live-budget evidence in the matching installed
build. There is no performance or all-map appearance claim.


The first installed soft-profile campaign on `ab442230`,
`build/rotor-land/soft-profile-dense-canopy-20261001-051017-72f0d6`, crashed at
its first grounded-spool emission and is failed, not appearance-accepted.
Source review found a concrete release memory-safety regression: the formerly
stateless `RotorLandPuff` inherited the `Cloudlet` fixed-block `operator new`,
while the soft profile added instance state beyond `sizeof(Cloudlet)`. That
allocator's size check is a debug assertion, so construction could overwrite
pool storage in release. The puff now declares and defines its own sized pool,
including matching delete. Compiler assertions require a larger derived size
and distinct allocation/deallocation functions; the focused source contract
requires those actual class declarations. The original dump's exact-build PDB
was unavailable after a later rebuild, so this source diagnosis is not an
exact instruction attribution of that dump. Root must rebuild the matching
binary and repeat the same installed first-emission/lifecycle fixture before
closing the crash or accepting the ring.
