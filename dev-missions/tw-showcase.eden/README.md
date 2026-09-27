# tw-showcase.eden — Tidewater water showcase

A 2-minute, hands-off camera fly-over for previewing the water: Everon's south-east bay (map
around 9800-10400 E, 1000-1600 N), where the sea floor shelves gently enough for the shore
waves to break.

- An LST (`CarrierW`, the Newport) holds about 300 m offshore, bow toward the beach.
- Four PBRs (`BoatW`) wait off its bow, run in at 3 s, unload one squad each (a 24-man platoon)
  with a TR UNLOAD waypoint and stay in the surf. `land.sqs` then sends each squad
  up to the flat ground behind the beach (and gets them out itself if a boat stops in the
  shallows without unloading; it never ejects anyone at sea).
- Two UH-60s, a Chinook and a Cobra fly over the bay; two A-10 pairs pass at about 27 s and 98 s.
- Six camera shots: open sea toward the LST, a chase on PBR 2, the surf from the sand, a crane
  over the landing, behind the LST toward the coast, and a pull-back reveal. At 120 s it fades
  out and ends through an END1 trigger (this build has no `endMission` command).

Afternoon (15:00), 21 June 1985, so the sun sits behind the seaward shots, light cloud. The player is an officer riding in the LST and never
seen. `init.sqs` holds the whole timeline; positions in it are `[east, north, height above
the surface]`. `mission.sqm` is generated from Everon's heightfield so every beach point sits
at the waterline.
