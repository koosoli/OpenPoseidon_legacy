# tw-showcase.eden — Open Poseidon - Tidewater Demo

A 2-minute, hands-off camera fly-over for previewing the water: Everon's south-east bay (map
around 9800-10400 E, 1000-1600 N), where the sea floor shelves gently enough for the shore
waves to break.

- An LST (`CarrierW`, the Newport) holds about 300 m offshore, bow toward the beach.
- Four PBRs (`BoatW`) wait off its bow and run in at 3 s, one squad each (a 24-man platoon).
  Each has a MOVE waypoint on the beach itself, then a TR UNLOAD there. The AI dithers in the
  surf near a waypoint on land, so for the last 60 m `land.sqs` cuts the engine and runs the
  boat straight up onto the beach point, where the TR UNLOAD puts the squad out; then the squad
  is sent up into the town behind the beach. Nobody is put out of a boat still in deep water
  (soldiers cannot swim).
- Two UH-60s, a Chinook and a Cobra fly over the bay; two A-10 pairs pass at about 27 s and 98 s.
- Six camera shots: open sea toward the LST, a chase on PBR 2, the surf from the sand, a crane
  over the landing, behind the LST toward the coast, and a final pull-back that starts low
  behind the troops entering the town and rises out over the bay. At 120 s it fades
  out and ends through an END1 trigger (this build has no `endMission` command).

Title card "Open Poseidon - Tidewater Demo" (large bold Steelfish with a drop shadow across the
top of the frame, `description.ext`) at the start and during the final pull-back. Music:
Seventh - Decide (`7thDecide`). Early morning (07:30), 21 June 1985, fairly overcast, so sun
glare off the water does not hide the landing. The player is an officer riding in the LST and
never seen. `init.sqs` holds the whole timeline; positions in it are `[east, north, height
above the surface]`. `mission.sqm` is generated from Everon's heightfield so every beach point
sits at the waterline.
