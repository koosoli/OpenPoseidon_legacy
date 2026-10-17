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
