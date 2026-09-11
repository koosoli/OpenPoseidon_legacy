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
fixture initially stopped its empty helicopter automatically; not valid evidence.
Piloted run 20260911-204801 on installed 000aa2fe passes: stopped rotor deficit
zero, running rotor clears 0.5 m at centre, 20 m-away sample untouched, new snow
refills it, clean exit. Inspected rotor-after and rotor-ground images confirm
the surface removal. Separate airborne snow plume remains absent. Runtime-cost
trace is being added; multiple helicopters and sloping terrain not yet accepted.

Measured repeat on installed c529af69, 20260911-205301, passed again: 117 rotor
updates, mean 199.68 us, P95 401.8 us, max 561.6 us, 4 Hz. Timer covers terrain
queries and the CPU disc update, excludes log writing and GPU snow rendering.
This is one helicopter, not a general FPS/many-helicopter performance claim.

Prone-eye follow-up prepared: preserve the head-axis clearance and additionally
clear to CalculateCameraPosition(moveTrans), the animated pilot point actually
used by InsideCamera. No camera movement, per-view snow hiding, or larger global
brush. Installed internal-camera acceptance remains pending.

Installed 7329e2c4, crawl-20260911-210053: lifecycle passes, but internal image
still shows a nearby dark snow wall. The eye extension is NOT an accepted fix
for the reported underground view. This horizontal NPC-camera fixture does not
yet distinguish legitimate occlusion below 1 m cover from seeing through terrain
when looking down. Next diagnosis must record the actual view position/angle and
snow mesh at the eye; do not keep increasing the clearance radius blindly.

## Real player look-down checkpoint

Installed 142b6d6a, Test-SnowCrawl -PlayerProne: native 1280x720, DLSS off,
actual player, one held Z action and four downward mouse motions. Deep run
crawl-20260911-215109 (1 m) and shallow control crawl-20260911-215314 (0.18 m)
exit cleanly. Inspected external posture and final internal views: the camera
looks at exposed ground in the contact depression, with snow walls at 1 m.
No clear through-terrain view reproduced here. This does NOT close the tester
report; requested a reference image/video asynchronously, other work continues.

Fixture corrections matter: instantaneous down/up did not establish a held
stance action, and two held Z presses toggle prone back to standing. Those
earlier player sequences (214711/214921) are not prone acceptance evidence.
The fixture now records playerProne in test-context and warns that visual
posture/direction verification is still required, rather than auto-passing it.

Car contact: installed 142b6d6a, UAZ fixture 214031 passes stationary/departure,
bounded centre deficit (0.65 m at 1 m cover), untouched outside sample and refill.
Near view confirms a broad geometric channel, still visibly faceted. Approximate
0.35 m underside clearance, 10 Hz; no suspension/traction/physical snow change.
Tracked vehicles and multi-vehicle cost measurements remain open.
