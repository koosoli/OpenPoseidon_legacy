# Natural authored footstep variant collection

Source-only fixture follow-up on d095bf4a. No production sound, RNG, animation,
input control or physics code changes; no game/GPU/build/deployment by this
agent. Installed acceptance remains pending root's next run.

The actual matched installed 6cf9973e campaign
`build/standing-water-footsteps/bounded-covered-real-steps-20261001-040527-dcb4ff`
failed at the requirement to observe both authored water alternatives. Its
phase evidence is precise:

| Phase | Actual result |
|---|---|
| Dry walk | 4.80 m before release, three fresh sand/gravel waves available; no water selection. |
| Exposed walk | 6.19 m, four fresh actual `standing-rain-water` events, depths 24.6..25.6 mm, all `sound\people\water_l.wss`, waves available. |
| Exposed return | 6.19 m, four fresh actual water events, depths 24.7..26.1 mm, all `water_l.wss`, waves available. |
| Covered walk | **Not executed.** The two-variant assertion failed before creating the stock roof. |
| Roof removed and pause | **Not executed.** Neither phase passed or failed its functional admission gate. |
| Cleanup | Exit zero, normal shutdown; no forced cleanup. |

Eight valid actual water events establish this standing-water dispatch path,
not listener audibility. Eight identical alternatives do not establish a
missing authored alternative. `EntityAIType::GetEnvSoundExtRandom` selects the
existing authored entry using `GRandGen.RandomValue()` when its configured
variant count exceeds one. The earlier installed 9527c006 campaign observed
both authored water alternatives through real walking. We do not assume draws
are independent or change the generator, mission seed or authored distribution.

The runner now validates initial exposed events with all existing freshness,
water/support/sole, source and wave gates, then executes the strict covered
contact and roof-removal controls. Pair diversity is deferred to a final
assertion over actual exposed and roof-removed events, so chance sampling cannot
hide whether the shelter control was exercised. Source counts are retained
per phase, without treating sample names as anatomical sole identifiers.

If the combined valid events still lack a variant, the fixture permits at most
six extra ordinary SDL-W traversals and 45 wall seconds. Every pass rechecks
the existing 7 m wet lane against the current read-only water samples, places
the real actor at its ordinary ground start, then walks 4 m without forced
contacts. Body polls and release remain within half-width .35 m and half-length
3.3 m; each actual sole event must stay within the already sampled .6/3.5 m
lane. The existing square .95 m covered window is unchanged. Actual event
source, depth, support, age, wave and movement/time gates all remain strict.
Each attempt records the refreshed lane, placement, source counts and normal
walk evidence. It cannot broaden shelter bounds, play a sample directly,
replace RNG, advance water from an audio query or force an animation edge.

The final both-variant assertion still fails if this bounded collection is
exhausted. That failure retains the already executed functional controls and
actual counts, plus invocation line/stack, followed by ordinary owned cleanup.
It is not a successful audio-pair test or a missing-source diagnosis by itself.

`Test-StandingWaterFootstepsRuntime.ps1 -SelfTest` passes original actual
trace/support/source/stale/dry falsifiers plus single-variant functional
evidence, strict final pair rejection, six-attempt/45-second limits and narrow
lane refusal tests. `git diff --check` passes. Root must run the installed
fixture under the game lock, verify a real sheltered wet contact and normal
roof-removed restoration, inspect actual variant counts and retain audible
listener/loopback verification as a separate unresolved gate.
