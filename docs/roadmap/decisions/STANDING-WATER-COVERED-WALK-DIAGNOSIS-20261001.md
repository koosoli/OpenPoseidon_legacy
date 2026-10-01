# Covered standing-water contact fixture diagnosis

Source-only follow-up on `0f8547fd`. Installed acceptance remains pending.

The retained campaign `build/standing-water-footsteps/actual-natural-pool-20261001-032442-f71ac3` ran the matched installed `9527c006` pair and exited normally with code zero. Its overall status is failed. Reading the actual stage values and complete contact log establishes a more precise result:

| Stage | Actual result |
|---|---|
| Dry ordinary player walk | 4.39 m measured before release; start 2686/5125, final 2690.42/5128.42. Three fresh normal sand/gravel audio dispatch rows, available waves, no stored water or water selection. |
| Exposed ordinary player walk | 6.04 m measured before release; start 2550/5072, final 2554.22/5077.45. Four actual `standing-rain-water` rows, two authored `water_R` and two `water_L`, waves available. Actual depths 23.7–25.8 mm with fresh sole/support proof. |
| Covered ordinary player walk | Start 2550/5074.25, requested only 0.7 m at time acceleration 0.2. Released at 2550.48/5074.87; after 0.6 simulation seconds of release settling, final 2550.85/5075.35. Actual clock 64192 → 65110 ms, living player. **No new sound-dispatch row**, therefore no accepted sheltered-audio negative. |

The generic error text occurred before the old runner stored `covered-walk`; it did not indicate a dry-stage failure. The successful exposed rows prove actual authored water selection and Restart dispatch for this pool and ordinary SoldierWB input. They do not prove an audible listener mix. The covered stage remains unaccepted; neither sound absence nor the nearby real roof licenses treating it as a passed shelter control.

## Source explanation and bounded fixture change

`Man::Sound` in `SoldierOldSim.cpp` prints a row whenever `_doSoundStep` is consumed, including sheltered, stale, unavailable-source and unavailable-wave cases. Seven total rows cannot exhaust its per-category 128-row trace caps. Therefore the covered stage's zero rows are upstream of the recorded sound admission/dispatch, rather than evidence that the shelter policy chose the wrong sample.

`SoldierOldMove.cpp` sets `_doSoundStep` when the actual primary animation crosses authored `soundEdge1` or `soundEdge2`. Distance alone cannot prove that edge happened. The first dry and exposed sole traces occur around one metre from their starting body pose; the fixture released W at only 0.7 m. This motivates a longer contact-bounded walk, but does not establish an exact universal first-edge distance or diagnose the installed underlying animation without fresh trace evidence.

The retained actual walks are diagonal despite `setDir 90` and `setDir 0`. `Soldier::KeyboardPilot` in `SoldierOldActions.cpp` can turn the body toward an out-of-range mouse-driven weapon aim in the ordinary external view. Holding the existing ordinary SDL LAlt look-around action avoids that branch. The fixture now holds LAlt during placement and walking, records requested/actual headings, and releases both held keys at cleanup. It changes no input binding, actor controller, movement, animation, audio source or sound gain.

The revised covered walk requests 1.1 m and requires an actual sound row before releasing W. It retains acceleration 0.2 for bounded polling and shortens release settling to 0.15 simulation seconds. The body is checked on every poll and after release within the strict central square of half-width 0.95 m; actual sole events must remain within the audited central 2×2 m square and independently report `sheltered` with supported stored water and an available non-water wave. The source roof is still the verified real CampEastC/stan_eastc, raised 3 m to clear its solid Geometry hull. No roof bounds are widened to rescue an event outside the supported region.

If no real sound edge occurs inside that square, this runner fails and retains fixture-unavailable evidence. It cannot force an animation, stamp, audio consumer or sound playback. Existing physical `PLAYER_GROUND_BOOT` traces are now enabled alongside sound traces, so a failure can distinguish a real physical boot edge from missing sound dispatch. Stage proof is assigned before assertions; poll positions, headings, real contact counts, input context and camera-effect state survive failures. The failure snapshot pauses and records actual player state before normal cleanup.

## Checks and remaining work

PowerShell parse/execution of `-SelfTest` and `git diff --check` passed. The self-test rejects a distance-only completion, an audio-only stationary completion, and body positions outside the strict covered window, in addition to the existing fresh-event/support/source/random-variant falsifiers. No game, GPU test, full build, deployment, or production C++ modification was performed by this agent.

Root must rerun the installed campaign to establish a genuine supported sheltered contact and roof-removed restoration. Preserve the existing overall failed result and its successful exposed-source evidence separately. If the new bounded walk still has no audio edge, inspect retained primary physical boot rows and exact headings before expanding the fixture or changing production sound logic. Actual listening or loopback capture is still required to establish perceived wet-footstep sound.
