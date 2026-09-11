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

## Walking follow-up (installed narrow acceptance)

The owner clarified that deep walking must produce a continuous channel, not
individual footsteps. Above 0.25 m cover, the existing 10 Hz body-contact path
now stamps a continuous full-depth channel, radius 0.22 to 0.35 m with depth.
Shallow snow retains toe/heel stamps; deep snow suppresses the legacy footprint
decals as well. Land/bridge/water gates remain; teleports clear only their
destination, never the intervening landscape. This is visual surface clearance,
not a new snow locomotion or collision simulation.

Unit coverage verifies continuity, untouched surrounding snow, shallow/disabled
no-op, teleport rejection and refill. Combined snow/default-content tests:
9998 assertions, 15 cases passed. Test-SnowCrawl -Walking selects an upright
actor for installed acceptance. Vehicle and rotor displacement remain open.

Installed bd1f889c, crawl-20260911-202646: actual depth 1.0 m, upright soldier
travels to [9700.16,3605.26,-0.00137329]. Inspected crawling.png and trail.png:
continuous channel, no separated footprints. Refilled.png is smooth again;
new passage creates a channel again. Shutdown passed. The metre-deep channel
edges are still visibly faceted, not a claim of final snow quality or a fix for
the internal prone camera. The first attempt 202527 failed because the test
used unsupported forceSpeed; removed that test command, not an engine defect.

## Rotor follow-up (prepared, runtime pending)

Each helicopter now incrementally erodes the same persistent surface field at
4 Hz, only upright, above dry terrain, below 30 m and with rotating blades.
Radius is capped at 8 m; removal scales with squared rotor speed and fades with
height. This is a bounded visual ground-effect approximation, not snow fluid
dynamics. It does not yet emit a separate plume. It does not depend on the
camera-ranked four-rotor airflow list. Existing roof masking still applies.
Tests cover stopped/high rotor, gradual removal, bounded extent, saturation
and refill: combined suite 10006 assertions in 16 cases. Installed -Rotor
fixture and incremental runtime cost are pending; do not mark accepted yet.
