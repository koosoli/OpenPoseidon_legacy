# Sandbox ride iteration

Baseline installed 33aaf7c8, source 7b1b096a.

Owner requests: unreliable moving Jeep rifle fire; UH60 personal rifle (NOT FFAR),
NPC flight, more targets, enough time to ready the weapon, repeated circuits.

- Straight moving Jeep baseline 181742: 24 mouse clicks produced 24 shots at
  approximately 40-49 km/h, all sampled insertion checks clear. Does not reproduce
  the owner's intermittent issue; turning/arc-edge behaviour remains open.
- Original UH60 cargo 0 inspected in 180934: cargover1, forward-facing proxy.
- Content iteration: explicit start/stop Jeep control, 32 extra unarmed targets,
  repeated route; helicopter pilot and departure/return actions; cargo 0 rifle
  permission uses existing actions as an INITIAL FIT TEST, not accepted artwork.
- Pending installed validation: UH60 pose/collision, personal-weapon shots, flight
  and landing, Jeep circuit/start/pause and existing regression checks.
- An isolated user-mod preview did not expose the seat mapping. The package fit
  test is kept explicit rather than counting that failed load as a pose result.

Do not mark this work complete before the installed checks succeed.
