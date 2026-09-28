# tw-showcase.eden — Tidewater water showcase

A 2-minute, hands-off camera fly-over for previewing the water: Everon's south-east bay (map
around 9800-10400 E, 1000-1600 N), where the sea floor shelves gently enough for the shore
waves to break.

- An LST (`CarrierW`, the Newport) holds about 450 m offshore, bow toward the beach.
- Four PBRs (`BoatW`) wait beside it, run in at 18 s, unload one squad each (a 24-man platoon)
  and back off to cover the beach; the squads run up to the flat ground behind it.
- Two UH-60s, a Chinook and a Cobra fly over the bay; two A-10 pairs pass at about 30 s and 95 s.
- Six camera shots: open sea toward the LST, a chase on PBR 2, the surf from the sand, a crane
  over the landing, behind the LST toward the coast, and a pull-back reveal. At 120 s it fades
  out and ends via the dev runner's `triEndMission "end1"` command.

Morning, 21 June 1985, light cloud. The player is an officer safely placed on dry ground and never
seen; the independent scripted camera covers the bay. This avoids ending the demo if an attempted
LST cargo assignment fails and the player would otherwise be left in the water. `init.sqs` holds
the whole timeline; positions in it are `[east, north, height above
the surface]`. `mission.sqm` is generated from Everon's heightfield so every beach point sits
at the waterline.

For the full two-minute interactive showcase, run `scripts/Start-TidewaterShowcase.ps1` from
PowerShell. It selects Tidewater, launches this mission without an automatic test exit, and writes
a dated log. Do not add `--check` or `--test-type screenshot`: those bounded smoke-test modes exit
shortly after mission startup or screenshot capture. The mission itself ends after 120 seconds.
