# Arma 2 missions straight from an installed Arma 2 folder, 2026-10-12

Second generation of [ARMA12-DIRECT-MISSIONS-20260910](../roadmap/ARMA12-DIRECT-MISSIONS-20260910.md),
after [the Arma 1 slice](arma1-direct-missions-20261011.md). Owner's first mission:
**SP_EyeForEye** (Chernarus). Branch `MrDudeX-arma2`.

## How to start it

```
OpenPoseidon.exe --arma2 "H:\SteamLibrary\steamapps\common\Arma 2" --arma2-mission SP_EyeForEye.Chernarus
```

`--arma2-mission` takes a mission folder name (`C0_FirstToFight.Utes`), a path inside the
mission archives (`campaign\missions\C0_FirstToFight.Utes`) or a full bank path.
`--arma2-campaign` starts a campaign, `--arma2-test-seconds N` plays N seconds and quits
(script errors are logged and the mission keeps running), `--arma2oa <folder>` names an
Operation Arrowhead folder explicitly.

## What is read, and from where

- **Nothing is copied.** `<Arma 2>\AddOns` is mounted read-only under the archives' own
  prefixes (`ca\chernarus`, `ca\missions`, ...). Folders named `Arma 2 Operation
  Arrowhead*` next to the Arma 2 folder (the Steam layout) are found on their own and
  add `Expansion\AddOns` and `Expansion\Dta`. `OP_A2_ONLY=1` keeps plain Arma 2.
- **BAF, PMC and ACR are not read.** Steam ships their data under
  `DLCsetup\<DLC>\<DLC>_data` as copy-protected archives (`hprotect` header) that only the
  original game unlocks. Any archive carrying that header is skipped and logged
  (`... is copy-protected; not read`), whatever folder it is in.
- **Config.** Every `config.bin` (raP) of the mounted archives plus `Dta\bin.pbo` is read
  privately, merged in `requiredAddons` order and parsed once into a private config
  (`Arma1Compat::ConfigRoot`). CWA's own `Pars` is not changed; scripts' `configFile`
  resolves in the Arma 2 config first.
- **Worlds** come from the Arma 2 `CfgWorlds` (Chernarus, Utes, and with OA Takistan,
  Zargabad, Desert_E, ...). Their named places (`CfgWorlds >> world >> Names`) back the
  script locations (`nearestLocation`, ...).
- **Models / animations.** ODOL v48/49 and OPRW 24/25 use the existing readers.
  Binarised `.rtm` (`BMTR`) is decoded: per phase, LZO-compressed (arrays >= 1024 bytes)
  14-byte bone transforms (int16 quaternion / 16384 + three half-float translations);
  the quaternion is conjugated relative to CWA's convention (checked by a pivot
  least-squares fit, rms 5e-5).
- **Text.** `stringtable.xml` is read (converted to CWA's CSV form); `<localized id=../>`
  in briefings is resolved.

## Script surface added (SQF)

**Scheduling.** Every `spawn`/`execVM`/`addAction` script runs as a stackful coroutine
(Windows fiber, POSIX ucontext): `sleep` and `waitUntil` suspend anywhere -- inside
called functions, loops and conditions -- and a while loop holding the frame longer than
~3 ms gives it back, as Arma 2's scheduler does (`OP_A2_FIBERS=0` restores the Arma 1 line
compiler). Stringtable keys are case-insensitive, as in Arma.

Single-quoted strings; `#include`/`#define` through the real preprocessor for
`execVM`/`preprocessFile(LineNumbers)` (also `\ca\...` absolute paths); `exitWith` in
scheduled code; growable parser stacks (literal arrays of thousands of values); a
trailing `;` keeps a code block's value; spawned code keeps a reassigned `_this`.

Commands: `setVariable`/`getVariable` (objects, groups, `missionNamespace`,
`uiNamespace`), simple tasks and the diary (shown in the map notebook as *Plan* and
*Main*), the conversation system (`kbAddTopic` reads the `.bikb`, `kbTell` shows the line
and plays the recorded speech, `kbWasSaid` true once the recording has played out, `9e46264`), scripted FSMs (`execFSM`, one transition
per simulation step; FSM-editor `"..." \n "..."` strings joined), locations
(`createLocation`, `nearestLocation(s)`, `locationPosition`, `text`, `setText`, ...),
`createVehicle [..]`, `group createUnit [..]`, `isKindOf` on class names,
`setVehicleInit`/`processInitCommands`, `attachTo`/`detach`, `addAction` with arguments
and code, `triggerActivated`/`triggerArea`, `currentWaypoint`/`setCurrentWaypoint`,
`emptyPositions`, `WFSideText`, `mapGridPosition`, layered `cutText`/`cutRsc`,
`endMission`/`failMission`, `allUnits`, `allGroups`, `nearestObjects`, `nearEntities`,
`getPosATL`/`setPosATL`, and presentation-only commands (post-process effects, loading
screens, UI list boxes, high command) accepted without effect.

The Arma 2 module objects (`FunctionsManager`, MP framework, ...) start from their class's
init event handler, as in Arma 2.

## Engine fixes found on the way (not Arma 2 specific)

- `EntityAI::NeedsAmbulance` spun forever for a unit that is its own person (virtual
  tail call compiled to a jump) -- found by the stall report below.
- `ArcadeTemplate` counts every loaded unit id before `Compact` (a short read no longer
  indexes past the table).
- An unknown vehicle class is skipped with a warning in compat mode instead of failing
  the mission start.

## Diagnostics

In test mode (`--arma2-test-seconds`), a stalled simulation is reported every 10 s with
the expression the evaluator last started, and at 10 s / 30 s with the main thread's
symbolised call stack. `OP_A2_WATCH="var,var"` and `OP_A2_EVAL="expr"` log values every
10 s of game time.

## Results so far

Every mission is played by `.a2build\run-a2-tests.ps1` (one log and one screenshot
each): 40 s from run 7 on. Until run 6 the screenshot, taken 20 s after the mission
started loading, also ended the run, so missions played only about 10-20 s. The `Templates\*` and `MPTemplates\*` folders are editor templates without a world
and are not counted.

| Run | Build | Missions | Enter the mission | Without a script error | Hangs / crashes |
|-----|-------|----------|-------------------|------------------------|-----------------|
| 3 | `a142bf4` | 34 (Arma 2, partial) | 34 | 19 | 6 hangs (Warfare: `NeedsAmbulance`) |
| 4 | `6651f0d` | 63 (Arma 2, incl. templates) | 50 | 23 | 0 |
| 5 | `f4a567d` | 98 (Arma 2 + Operation Arrowhead) | 91 | 27 | 7 crashes (flag proxy, fixed `47c5b1c`), 5 OA campaign time-outs |
| 6 | `516bc8b` | 98 (Arma 2 + Operation Arrowhead) | 98 | 63 | 1 stall (CE5A: pathfinding, guarded `534f5f3`) |
| 7 | `4799c30` | 98 (Arma 2 + Operation Arrowhead), 40 s each | 98 | 96 | 0 |
| 8 | `f02e242` | 98 (Arma 2 + Operation Arrowhead), 40 s each | 98 | 96 | 0 |
| 9 | `06789da` | 98 (Arma 2 + Operation Arrowhead), 40 s each | 98 | 97 | 0 |
| 10 | `9e32ad8` | 98 (Arma 2 + Operation Arrowhead), 40 s each | 98 | 96 | 0 |
| 11 | `c44ba31` | 98, laptop (Intel Iris Xe, `--render gl33`), 40 s each | 98 | 97 | 1 hang (SP_WarWelcome: modal `hintC`, fixed `7bf71f5`) |
| 13 | `9fa39fe` | 63 of 98 (stopped for a newer build), laptop gl33, Arma 2 soldiers by default | 63 | 62 | 1 crash (TE06_Parachute: water without a section), 1 stall (C6B_DogsOfWar: `AILocker::LockPosition`), both fixed `3dfb515`; MP_Detector exited silently once, passed on rerun |
| 14 | `d427d85` | 21 of 98 (stopped for the foliage fix), laptop gl33; first run with map objects on Arma 2 worlds | 21 | 20 | 0 |
| 15 | `b7e1654` | 98, laptop gl33, Arma 2 soldiers, map objects and foliage on every Arma 2 / OA world | 98 | 97 | 0 (TE06 and C6B, the run-13 failures, pass); the one script error is MP_Paradrop's own `setTriggerArea` |
| 17 | `58afdbb` | 41 of 98 (stopped for the first-person diagnostics), laptop gl33 | 41 | 40 | 0; the one script error is MP_Paradrop's own `_targetmarkerdir`. Census of the first 29: 3078 entities with the Arma model, 77 CWA civilians -- all animals, fixed `601bf9f` |
| focus 7 | `5253d47` | 6 (T01, MP_Superpowers, SPE1_Vehicles_Allies, T05, SP_TrialByFire, C2B), laptop gl33, `OP_A2_PROXYLOG=1` | 6 | 6 | 0; census 607 Arma, **0 CWA**; armed HMMWV / Land Rover carry a gun; T05's stray waypoint label gone. Found the soldiers posed as their mirror image (face to the back) and the white concrete walls, fixed `dee0e27`, `a7f3a32` |
| focus 8 | `a7f3a32` | 8 (MP_Superpowers, T01, SPE1_Vehicles_Allies, T03, SP_TrialByFire, C2B, MP_UrbanWarfare, C0), laptop gl33 | 8 | 8 | 0; census 763 Arma, 0 CWA. First person shows the rifle in both hands at the shoulder (M16A4/ACOG, M4/EOTech), FR and ACR soldiers see past their own gear, third person shows every soldier's back to a camera behind him; SPE1's concrete walls are textured |
| 18 | `a7f3a32` | 34 of 98 (stopped for the next build), laptop gl33 | 34 | 33 | 0; the one script error is MP_Paradrop's own; census 4581 Arma, **0 CWA** -- no regression from turning the soldiers round |
| focus 9 | `31df49c` | 8 (MP_Superpowers, SPE1_OneShotOneKill, C0, MP_Deathmatch, SPE1_Vehicles_Allies, C2B, T01, MP_UrbanWarfare), laptop gl33 | 8 | 8 | 0; census 653 Arma, 0 CWA. Every armed soldier now carries rounds for his gun (RPK 8x PK, FN FAL M21, Bizon HK, MAAWS Carl Gustav); the CDF/TK machine gunners hold the Arma 2 PK; the LHD deck crew wear their seven colours. The M107 had M21 rounds but an empty gun, fixed `3c30bb1` |
| 19 | `f77d01a` | 98, laptop gl33 | 98 | 97 | 0; the one script error is MP_Paradrop's own; census **13042 entities with the Arma model, 0 CWA**. Only shotgun slugs and laser batteries still find no CWA round (fixed `4562178` for the shotguns). MP_Detector and MPE1_Dogfighters end with the player killed: the enemy now carries rounds, and the A-10 nobody flies |
| focus 10 | `e783482` | 7 (MP_Superpowers, SPE1_OneShotOneKill, MP_Paradrop, C2B, MP_CivilWar, C0, TE02), laptop gl33 | 7 | 6 | 0; the one script error is MP_Paradrop's own; census 608 Arma, 0 CWA. Hands natural by day and dark at night, TE02's bushes have leaves, the aiming view engages (M16 iron-sight overlay). Found: the M107's Arma 2 2D scope drawn as a box in flat colour (`8e1af97` keeps CWA's scope), Arma 2 vehicles washed grey under GL33 (`949ee53`), Arma 2 rifles aiming through CWA's M16 sight (`fa764b4`, 3D sights) |
| focus 11 | `33b7181` | 8 (C2B, TE02, MP_CivilWar, MP_Superpowers, T03, C0, SPE1_OneShotOneKill, SPE1_Vehicles_US), laptop gl33 | 8 | 8 | 0; census 693 Arma, 0 CWA. Arma 2 3D sights (SCAR EOTech, AK irons, M16A4 ACOG) and the M107's 2D mil-dot scope work; C2B's M4 GL still over CWA's mask (fixed `7fe3564`); A/B runs 11b-d trace the grey striped vehicles to GL33 shadow self-comparison (fixed `faad391`) |
| focus 12 | `faad391` | 6 (C2B, TE02, T03, T06, SP_TrialByFire, MP_Superpowers), laptop gl33 | 6 | 6 | 0; census 547 Arma, 0 CWA. The normal offset alone left the HMMWV's stripes (rule for back faces `e0bbaac`); T06's view through the sights black (`964ec29` logs the camera) |
| 20 | `33b7181` | 21 of 98 (stopped for `faad391`), laptop gl33 | 21 | 20 | 0; the one script error is MP_Paradrop's own; census 2357 Arma, 0 CWA |
| 20b | `faad391` | 48 more of the 98 (runs 20 + 20b: 69), laptop gl33 | 48 | 48 | 0; census 5663 Arma, 0 CWA |
| focus 13 | `d508f9e` | 6 (C2B, T06, T07, T03, TE02, MP_WarWelcome), laptop gl33 | 6 | 6 | 0; census 452 Arma, 0 CWA. The new object census shows camouflage nets drawn without their holes: ODOL section alpha bits were masked (fixed `4646571`) |
| focus 14 | `4646571` | 6 (T07, C2B, TE02, T03, C0, SP_TrialByFire), laptop gl33 | 6 | 6 | 0; census 600 Arma, 0 CWA. Camouflage net see-through, HMMWV camouflaged with no stripes, EOTech sights; C0 wrote no sight screenshot (its in-game display stopped updating after 14 s) |
| 21 | `4646571` | 98, laptop gl33 | 98 | 97 | 0; the one script error is MP_Paradrop's own; census 13038 Arma, 0 CWA -- the ODOL alpha change regresses nothing. Only texture names that failed: CWA's `biscamel\icamel2.paa` (every mission) and a procedural UI colour in MPE_SectorControl (fixed `9872760`) |
| focus 15 | `fb88b7c` | 6 (C0, SP_Counterattack, T01, CE0, C1, C3), 90 s, `OP_A2_EVAL` radio + sound | 6 | 6 | 0; census 838 Arma, 0 CWA. Conversation lines follow their recordings (C1: next line 0.0-0.6 s after the last ends); Counterattack's radio `.wss` read as `.ogg`, Eagle Wing's sounds by archive path; no missing CfgSounds |
| focus 16 | `d15a573` | 8 (T01, C2B, C1, C0, TE02, CE0, MP_Superpowers, SP_EyeForEye), laptop gl33, face pictures | 8 | 8 | 0; census 658 Arma, 0 CWA. Identities apply (Cooper hhl_27, O'Hara hhl_04, USMC soldiers random Arma 2 faces), CE0's listeners react (event handler, FSM). The face pictures show Arma 2 soldiers without heads -- old (run 19 the same); fixed `e01cf5c`, `b5727b2` |
| wgpu 1 | `d15a573` | 3 (T01, C2B, TE02), laptop **WGPU, DX12** | 0 | 0 | T01: renderer creation panicked writing to stderr (exit 1); C2B, TE02: ended by the test watchdog during the renderer's start-up (exit 3; it now allows 180 s, `4bf48c7`) |
| focus 17 | `4bf48c7` | the 8 of focus 16, laptop gl33, face pictures | 8 | 8 | 0; census 661 Arma, 0 CWA. Heads drawn, but turned round (face at the back); fixed `b5727b2` |
| focus 18 | `b5727b2` | the 8 of focus 16 | 7 | 7 | 0; census 606 Arma, 0 CWA. Heads face forward (C2B Cooper, O'Hara; MP_Superpowers, TE02). T01 ended after 32 s with exit 0 before the mission started (log ends at the mission start); rerun alone: entered, clean, 71 s -- not reproduced |
| focus 19 | `b5727b2` | T01, C2B, TE02 with `triShadowSetDarkness 1`; C2B, TE02 with `POSEIDON_GL33_SHADOW_SELF=0` | 3 / 2 | 3 / 2 | 0. Without shadow the polygonal light/dark patches on the faces are gone; without only the self-shadow rule O'Hara's face is clean, Cooper's (head down under his cap) keeps them. Default heads (`defaulthead` + `hhl_NN_co`) blurred, pale and eyeless in every run: their face texture was never registered (fixed `3e4dc87`) |
| wgpu 2 | `b5727b2` | T01, C2B, TE02, laptop **WGPU, DX12** | 0 | 0 | T01, TE02: the same stderr panic at renderer creation (exit 1; fixed `5422672`). C2B: renderer created after 44 s, then no frame for 2 minutes; watchdog after 180 s (exit 3) |
| focus 20 | `6e89b47` | T01, C2B, TE02, C4_BitterChill, MP_Superpowers, laptop gl33, `OP_A2_EVAL` `createSoundSource ["Sound_Fire", getPos player, [], 0]` | 5 | 5 | 0; census 614 Arma, 0 CWA. Default heads now sharp (skin, brows, stubble; `3e4dc87`). `createSoundSource` created the `fire` sound source in all 5 (heard: not checked). Shadow patches on faces unchanged (not touched) |
| wgpu 3 | `6e89b47` | T01, C2B, TE02, laptop **WGPU, DX12** | 0 | 0 | No stderr panic any more: the renderer is created in all three (~60 s), then no simulation step until the watchdog (180 s, exit 3) |
| wgpu 4 | `6e89b47` | T01, laptop **WGPU, Vulkan** | 1 | 1 | Renderer created in 60 s, mission entered and played to the end (exit 0, 154 s) -- but at a few frames per second, and no screenshots were written |
| 22 | `6e89b47` | 98, laptop gl33, face pictures | 98 | 97 | 0; the one script error is MP_Paradrop's own (as in run 21); census 13049 Arma, 0 CWA. No regression from the heads, faces and `createSoundSource`. Face pictures sampled (8 missions): sharp faces; the shadow patches remain; several default heads look as if their eyes were shut (the face's own bones are not posed -- open). SP_Counterattack's face shots show the road (player seated), CE5A's are dark (night) |
| focus 21 | `574ee4a` | MPE1_HuntersHunted, SPE1_DumSpiroSpero, CE2, TE11, T01, laptop gl33 | 5 | 5 | 0; census 1274 Arma, 0 CWA. Backpacks: 9 classes named theirs (CZ vest pouches, ACR and US assault packs, ALICE, civil assault pack); DumSpiroSpero's player (CZ_Special_Forces_TL_DES_EP1) wears his `backpack_acr_small` on his back in the posed third-person view (run 22 without it). HuntersHunted is at night -- not visible |
| 23 | `574ee4a` | 98, laptop gl33 | 98 | 97 | 0; MP_Paradrop's own error as before; census 13038 Arma, 0 CWA, every exit code 0. 109 backpack lines in the logs -- the backpacks regress nothing |
| focus 22 | `e412bd6` | the 8 of focus 16, laptop gl33, face pictures | 8 | 8 | 0; census 661 Arma, 0 CWA. Skinned heads: faces, eyes, headsets and collars sit as with the one-piece head (focus 20) -- no face torn from its skull. That the neck now follows the neck is not visible in these face pictures (not measured separately). C1, CE0, SP_EyeForEye: night/indoor, dark |
| 24 | `e412bd6` | 98, laptop gl33 | 98 | 97 | 0; MP_Paradrop's own error as before; census 13032 Arma, 0 CWA, every exit code 0 -- the skinned heads regress nothing. Its logs (and run 23's) show 21 missions whose description.ext failed to preprocess: every CE mission and every SPE scenario on `\CA\Missions_e\campaign\loadingscreen.ext`, Eagle Wing on `\ca\missions_EW\campaign\description_mission.hpp` -- fixed `27903ad` |
| focus 23 | `27903ad` | T01, C2B, C1, CE0, CE1, CE2, 60 s, laptop gl33 | 6 | 6 | 0; census 1304 Arma, 0 CWA. **Lip sync works:** the log names speakers opening the mouth by their .lip track (`Arma 2 lip sync: WEST Alpha Schwarz:1 opens the mouth (0.87)`, also CIVL speakers). No description.ext fails to preprocess any more (run 24: CE0-CE2 each failed). The 'Lip: NO MATCH' lines left are `say` sounds without a track in the data (weeping, a drunk) |
| 25 | `27903ad` | 98, laptop gl33 | 98 | 97 | 0; MP_Paradrop's own error as before; census 13056 Arma, 0 CWA, every exit code 0. No description.ext fails to preprocess (run 24: 21 missions) and the description.ext now read add no script error. 25 lip-sync openings logged |
| focus 24 | `131160c` | CE7B, CE0, T01, 60 s, laptop gl33 | 3 | 2 | CE7B's listener FSM loads (no parser error); its intro conversation now runs on to the HALO jump, whose script calls `expectedDestination` -- unknown, 178166 errors in 60 s (added `6c666cb`). CE0, T01 clean |
| focus 24b | `6c666cb` | CE7B, CE0, T01, 60 s, laptop gl33 | 3 | 3 | 0; census 162 Arma, 0 CWA. CE7B runs its intro conversation into the HALO jump (`time_to_halo` P_0, G_0-G_4) with no script error. The listener's `No, sir!` (O_0) was not told within 60 s -- not followed further |
| 26 | `6c666cb` | 98, laptop gl33 | 98 | 97 | 0; MP_Paradrop's own error as before; census 13017 Arma, 0 CWA, every exit code 0 -- `expectedDestination` and the FSM string joins regress nothing |
| focus 24c | `577755f` | CE7B, CE0, T01, 60 s, laptop gl33, FSM state log | 3 | 3 | 0; census 161 Arma, 0 CWA. All 4 CE7B listener FSMs go `React` -> `END`: the `_sentenceId in [...]` link never held. Cause: `_sentenceId` was stored under its mixed-case name, and variable lookup lowers the name -- the listener saw nil (fixed `e4ad2d5`) |
| focus 25 | `e4ad2d5` | CE7B, CE0, T01, 60 s, laptop gl33 | 3 | 3 | 0; census 162 Arma, 0 CWA, every exit code 0. **CE7B's listener answers:** on `beginning_in_hercules_G_3` its FSM goes `React` -> `No__sir_` and tells `beginning_in_hercules_O_0` (0.9 s), which reaches the player's event handler; G_0-G_2 still go to `END`, as their link condition says |
| 27 | `e4ad2d5` | 98, laptop gl33 | 98 | 97 | 0; MP_Paradrop's own error as before; census 13056 Arma, 0 CWA, every exit code 0 -- the `_sentenceId` fix regresses nothing. CE7B's listener answers here too (`No__sir_` once); 40 lip-sync openings logged |
| camp 1 | `397bef5` | C0, CE0 in played mode with `OP_A2_CAMPAIGN=30,end1,2`, laptop gl33 | 2 | 1 | **Campaign flow works for Operation Arrowhead:** CE0 started as mission of `ca_missions_e_campaign`, intro skipped, briefing confirmed, `END1` after 30 s, debriefing -> outro -> **CE1_GoodMorningTStan** -> `END1` -> **CE2_Pathfinder** (as the campaign's description.ext says), quit before CE2's mission; exit 0, no script error. C0 (Harvest Red) entered its campaign and mission but simulates at about one tick per second (vehicles stage ~700 ms/tick, in every run since run 22 after ~10 s, also in test mode), so 30 s of mission time never came: timeout after 480 s. The mode now counts wall-clock seconds (`07b033b`) |
| c0 prof | `397bef5`-`7cf3234` | C0, 60 s, `POSEIDON_SIM_VEHICLE_COST=1` | 1 | 1 | Vehicles stage up to 780 ms/tick, all in `man.base`; with `OP_A2_SOLDIERS=0` 0.3 ms/tick. Section timing: collision testing 12 ms per man (max 344 ms), 0 hits; soldiers radius 3.7 m, geometry 54 faces / 1 component |
| c0 cache | `439492f` | C0, 60 s, A: new RTM cache limit, B: `POSEIDON_ANIM_CACHE_MATRICES=32000` | 2 | 2 | A: vehicles max **8 ms/tick**, collision 0.011 ms per man, no reload line. B: 826 ms/tick, 2000 cache loads logged. Exit 0 both |
| camp 2 | `439492f` | C0, CE0 played, `OP_A2_CAMPAIGN=30,end1,2` | 2 | 2 | **C0 -> C1** and CE0 -> CE1 as the campaigns' description.ext say; exit 0, no script error |
| 28 | `439492f` | 98, laptop gl33 | 98 | 97 | 0; MP_Paradrop's own error as before; census 13033 Arma, 0 CWA, every exit code 0. No `Animation cache` reload line in any mission. Highest vehicles stage per mission: C0 6.5 ms (run 27: 616), MPE_SectorControl 30 (278), MP_Detector 108 (142; 15 samples over 50 ms -- not the cache, open), CE8 73 (run 27 logged no simulation sample for CE8) |
| det prof | `439492f` | MP_Detector, 60 s, `POSEIDON_SIM_VEHICLE_COST=1`, A: Arma 2 soldiers, B: `OP_A2_SOLDIERS=0` | 2 | 2 | 431 men. A: men 64-91 ms/tick, collision 0.001 ms per man, nearly all in `Man::Simulate`'s base (Person/EntityAI) section, 0.24-0.33 ms per man. B: men 6-20 ms/tick, base 0.12-0.21 ms per man. A big mission everywhere; Arma 2 soldiers cost roughly 1.5-3x more in the base section -- not followed further |
| camp 3 | `439492f` | C0, CE0 played, `OP_A2_CAMPAIGN=30,end1,12` | 2 | 2 | **Both campaigns played to their end on the END1 branch**, each step as its description.ext says: C0 -> C1 -> I1 -> C2 -> C2B -> I2 -> C3 -> C4 -> C5 -> C6 -> C7A (11 missions; C7A has no END1 successor) and CE0 -> CE1 -> CE2 -> CE3 -> CE4 -> CE5A -> CE6 -> CE8 (8). Both returned to the main menu (exit 4 = campaign over). One burst of 424 script errors in 56 ms at CE4 -> CE5A: CE4's UAV module loop still running into CE5A's intro (fixed `9ab0ef1`: an Arma 2 mission's scripts end with it). No animation-cache reloads |
| focus 26 | `9ab0ef1` | C2, C3, CE3 (the three that loaded the Javelin optic), 60 s, laptop gl33 | 3 | 3 | 0; census 922 Arma, 0 CWA, every exit code 0. `javelin_optic.p3d` now goes through the ODOL reader in all three (`canonical ODOL adapter`); no `Bad version` any more (run 28: 3x). Drawn through the optic: not checked |
| camp 4 | `9ab0ef1` | CE3 played, `OP_A2_CAMPAIGN=30,end1,3` | 1 | 1 | CE3 -> CE4 -> CE5A, quit before CE6; exit 0, **no script error** (camp 3: 424 at CE4 -> CE5A). At each mission end its scripts ended with it (16, 2, 3) |
| 29 | `9ab0ef1` | 70 of 98 (stopped for the merged build), laptop gl33 | 70 | 69 | 0; MP_Paradrop's own error as before; census 7937 Arma, 0 CWA, every exit code 0 |
| 30 | `77d1d4b` (merge of feat/jump-reload-lean) | 39 of 98 (stopped for `e99a7a3`), laptop gl33 | 39 | 38 | MP_Paradrop's own error as before; census 5460 Arma, 0 CWA. **Every process crashed after 'Shutdown complete'** (0xC0000005 in TextureGL33::ReleaseMemory from the atexit destructor of a static Ref<Texture> in the new muzzle-burst and underwater-bubble effects) -- fixed `e99a7a3` |
| 31 | `e99a7a3` (current state + exit-crash fix) | 98, laptop gl33 | 98 | 97 | 0; MP_Paradrop's own error as before; census 13059 Arma, 0 CWA, **every exit code 0**, no unhandled exception, no animation-cache reload. Highest vehicles stage: CE8 81 ms, MP_Detector 81, MPE_SectorControl 22, C0 under 15. The merged state regresses nothing in the Arma 2 missions |
| camp 5 | `1f07764` | 4 branches, one end per mission (`OP_A2_CAMPAIGN=30,<ends>,<n>` per line) | 4 | 4 | **Every branch as the campaigns' description.ext says, no script error:** C4 -END2-> I3 -END1-> C5B -END2-> C6B -END4-> C7D -END4-> campaign over; C5 -LOSER-> C5 again -END5-> C7C; CE4 -END3-> CE5B -END1-> CE6 -END4-> CE7B -END1-> CE8 -> over; CE6 -END3-> CE7A -END3-> CE7A again -END1-> CE8 -> over. Exit 4 = back at the main menu after the campaign's last mission, 0 = the given number of missions played |
| camp 6 | `1f07764` | C6 x2, C7C (one log per line) | 3 | 3 | C6 -END2-> C7B (END2 repeats C7B), C6 -END6-> C7EF (END6 repeats C7EF), C7C -END1-> campaign over; no script error. **Every mission of both campaigns has now been reached and played from its campaign** |
| long 1 | `1f07764` | 10 (T01, SP_EyeForEye, SP_WarWelcome, C1, C2B, C4, MPE_SectorControl, CE0, CE2, CE7B), **300 s** each, laptop gl33 | 10 | 10 | 0; census 1654 Arma, 0 CWA, every exit code 0, no script error over five minutes, nobody killed, no animation-cache reload. New warnings only from what happens later (muzzle flashes hidden, turrets turning). Not followed: CE2 `AISubgroupFSM.cpp(1638) check failed: veh == unit->VehicleAssigned()` (4x in 40 s, 39x in 300 s -- a get-in command for a vehicle the unit is no longer assigned to); the two 'stall' warnings were during loading (C4: 20 s), CE7B's dark frames are its night flight |
| long 2 | `1f07764` | 10 (T05, MP_Paradrop, SP_Counterattack, C3, C5, C7A, TE02, CE1, CE4, CE6), **300 s** each, laptop gl33 | 10 | 9 | MP_Paradrop's own known script error; census 1845 Arma, 0 CWA, every exit code 0. C7A ends itself after 39 s with end mode 2, as in run 31 (40 s) -- the mission's own end, not ours. CE1: the unattended player is killed after 134 s in the opening armour fight (M1A1/T72/BMP2 firing, rocket craters); the log names no killer, so cause not confirmed, but nothing points at a crash or a script fault. Only new error kind against run 31: `voice\\<speaker>\\.wss` not found (Adam, Greg, Peter, Rob), from the optional bearing callouts merged with `feat/jump-reload-lean` (`AIRadio.cpp` BuildBearingSentence speaks the word `at`, which the loaded CfgVoice >> Words does not define; warning `Radio word 'at' is not in CfgVoice >> Words`). The word stays silent, the radio message still goes through. Not changed: foreign feature, waits for a decision |
| long 3 | `1f07764` | 12 (MP_WhenDiplomacyFails, SP_BearRising, SP_SquadCommander, SP_TrialByFire, C2, C6, I1, MPE_SteelPanthers, CE3, CE5A, SPE1_DeathFromAbove, SPE1_Jackal), **300 s** each, laptop gl33 | 12 | 12 | 0; census 2493 Arma, 0 CWA, every exit code 0, nobody killed, no animation-cache reload; worst second 33.7 ms/tick (C2, once), otherwise under 20. SP_TrialByFire was won by the AI squad on its own: tasks 'Dorf sichern' and 'Flugfeld einnehmen' SUCCEEDED, campaign key BIS_SP01 activated, end mode 3 after 128 s -- the mission's own end. Its one load-time assertion (`TransportCrew.cpp(974) ValidateOutVehicle`, `AIUnit.cpp(1560) _state != InCargo`) is also in run 31. CE5A: 5x the known `AISubgroupFSM.cpp(1638)` check (as CE2). New error kinds against run 31: only the bearing-callout word `at` again (SP_BearRising, I1, CE5A), see long 2 |
| long 4 | `1f07764` | 12 (T03, T04, MP_CivilWar, SP_FreedomFighters, C6B, I2, I3, MPE1_Dogfighters, TE05, CE5B, CE7A, SPE1_SteelPanthers), **300 s** each, laptop gl33 | 12 | 12 | 0; census 1669 Arma, 0 CWA, every exit code 0, no animation-cache reload; one second each over 20 ms/tick in C6B (20.8), MP_CivilWar (23.4), I3 (51.3), otherwise under 20. MPE1_Dogfighters: player killed after 23 s, as in every earlier run (23-45 s; the unattended player's aircraft). Known engine checks: `AISubgroupFSM.cpp(1638)` 91x in CE5B (as CE2/CE5A), `AIUnit.cpp(1560) _state != InCargo` 4x in I2. New error text only: the known `Type is not primary for object` for a new object (blankets_ep1, CE7A, 19x) and the bearing-callout word `at` (CE5B) |
| long 5 | `1f07764` | 12 (T02, T06, MP_Superpowers, MP_UrbanWarfare, SP_BenchmarkSkirmish, C7D, MPE_MountainWarfare, TE08, TE10, TE13, SPE1_LaserShow, SPE1_Vehicles_US), **300 s** each, laptop gl33 | 12 | 12 | 0; census 957 Arma, 0 CWA, every exit code 0, nobody killed, no own mission end, no engine check or assertion, no animation-cache reload; one second at 39 ms/tick in C7D, otherwise under 16. New error kinds against run 31: only the bearing-callout word `at` (C7D, SPE1_LaserShow; speakers Dan, John, Peter, Rob) |
| long 6 | `1f07764` | 12 (T07, T08, MP_TeamDeathmatch, MP_WarWelcome, SP_BenchmarkBattle, C5B, C7B, C7C, MPE1_HuntersHunted, TE07, TE15, SPE1_DumSpiroSpero), **300 s** each, laptop gl33 | 12 | 12 | 0; census 1835 Arma, 0 CWA, every exit code 0, nobody killed, no animation-cache reload. MP_TeamDeathmatch ends itself with end mode 3 after 19 s, as in every earlier run (19-34 s). **SP_BenchmarkBattle: 18 seconds over 20 ms/tick, peak 96.9 ms** (mean 12.3) for about 50 s once the armour fight starts; all of it in the AI stage, and there in `attack=` (87 ms/tick for ~11 attack thinks/tick, i.e. ~8 ms each: `AIUnit::SetWatch` -> `FindCombatFiringPosition` / `EntityAI::AttackThink`). Not yet followed. Known engine checks: `AISubgroupFSM.cpp(1638)` 53x SPE1_DumSpiroSpero, 9x SP_BenchmarkBattle; `ArcadeTemplate.cpp(2036) nUnits > 0` once in MPE1_HuntersHunted; `TransportCrew.cpp(974)` twice in TE15 -- all three kinds already in run 31. New error text only: `Type is not primary` for metalcrate, and the bearing-callout word `at` (SPE1_DumSpiroSpero) |
| long 7 | `1f07764` | 12 (MP_Deathmatch, C7EF, MPE1_Littlebird, MPE1_OneShotOneKill, MPE_MountainWarfare3Sided, MP_UrbanWarfare.Zargabad, TE01, TE09, TE11, SPE1_Benchmark1, SPE1_HikeInTheHills, SPE1_Vehicles_TKA), **300 s** each, laptop gl33 | 12 | 12 | 0; census 857 Arma, 0 CWA, every exit code 0, no animation-cache reload; worst seconds 29.7 ms/tick (MPE1_Littlebird) and 27.8 (SPE1_HikeInTheHills), once each. MP_Deathmatch ends itself (end mode 3) after 16 s, as in every earlier run (15-37 s). **C7EF plays to its own end**: the closing conversation (c7f_reign_of_justice, FSM listener and the player's event handler answering) runs through, then the mission calls `endMission "end1"` after 196 s; earlier runs stopped it at 54-68 s. SPE1_HikeInTheHills: the unattended player is killed after 169 s among insurgents (earlier runs ended at 54-70 s, before contact); killer not logged. Engine check: `ArcadeTemplate.cpp(2036) nUnits > 0` once in MPE1_Littlebird (kind already in run 31). No new error kind, no bearing callout in these twelve |
| atfix | `a57a8c9` | 7 (SP_BearRising, C7D, I1, CE1, CE5A, CE5B, SPE1_DumSpiroSpero -- the missions whose long runs had the bearing-callout word `at` fail), **300 s** each, laptop gl33, **first run windowed 1920x1080 on a low-settings test profile** (`POSEIDON_USER_DIR=a2test\userdir_low`: preset Low, MSAA 0, grass/shadows/particles off, vsync off) | 7 | 7 | 0; census 1935 Arma, 0 CWA, every exit code 0. No `voice\<speaker>\.wss` error and no `Radio word` line at all: either every callout was Rob's (his own `at.wss`, taken silently in this build) or none came up -- the build cannot tell, so `5e1f938` logs the clip once. CE1: the unattended player killed after 245 s (long 2: 134 s). No new error kind against run 31 |
| 32 | `c04cd72` (merge of the owner's `2ff794f`) | all 98, 40 s each, laptop gl33, windowed 1920x1080, low-settings test profile | 98 | 97 | MP_Paradrop's own error; census 13046 Arma, 0 CWA, every exit code 0 -- same as run 31 (98/97, 13059). No new error kind; gone against run 31: the `Unknown action ERROR` lines (Arma 1 action names, from `2ff794f`) and three `Type is not primary` objects. Same 94 load stalls (run 31: 93), same CWA-side startup lines in every mission (`RscDisplayOptionsVideo` undefined base class, `biscamel\icamel2.paa`). **But the frame rate collapsed on the test profile**: T01 runs 32 sim ticks a second with 31 steps discarded in 4 frames (run 31: ~60 ticks, 4 in 3); the new PERF-030 log puts 200-270 ms per frame in land:gnd `water=`, and swaps take ~270 ms. The same profile on `a57a8c9` (atfix run) shows it too, so it is the profile, not the merge -- being narrowed with an A/B (view distance 50000 by default in the fresh profile, multitexturing off, MSAA) |
| ab | `5e1f938` | T01 five times, 40 s, windowed 1920x1080, one test profile each | 5 | 5 | **The frame-rate collapse of run 32 is the fresh profile's view distance**: a new `game.cfg` starts at `preferredViewDistance=50000` with `respectMissionViewDistance=0`. Low profile: 33 sim ticks/s; same with 1600 m: 61; multitexturing on: 32; 1600 m + multitexturing: 61. The test profile now has `preferredViewDistance=1600`, `respectMissionViewDistance=1` (missions set their own view distance, as in Arma 2) |
| tex | `5e1f938` | 12 (T01, MP_Detector, SP_EyeForEye, C2, C3, MPE_SteelPanthers, TE07, CE0, CE2, SPE1_Vehicles_Civilian/TKA/US), 40 s, screenshots, test profile **before** the view-distance fix | 12 | 12 | Units, vehicles and buildings textured as before -- but at ~4 fps the object streamer made only ~300 of 19992 objects resident in 40 s, so C2 opened in an empty field instead of the ruined factory of runs 26/focus 26. Texture checks need the frame rate |
| tex2 | `5e1f938` | same 12, 40 s, screenshots, **test profile with 1600 m** | 12 | 12 | T01 61 ticks/s, C3 59, C2 50, SP_EyeForEye 44 (MP_Detector 32, 431 men); every placement resident (C2 19992/19992). Screenshots: C2 back in the ruined factory, brick and plaster textured; Zargabad market and streets, Takistan base, Utes camp, uniforms, vehicles, the MV-22 all textured; German subtitles right. Low profile = no shadows, no grass. **Text bug seen**: CE2's task hint read `localize "str_ep1_ARM_Convoy2_ce2"` and its notes `localize "str_ep1_5po_ce2.sqf8"` (also in run 26) -- fixed in `32134d7` |
| 33 | `32134d7` (code is CODE in Arma 2) | all 98, 40 s, windowed 1920x1080, test profile with 1600 m | 98 | 88 | census 13050 Arma, 0 CWA, every exit code 0, stalls 93 (run 32: 94). **CE2's notes now read Durchführung / Einsatzbesprechung / Mission / Situation / Unterstützung** instead of `localize "str_..."`. New script errors in 9 missions (TE01/05/09/13/14, MPE1_LaserShow, MPE_SteelPanthers, SPE1_LaserShow, SPE1_SteelPanthers; 4454 in all), every one `_binds = actionKeysNamesArray |#|_this`: with code typed as CODE the OA hint framework (`fn_hints_main.sqf`, BIS_getKeyBind) now runs, and the command was missing in Arma 2 mode -- added in `59a1610`. Otherwise only MP_Paradrop's own `setTriggerArea` error |
| 34 | `59a1610` (actionKeysNamesArray) | all 98, 40 s, windowed 1920x1080, test profile with 1600 m | 98 | 97 | MP_Paradrop's own `setTriggerArea` error only; census 13064 Arma, 0 CWA, every exit code 0, stalls 92. The 4454 `actionKeysNamesArray` errors of run 33 are gone; no new error kind against run 32 beyond a `Type is not primary` object (rack_ep1, already seen in run 31). Code typed as CODE costs nothing elsewhere |
| atfix2 | `59a1610` | the 7 bearing-callout missions, 300 s each, windowed 1920x1080, test profile with 1600 m | 7 | 7 | 0; census 1929 Arma, 0 CWA, every exit code 0, no `.wss` error. **The bearing callouts' `at` is spoken**, each speaker from his own clip: `voice\Adam\at.wss`, `voice\Dan\at.wss`, `voice\Peter\at.wss`, `voice\Rob\at.wss` (the CWA voice data ships one per speaker -- the earlier note that only Rob had one was wrong). Callouts came up in C7D, CE1 (3), I1, SPE1_DumSpiroSpero (2). CE1: the unattended player killed after 118 s again |
| long 8 | `59a1610` | 14 (MP_Detector, C0, MPE1_HikeInTheHills, MPE1_LaserShow, TE03, TE04, TE06, TE12, TE14, CE8, SPE1_Littlebird, SPE1_OneShotOneKill, SPE1_Vehicles_Allies, SPE1_Vehicles_TKG), **300 s** each, windowed 1920x1080, test profile with 1600 m | 14 | 14 | 0; census 1278 Arma, 0 CWA, every exit code 0, nobody killed, no own mission end, no new error kind against run 32. Sim rate 61 ticks/s in the bootcamps and SPE1 missions; **CE8 56 (76 s over 20 ms/tick, peak 92), MP_Detector 30 (peak 486)**. MP_Detector: `AIUnitImpl.cpp(2642) check failed: grp` 411x in five minutes -- IncreaseExperience for a killer without a group, guarded (returns); present since run 30 in every MP_Detector log. MP_Detector's cost being profiled (`35b38da`) |
| det2 | `35b38da` | MP_Detector 60 s twice, `POSEIDON_SIM_VEHICLE_COST=1`, A Arma 2 soldiers, B `OP_A2_SOLDIERS=0`; test profile | 2 | 2 | Man::Simulate per call: A base 0.28 ms (max 3.2), B 0.14-0.17 (max 4.5); every other Man section under 0.002. **EntityAI::Simulate itself costs nothing** for men (lock, weapons, flag, burst, recoil, laser all under 0.001 ms). By-kind man stage 28-53 ms/tick in both arms. The time is between: VehicleSupply's supply step or Person's rain-shelter probe -- `b08424b` splits those |
| det3 | `b08424b` | as det2 | 2 | 2 | **Found**: Person::Simulate `base` (VehicleSupply + EntityAI) 0.001 ms per call; the uniform-wetness part 0.146-0.245 ms per call while it rains, from ~400 rain-shelter probes per 3000 calls -- two 30 m `ObjectCollision` rays per man per second, 1-2 ms a pair on Chernarus. In a window where the rain had stopped: 0 probes, wet 0.000. Same in both arms (Arma 2 and CWA soldiers): it is the rain, not the models. `873fa5d`: a man within 0.5 m of his last probe keeps the answer, re-asks after 10 s |
| det5 | `873fa5d` | MP_Detector 60 s three times, `POSEIDON_SIM_VEHICLE_COST=1`, Arma 2 soldiers, test profile | 3 | 3 | **58-60 sim ticks/s (det3 on `b08424b`: 32)** with rain in all three (2586-2588 shelter probes in 60 s, i.e. each of the 431 men every 10 s instead of every second); uniform-wetness part at most 0.163-0.177 ms per call in the windows that hold the 10 s re-probes, otherwise 0. Man stage ~1.2-1.4 ms/tick where it was 10-40. (det4, one run on the same build, had no rain at all -- 0 probes -- and is not a comparison.) Full regression run 35 started |
| 35 | `873fa5d` (rain-shelter probe) | all 98, 40 s, windowed 1920x1080, test profile with 1600 m | 98 | 97 | MP_Paradrop's own error only; census 13049 Arma, 0 CWA, every exit code 0; no new error kind against run 34 (three more `Type is not primary` object names). Sim ticks/s against run 34: **MP_Detector 32 -> 59, MP_CivilWar 48 -> 61, MP_Superpowers 43 -> 52**; lower this time in C1 (62 -> 44), SP_BenchmarkBattle (62 -> 50), MP_WhenDiplomacyFails (60 -> 48) -- but C1 simulates in about 1 ms per tick, so these are frame-bound (rendering on the laptop's Iris Xe), and the same missions moved by 10-20 between runs 33 and 34 too. Nothing in the change adds work: a moving man is still probed every second |
| 36 | `dbff02e` (merge of the owner's `MrDudeX-wgpu-aktuell` @ `748c0ba`: script lights, vehicle tail/brake lamps, WGPU-035/036) | all 98, 40 s, windowed 1920x1080, test profile with 1600 m | 98 | 97 | MP_Paradrop's own error only; census 13073 Arma, 0 CWA, every exit code 0, stalls 91. One new error line: `Skipping visible light volume because its shape failed to load` once each in C1, CE5B, CE7B -- the new shapeless point lights (script lights, automatic tail/brake lamps) reaching the volume draw; nothing failed to load. Quietened in `117a84e` (draw unchanged). Ticks/s: C1 44 -> 62, MP_CivilWar 61 -> 43, MP_Detector 59 -> 38 -- frame-bound swings again: MP_Detector's simulation costs the same 2.5 ms per tick in both runs |
| lights | `117a84e` | 7 (C0, C1, C2, C4, CE5B, CE6, CE7B -- missions whose scripts make `#lightpoint`s), 40 s, screenshots, test profile | 7 | 7 | Every exit code 0, no `visible light volume` line any more. Screenshots: CE5B's task hint now reads 'Räumen Sie Rasman' (run 26: `localize "STR_EP1_addtasks.sqf7_1"`) but drew its line break as a box -- fixed in `afc7d93` (the hint control breaks at backslash-n). C4: one or two hard-edged grey rain-streak rectangles hang in the sky (also one in run 26, so not from this merge) -- the distant rain rendering, not Arma 2 content; noted, not changed. CE5B's tree lit white beside the convoy is in run 26 too. `has no CWA counterpart` checked over run 36: `FireLit` and the `DATSUN_PK*_NAPA/GUE` pickups are not defined as classes anywhere in the Arma 2 / OA data (FireLit only in misc.pbo's CfgPatches units list), so Arma 2 cannot create them either |
| hint | `afc7d93` | C3, CE2, CE5B, 40 s, screenshots, test profile | 3 | 3 | Every exit code 0. The task hint reads on two lines: 'Current task: / Beschützen Sie den Konvoi' (CE2; run 26: `localize "str_ep1_ARM_Convoy2_ce2"`), 'Current task: / Räumen Sie Rasman' (CE5B) |
| 37 | `aae1bbd` (no task hints of our own) | all 98, 40 s, windowed 1920x1080, test profile with 1600 m | 98 | 97 | MP_Paradrop's own error only; census 13027 Arma, 0 CWA, every exit code 0. The `visible light volume` line is gone; no new error kind (one more `Type is not primary` object, table_small_ep1) |
| voice | `919e7e1` (Arma 2 voices) | SP_BenchmarkSkirmish, C0, C1, CE2, 60 s, test profile | 4 | 4 | Every exit code 0. Every Arma 2 man is given a voice of his identityTypes (`'A1x_USMC_Soldier' speaks with 5 Arma 2 voice(s)`). The sound census at the end read 0 files: it was never fed by the Steam Audio backend (fixed in `ff9849f`) |
| voice2 | `ff9849f` | SP_EyeForEye, SPE1_Littlebird, SP_BenchmarkBattle, MP_Detector, 120 s, test profile | 4 | 4 | Every exit code 0. Littlebird's radio: `ca\dubbingradio_e\RADIO\Male04EN\DEFAULT\numbers\One.ogg` (4 plays), `...\Pilot.ogg`, `...\vehicles\helicopter.ogg`, `ca\DubbingRadio_E\Data\radionoise1.ogg`, `over02.ogg`; no CWA voice file. Two words not in Arrowhead's protocol (`GetIn`, `unknown`; fixed in `b4eb960`). The other three had no radio traffic in 120 s |
| 38 | `b4eb960` (Arma 2 voices, Arrowhead word names) | all 98, 40 s, windowed 1920x1080, test profile with 1600 m | 98 | 97 | MP_Paradrop's own error only; census 13045 Arma, 0 CWA, every exit code 0, no new error kind. Radio in 6 missions, all Arma 2 clips (Male01/04/05EN, radio noise, over01-03); no `Radio word` or `no clip` line. One CWA voice still asked for: MPE_SectorControl's `voice\Adam` (the player's profile voice; `dd365dc` gives the player an Arma 2 voice too). SP_Counterattack asked for `ca\dubbing\GLOBAL\RADIO\Male04\RU\...` (the enemy's radio, not played to the player) |
| animals | `6f48115` (animals placed) | the 13 missions with animals, 60 s | 2 | 0 | Both stopped at load: `A1_CfgMovesCow/States/Cow_Stop.connectFrom` missing -- the animals' moves were imported one by one and could not find their bases (`Undefined base class 'StandBase'`); stopped by hand. Fixed in `7719879` |
| animals2 | `7719879` | the 13 missions with animals, 60 s, screenshots | 13 | 13 | Every exit code 0, census 1914 Arma, 0 CWA; every animal placed with its own model (13 goats, 8 cows, 5 sheep, 4 dogs, 3 hens, 2 cocks, 1 boar kinds over the missions), its agent task started, no script error. But no animal had a move (`move ''`) |
| animshot | `fc8bdc6`, `e0d334c`, `d054c42` | 7 missions, one animal kind each (`OP_A2_SHOT_CLASS`), 40-90 s, screenshots | 7 | 7 | `fc8bdc6`: animals without a move, the cows lying flat in the ground. `e0d334c` (a man's degree finds a map, `fc76b4d`): `cow_stop`, `hen_stop`, ... run, but the bodies are twisted. **`d054c42` (animals as skinned rigs): cows, sheep, goat, hen, cock and boar stand on the ground in their own shape** (dog: shot too dark at night). Rest heights read from each rig's standing move: cow 1.20 m, sheep 0.72, goat 0.67, dog 0.61, boar 0.65, hen 0.27, cock 0.29. In 90 s no animal walked yet (`motion still`); whether main.fsm runs is measured next (`2cf5bb3` logs its states) |
| animfsm | `2cf5bb3`, `2804f0f`, `e20e161` | T01 (cows, sheep), TE02 (sheep), 120-150 s | 2 | 2 | `2cf5bb3`: the animals' main.fsm never entered a state -- the binary `execFSM` was shadowed by an ignoring entry (fixed `02f8318`). `2804f0f`: the FSMs run, 110856 script errors for the missing `moveTo`/`moveToCompleted` (added `71ca6b5`/`e20e161`). `e20e161`: no script error; positions every 15 s: TE02's 9 sheep graze (`sheep_eat`) and wander apart over 150 s, T01's 7 sheep graze and drift; T01's 5 cows stand (`cow_stop`) and do not move in 150 s -- open |
| 39 | `e20e161` (every scripted FSM runs) | all 98, 40 s, windowed 1920x1080, test profile | 98 | 67 | **Regression**: with the binary `execFSM` working, every FSM of the missions and modules started (mission.fsm 41 missions, agony.fsm 25, fn_taskHandler 24, ALICE, SILVIE, ambient animals and combat, SOM, zora, weather ...). 30 more missions with script errors from commands not implemented: `isFlatEmpty` (1.9 million), `nearObjectsReady`, `selectBestPlaces` with 5 arguments, `setUnconscious`, `setGroupIcon`, `nearTargets`, `triggerText`, `triggerActivation`, `removeAllItems`; str_EW2 crashed (0xC0000005 in Turret::AnimateMatrix from an FSM's moveInDriver into a helicopter). Census 13005 Arma, 0 CWA. `c93353c` runs only the animals' FSMs again until these are in (`OP_A2_EXECFSM=all` runs all) |
| 40 | `c93353c` (only the animals' FSMs by execFSM) | all 98, 40 s, windowed 1920x1080, test profile | 98 | 97 | MP_Paradrop's own error only, every exit code 0; census 13249 Arma (204 more than run 38: the animals), 0 CWA. New error lines are only the CWA man code asking animal skeletons for a soldier's bones (`Bone 'zbran'/'roura'/'lruka'/'pruka' not found` in the animal models), without effect |
| 41 | `30c060e` (every FSM, `OP_A2_EXECFSM=all`) | all 98, windowed 1920x1080, test profile | 98 | 67 | No crash any more (the str_EW2 turret crash fixed in `30c060e`), but the world commands of `265d851` (`isFlatEmpty`, `selectBestPlaces`, `nearTargets`, ...) were still unknown: their module was dropped by the linker. Registered from the Arma 2 module in `ae36d97` |
| 42 | `ae36d97` (every FSM, `OP_A2_EXECFSM=all`) | all 98, windowed 1920x1080, test profile | 98 | 85 | Census 13265 Arma, 0 CWA. Two crashes (MPE1_HuntersHunted, TE15_UAVs, exit code -1073741819) in `A1Case`: a `switch` kept a pointer into a script that had been dropped (fixed `3270c87`). Remaining errors: `taskDestination` of a task without destination gave `[]` (11 missions) and `actionKeysNames` an array (both fixed `da45c45`); MP_Paradrop's own `setTriggerArea` error |
| 43 | `da45c45` (every FSM, `OP_A2_EXECFSM=all`) | all 98, 30 s, windowed 1920x1080, test profile | 98 | 96 | Every exit code 0: no crash (MPE1_HuntersHunted and TE15_UAVs run through, `3270c87`), no `taskDestination` or `actionKeysNames` error any more. Census 13282 Arma, 0 CWA. Left: MP_Paradrop's own `setTriggerArea` error, and TE15_UAVs' `getGroupIcons` unknown (6 errors; group icons kept per group in `eae0482`) |
| t01walk | `da45c45` | T01, 150 s, positions every 15 s | 1 | 1 | Exit code 0. The cows still stand and `isNull group` is still true for every animal: `5495a1e` joined the agent with `join`, which skips a unit that has no group yet. Fixed in `418e2fa` (the agent is added like a unit from `createUnit`, and the civilian centre is created when the mission has none) |
| t01walk2 | `418e2fa` | T01, 150 s, positions every 15 s | 1 | 1 | Exit code 0. Every animal joins its own civilian group on its first `moveTo` (`isNull group` false). **The cows walk**: `cow_walk`, `cow_turnr` and their idle moves (`cow_idle_pasenipopochazeni`, `..._postavanirozhlizise`, ...); one cow moves from 3950,3389 to 3955,3394 via 3945,3397, another 3940,3378 to 3935,3374. The 7 sheep graze (`sheep_eat`), walk and turn, and spread over about 40 m |
| 44 | `eae0482` (every FSM, `OP_A2_EXECFSM=all`) | all 98, 40 s, windowed 1920x1080, test profile | 98 | 97 | MP_Paradrop's own error only among the 97 that ended, every exit code 0, census 13201 Arma, 0 CWA. TE15_UAVs froze 10 s into loading at the first stall report and was killed at 282 s: the report suspends the main thread and walked its stack with dbghelp, which waited on a lock the suspended thread held (fixed `c00b0b0`; a diagnostic fault, not the mission's) |
| weap | `e9dbd55` (Arma 2 weapon sounds, ammunition, magazines, vehicle weapons) | SP_BenchmarkSkirmish, T03, SPE1_Littlebird, 90 s | 3 | 3 | Every exit code 0, no script error. Hand weapons fire with their own Arma 2 samples (`m16_single1`, `ak74_single1`, `m249_single1`, `m240_single1` requested in the sound census), tank and AA guns with theirs (`gun120`, `2a38_4pcs_fast`). Magazines take Arma 2's count and ammunition (M249 200 rounds, M9 15, RPK 75-round magazine 45 as the installed config says). Vehicles rebuilt with their own weapons: M1A1 (M256 SABOT/HE + M240), AH-1Z, AH-64D, A-10, F-35B, BRDM-2, ZU-23, Avenger, TOW, MK19, AGS-30. Armour read in game: Arma 2 men 3, CWA soldiers 3 (Arma 2's `Man` is 3 too); bullet damage CWA 9, Arma 2 5.56 8, 5.45 7. CWA's burst sounds (`m16burst`, `ak74burst`) were still requested twice, from a weapon without its own shot (open). A burst with an own shot played it once for the whole burst; it sounds for every round since `11540b1` |

SP_EyeForEye (the owner's first pick), all eight Arma 2 Bootcamp missions and the
Arma 2 campaign through I3 enter and run with no script error. Operation Arrowhead's
Takistan and Zargabad render; its bootcamp and scenarios start.

Run 6 is the first with scripts as coroutines: the Operation Arrowhead campaign no
longer times out (CE0-CE8 all enter and run), and every one of the 98 missions enters.
What it found, fixed afterwards:

- **Config strings were cut at 2 KB.** FSM states keep whole scripts in one string
  (C1_IntoTheStorm's opening radio conversation is 5966 characters); the config parser's
  word buffer ended them mid-block. Values are now read up to 256 KB.
- **`exitWith` inside a `call`ed function** broke out of the caller as well, so
  `_side call GetSideLetter` gave nil and the Warfare unit tables were built from
  `<null>SOLDIER`. `call` now takes the exitWith value as its result.
- **`typeOf`** returned the import's `A1x_` class name; scripts look it up in their own
  class lists (C7B's convoy). It returns the mission's name now.
- **Locations are their own type** (`typeName` = `LOCATION`, `locationNull`, `isNull`),
  which `BIS_fnc_locations` relies on.
- **Pathfinding:** the operative-path field map only walks fields on the map; a 60 s
  stall in CE5A_Sandstorm was inside that loop.
- Commands: `addSwitchableUnit`/`removeSwitchableUnit`, `ctrlShown`, `inputAction`,
  `showHUD`, binary `hideObject`, `moveOut`, `forceWalk`, backpack commands.

Run 7 (all 98 enter, 96 without a script error, none hangs) followed one more find:

- **`rank="LIEUTENANT"`.** CWA spells the rank `LIEUTNANT`; the mission loader stops
  reading a class at its first unknown enum value, so 236 Arma 2 units lost their name
  and init line. In C1_IntoTheStorm that was the helicopter pilot: never put in the
  seat, the Venom carrying the player fell into the sea. Arma 2's other spellings in the
  shipped `mission.sqm` files (`AMBIENT LIFE`, `DIAMOND`, `FILE`, `GETIN NEAREST`,
  `DISMISS`, `... SEIZED`) are read as the nearest CWA value, and an unknown value is
  logged. C1 now flies its route with no script error.
- `format` prints nil as `any`, `call` accepts a nothing value on its left.

A rerun of the missions that ended early or had errors (build `877165b`, then `f02e242`)
left no script error in any of them. Missions now say why they end on their own:

- **SPE1_Jackal** starts with a HALO jump from an archive script given without its
  leading backslash; `exec` did not find it, so the player started on the ground among
  the enemy. Fixed: the player now falls from 2000 m (`bis_fnc_halo`), and in the
  unattended test hits the ground at 36 s because nobody opens the parachute.
- **MPE1_Dogfighters**: the player's A-10 starts in the air and, with nobody at the
  controls, crashes after 9 s -- expected without input.
- **MP_Deathmatch / MP_TeamDeathmatch / C7A_WarThatNeverWas** end by their own end
  conditions in single player (end modes 3 / 2).

Run 8 showed the HALO free fall itself spinning (SPE1_Jackal, 132780 errors): the SQS
runner wrote its own clock into `_time` around every step of a coroutine script, and
fn_halo keeps its frame time in `_time`, so `time - _time` stayed 0 and the division by
it failed before the loop's `sleep`. Coroutine scripts no longer get `_time`
(`22b7a49`), a scheduled script resumes in a later frame at the earliest, and `0 atan2 0` is 0.
Rerun: no script error in any of the six missions checked.

The two missions left with an error: MP_Paradrop uses a variable it never sets
(`_targetmarkerdir`; a mission bug, Arma 2 reports it too) and MP_WhenDiplomacyFails
(Warfare `<null>SOLDIER`, fixed after the run).

## Second machine (laptop, Intel Iris Xe), 2026-10-13

Build `2e82aea` from the `Arma 2 / Windows game build` workflow, installed by hand into
`ARMA Cold War Assault\Remastered` (Steam in `C:\Program Files (x86)`; Operation Arrowhead
next to Arma 2 is found on its own).

- **WGPU on Vulkan:** SP_EyeForEye loads and shows the briefing; on the first 3D frames
  after "Continue" the driver reports `wgpu device lost (Unknown)` and the game exits
  (code 1). Same with `OP_A2_FIBERS=0`, so not the scheduler. Renderer issue on this
  integrated GPU, not Arma 2 specific.
- **`--render gl33`:** SP_EyeForEye plays, no script error in the log, diary and tasks
  created.
- `WGPU_BACKEND=dx12` not yet tried.

## Open findings from the run 8 screenshots

Every mission now leaves a screenshot about 30 s into play (`.a2build\tests`). Most show
the expected scene (bootcamps, the Chernarus campaign, Operation Arrowhead's campaign and
scenarios). Not yet right:

- **Utes, SP_TrialByFire:** the LHD that is part of the Utes map is drawn in pieces --
  frames and catwalks without the flight deck, parts floating above it. The parts are
  map objects, so this is about placing composite Arma 2 models (their origin vs. their
  bounding centre), not about the mission.
- ~~**MPE_MountainWarfare (+3Sided):** the Warfare players stayed outside the map.~~
  Fixed for run 9: `scopeName`/`breakTo`/`breakOut` were ignored (Warfare's list of
  starting points filled up with nil), `worldName` did not exist (no towns found) and
  `synchronizedObjects` was always empty (no teams). All three work now, and the player
  starts at the base. A scan of every SQF/FSM in the Arma 2 and Operation Arrowhead
  archives for command names the engine does not know added about 30 more
  (`overcast`, `fog`, `rain`, `wind`, `cursorTarget`, `actionKeys`, `nearRoads`, ...).
- **MPE1_Dogfighters:** the player's A-10 starts in the air with the engine reported off
  and glides into the ground in about 9 s without input.

Run 9 leaves one mission with a script error, MP_Paradrop, whose script uses a variable
it never sets (Arma 2 reports it as well). After it: `exitWith` in the body of a
`while` or `for ... from ... to` loop ends the loop (it was lost with the body's own
context, so such loops never ended).

Run 10 (with the loop `exitWith` fix) shows no regression. With Warfare now starting
properly, MPE_MountainWarfare gets further and reports `createUnit` with an undefined
soldier class twice (`Format ["%1SOLDIER", side letter]`) -- open.

Run 11 is the first on the laptop and the first with Arma 2 soldiers carrying their own
weapons (`A1x_` weapon classes with the Arma 2 models) and Arma 2 vehicles crewed by their
own crew classes; see [the asset plan](../roadmap/ARMA2-ASSETS-20261014.md). No mission lost
anything to that. SP_WarWelcome stopped at a modal `hintC` 25 s in, which an unattended
test cannot dismiss; test mode now shows it as an ordinary hint.
