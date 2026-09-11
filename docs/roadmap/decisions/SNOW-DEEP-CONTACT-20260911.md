# Deep snow contact continuation

Baseline installed 957a14a1, crawl-20260911-181639, actual depth verified as
1 metre, native 1280x720 and isolated profile. The previous crawl fixture only
used 0.18 m; passing it did not establish deep-snow correctness. The new depth
parameter sets and checks the actual limit/deposit rather than silently being
capped by the default 0.5 m. Internal/external views use the real prone actor.

Reproduced: prone-internal.png is inside a snow wall and exposes invalid-looking
surface/weapon intersections. The overhead crawling image shows a narrow trench.
SnowField::PressBody clears only a root-centred capsule and leaves 20 percent
of the global depth. It does not cover the actual animated head position.

First bounded correction: use full-depth clearance for the prone body and add
a 0.4 m head/neck corridor to the existing stamp field from the animated memory
point. Same 10 Hz body cadence and existing land/bridge/water gates. Reject a
nonfinite or more-than-2.5 m head offset. No terrain/collision/camera elevation,
no new snow simulation, no shader or GPU-resolution increase. Existing ordinary
Trail callers retain their 0.8 strength. New unit coverage checks the deep body,
head corridor, outside footprint, invalid offsets and refill by precipitation.

Installed A/B acceptance is pending. This does not close standing body or vehicle
displacement, rotor erosion, general buried-camera cases, or the whole snow task.
