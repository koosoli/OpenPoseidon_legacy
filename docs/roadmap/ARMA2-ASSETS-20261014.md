# Arma 2 assets that load and work, 2026-10-14

Follow-up to [the Arma 2 mission slice](../verification/arma2-direct-missions-20261012.md).
The owner's goal (2026-10-14): Arma 2 missions are *playable* with Arma 2's own soldiers,
weapons, vehicles, aircraft and objects -- loaded correctly and working, not CWA stand-ins.

## Where it stands (build `364ebb0`)

What a mission class becomes today is decided in `Arma1Compat::ResolveVehicleClass` and
`Arma1ModelClass` (`engine/Poseidon/Game/Compat/`):

| Kind | Model | Behaviour (config, physics, animation) |
|------|-------|-----------------------------------------|
| Objects, buildings, props | Arma 2 model (`A1x_<class>`) | CWA base class picked by simulation |
| Vehicles, helicopters, planes, boats | Arma 2 model | the CWA vehicle of the same kind (`BRDM2_INS` -> `BRDM`, `MV22` -> `UH60`, ...): CWA speed, armour, crew seats, memory-point names |
| Soldiers | **CWA soldier model and moves** (`SoldierWB`/`EB`/`GB`) | CWA. Arma 2 models and moves exist behind `OP_A2_SOLDIERS=1` and **crash on the first frame** (`2e82aea`, Utes bootcamp, 0xC0000005) |
| Weapons, magazines | **CWA weapon** chosen by kind and calibre (`Arma2WeaponFallback`) | CWA |

So vehicles and objects already look like Arma 2, soldiers and weapons do not, and nothing
behaves like Arma 2 beyond what the CWA stand-in does.

## Done since (not yet measured on the laptop)

- `4851193` asset census in the log and `OP_A2_SHOT` screenshots (first and third person).
- `92ceb64` every Arma 2 soldier class carries its own `weapons[]`/`magazines[]`
  (resolved by kind and calibre) instead of the CWA stand-in's M16; each weapon is
  `A1x_<weapon>`, the Arma 2 model on the CWA weapon (magazines, ballistics stay CWA).
- `0e9804e` `createVehicle [type, pos, [], r, "FLY"]` starts an aircraft airborne with the
  engine on (MPE1_Dogfighters' A-10).
- `4b750f0` vehicles with the Arma 2 model take its `maxSpeed`, `armor`, `fuelCapacity`,
  `transportSoldier`.

- `c44ba31` Arma 2 vehicles are crewed by their own crew classes.
- `121cc8c` animals (sheep, cows, ...) are left out instead of walking about as civilians.

### Arma 2 soldiers (`OP_A2_SOLDIERS=1`), 2026-10-14

What it took, each step measured on the laptop or offline on Arma 2's own files:

| Commit | Symptom | Cause |
|--------|---------|-------|
| `33d95cf` | crash in `Man::DrawProxies` on the first frame | a proxy's selection point indexed unchecked; Arma 2 proxy selections are empty |
| `b41e75c` | soldier crumpled | bone selections took each bone's whole subtree at full weight (right for rotors, wrong for a skin): now per-pair weights; empty proxy selections get a vertex of their bone |
| `6c90af4` | missions stall > 60 s while loading | `Skeleton::Prepare` ran after each of ~2000 moves; now once |
| `8cca829` | limbs flung about | BMTR moves are **local** (rotation about the rest joint, parent first), mirrored in x, root = pelvis height above ground; and the ODOL 48/49 reader skipped the bounding centre the vertices are stored around. Edge stretch of the posed mesh 4.3 -> 0.17 (stand, walk, kneel, prone) |
| `3c27692`, `e4d47fe`, `f738f76` | waist-deep in the ground | an Arma 2 man's origin is at the hip; lifted to feet at y=0, kept out of auto-centring, and the land-contact point (one point at the hip) moved to the feet. `getPosATL player` -1.03 -> -0.02 |

| `9fa39fe` | no rifle in the hands, no head | a proxy's bone index is into the model skeleton, not the LOD's sub-skeleton; head proxies (-1) follow `head` |

`0139b92` makes Arma 2 soldiers the default (`OP_A2_SOLDIERS=0` for the CWA models): on the
12 asset missions 1241 entities are drawn from Arma 2 models and 20 from CWA (civilians).

### Run 13 and after (laptop, gl33), 2026-10-14

| Commit | Finding | Fix |
|--------|---------|-----|
| `797edce` | Arma 2 car wheels did not turn: the wheels exist only as bones (`wheel_<side>_<n>`), the LODs carry no wheel selection | the ODOL loader gives each wheel bone a selection; `CarType` takes those names when the CWA ones are missing (1 left / 2 right; 1 front, 2 back, 3 middle, 4 second front) |
| `3c6a383` | the player's aim turned his rifle about his feet | Arma 2 memory points `aimpoint`, `aiming_axis`, `head_axis`, `footstepl/r` are looked up when the OFP names are missing |
| `b25490e` | **no map objects at all on Arma 2 worlds under GL33** (census: 0-1 map models within 150 m of the player; Utes' LHD missing, C0's crew on the sea) | the streamed-object residency update ran only on WGPU; it now runs on every renderer |
| `d427d85`, `b7e1654` | with map objects present, every tree and bush drew as an opaque card | Arma ODOL section hints lack OFP's alpha bits: taken from the texture, and for streamed objects (converted on a worker, no texture headers) from the `*_ca.paa` naming rule. Utes T01 now shows its pines, houses and road |
| `3dfb515` | TE06_Parachute crash in `DrawWater`; C6B_DogsOfWar 60 s in `AILocker::LockPosition` | sectionless water segment skipped; lock range bounded |

| `541fdb1` | the player's rifle lay at his feet (third person) | the corepatch M16A4 is stored around its binariser's bounding centre (0.98 m up, 0.48 m along), the vanilla AK-74 the AI carried is not. Proxies now place a model's source origin (`AnimationOrigin()`), for proxy objects in ShapeAdapter and for held items in `Man::DrawProxies`. Measured: the player holds his M16A4 in MP_Superpowers, MP_UrbanWarfare, C2B |

| `da8a367` | each soldier stood ~0.5 m beside his own origin | the man block now shifts x and z by the binariser's centre as well as lifting y; offline the footstep points come out symmetric (x +-0.18) and the aiming axis on the origin |
| `131eb6a` | **every Arma 2 soldier faced backwards** (aimed behind himself, rifle and hands out of the first-person view, hands seen from behind in third person) | the A1x man class inherited CWA's `reversed=1`; an Arma 2 man is stored facing +z (posed offline: knees and hands go to +z). The class now says `reversed=0` and BMTR moves are conjugated once so the engine's reversed load returns them as posed. Measured: first person shows hands and rifle (MP_Superpowers, UrbanWarfare, C2B), third person shows the soldiers' faces toward a camera in front of them |
| `58afdbb` | C0's crew on the water | streamed objects arrive over the first frames; at the 8 s screenshot the census finds all nine LHD parts and the player at y 15.9 on the deck (tops 16.1) |

Still open: in first person the hands are untextured-looking and the fingers spread; the
rifle sits low in the view.

### After run 17 (laptop, gl33), 2026-10-14

| Commit | Finding | Fix |
|--------|---------|-----|
| `601bf9f` | `121cc8c` left animals out by resolving them to no class, but `ArcadeUnitInfo::Serialize` took the empty class for a missing addon and stood in a CWA `Civilian` (run 15: 139 units, the census's only CWA stand-ins; run 17: 77 in the first 29 missions) | the unit gets presence 0 and `presenceCondition` false, so it is not created |
| `9600cb1` | T05_FixedWing_VTOL_Training: a yellow waypoint bracket on the player's own F-35 reading "%s %s %s im Kampf (%s) gefallen (0 m)" | the 3D waypoint marker ignored `showWP` (the mission says NEVER, as most Arma 2 missions do; the map already honoured it), and its label indexed the waypoint-type strings by type, so a type outside MOVE..SUPPORT read the string before them (`STR_STAT_UNIT_LOST`). Under the compat layer the marker follows `showWP`; any unnamed type reads Move |
| `5253d47` | the HMMWV M1151 M2 had no gunner and no gun (census `weapons=CarHorn`): every Arma 2 car resolved by name to CWA's unarmed jeep | a car whose Arma 2 `Turrets` mount a weapon (not a horn or smoke launcher) is carried by `JeepMG`. The M1151 keeps OFP's turret selections (`otocvez`/`otochlaven` in LOD 0, `osaveze`/`osahlavne`/`usti hlavne`/`konec hlavne`/`pos gunner` in memory) |
| `ebe75b4`, `fac52fd` | first person broken on all three soldier models looked at: USMC (`usmc_soldier_CO`) shows spread pink fingers, FR (`Cooper`) and ACR (`acr_soldier`) a piece of gear filling the bottom of the screen and no rifle; Takistan's chain-link fence draws as a white wall | diagnostics: `OP_A2_PROXYLOG=1` logs the view-pilot LOD's posed bones against LOD 0 under the same palette, what lies in front of the eye and the section textures; the census lists the map models nearest the player first, with the textures and flags of the nearest dozen |

Offline on Arma 2's own files (`diag29`): the view-pilot LOD (res 1100) of `USMC_Soldier_CO`
carries the same skin weights as LOD 0 (6780 of 7210 vertices matched by position name the
same dominant bone, the other 430 sit on a joint between neighbouring finger bones), so the
skin data is not what breaks first person. The view LOD has no head and no helmet; its
hands use `hhl_06_co.paa` with `hhl.rvmat` (`PixelShaderID Skin`), the same material the
third-person hands use.

Focus run 7 (`5253d47`, 6 missions, all entered and clean; census 607 entities with the
Arma model, **0 CWA stand-ins** -- the animals are gone; HMMWV M1151 M2 and Land Rover
Special now `weapons=Browning`; T05's stray label gone): in game the view LOD posed exactly
like LOD 0 (`OP_A2_PROXYLOG`: bones within 0-4 cm). What was wrong was the pose itself:

| Commit | Finding | Fix |
|--------|---------|-----|
| `dee0e27` | A stored Arma man faces **-z** like every Arma/OFP model: toes, eyes and the `pilot` camera point lie at -z. The BMTR moves were read mirrored in x: every joint closes (stretch 0.09-0.21) but the man becomes his own mirror image, limbs to +z and face to -z. `131eb6a` then left him unturned so the rifle pointed forward -- and the face backward: the first-person camera looked out of the back of his head (FR/ACR: their backpack filled the screen; USMC: his arms seen from the wrong side), and in third person the camera behind the player saw his face and knee pads | the moves pose the TURNED model, as the engine draws it: read without the x-mirror and conjugated once by diag(-1,1,-1) for the engine's reversed load, with the man turned again (CWA's `reversed=1`). `diag30` (the repo's own `ConvertBMTRToRTM` on USMC_Soldier_Light, USMC_Soldier_CO and FR Cooper; stand, aim, idle, walk, kneel, prone): stretch halves to 0.04-0.09, and eyes, hands, knees and toes land on the man's own front. `OP_A2_MAN_XMIRROR=1` restores the old pair |
| `a7f3a32` | the "white fence" is Arma 2's concrete wall (`Wall_IndCnc_4_D`, `_End_2`, `_End_3`): one section with no face texture, only a `Multi` rvmat blending four layers by a mask; GL33 drew it white (census: section texture `(none)`) | a texture-less section takes its material's stage 0 as its texture when that is a base colour (`*_co`/`*_ca`); the wall draws with `concrete_wall_co.paa` |

Measured in game with `a7f3a32` (focus run 8, 8 missions, all clean, census 763 Arma / 0
CWA): first person shows the rifle in both hands at the shoulder (MP_Superpowers'
M16A4 with ACOG, C2B's M4 with EOTech), the eye sits 17 cm in front of the neck (was 2 cm
behind it), the FR and ACR soldiers no longer see their own backpack, and in third person
every soldier, the player and the AI, turns his back to a camera behind him with the rifle
pointing ahead. SPE1's concrete walls draw as concrete.

Still open: the first-person hands look flat (skin texture with the `Skin` shader, which
GL33 draws as plain diffuse).

### Weapons, magazines and colours, 2026-10-15

| Commit | Finding | Fix |
|--------|---------|-----|
| `341a466` | the LHD deck crew in C0 all wore yellow ("gold all over"): Arma 2 colours one model per class through `hiddenSelections` + `hiddenSelectionsTextures`, which the import dropped | the A1x class carries the pairs whose texture exists (an OFP hidden selection without a texture is hidden, an Arma 2 one is not); an entity starts with those textures. Measured: red, green, white and yellow crew in C0 |
| `6d42036` | the CDF/TK machine gunners carried CWA's PK: a weapon name both games know was returned unchanged | a hand weapon both games name alike (PK, Strela, Stinger, hunting rifle) gets the Arma 2 model on the CWA weapon of that name |
| `31a0892` | `Stinger`/`Strela` name no CWA class (CWA: `AALauncher`, `9K32Launcher`), so AA soldiers had no launcher; launcher rounds (PG7VL, OG7, SMAW, AT13) found no magazine | AA, ATGM and RPG/SMAW/LAW launchers and their rounds map to the CWA launcher of their kind |
| `e6bee9d` | RPK, DMR, Bizon, Makarov SD rounds found no magazine: OFP writes `magazines[]={"this"}` for a weapon that is its own magazine, and "this" names no class | "this" means the CWA weapon's own name; 9x39 goes to the SVD, .22/.17 HMR to the hunting rifle |
| `3c30bb1` | the M107 (on CWA's M21) had eight M21 magazines and an empty gun: inherited, `{"this"}` named the A1x class | an A1x weapon on such a weapon names the CWA weapon as its magazine |
| `ddc557d` | the face rig reserved its 8 bones on the full 128-bone Arma 2 skeleton before finding no room: 136-bone palettes for every Arma 2 man, 961 warnings per run | no room, no bones |
| `7c1b32e`, `f012ca6`, `f77d01a`, `31df49c` | -- | census lists each soldier's magazines; a third screenshot through the weapon's optics, with the optics model logged; a scoped Arma 2 weapon takes its own 2D scope (`modelOptics`) and zoom -- not yet seen in game: the aiming view did not engage in the unattended test (being logged) |

Aiming: in run 19 the test switched the player to the aiming view and the world reported
it (`CameraType` 1 = CamGunner, optics model `data3d\optika_m16.p3d` for MP_Superpowers'
M16A4, `optika_ak47.p3d` for MP_CivilWar's AK), but every `_optic.png` showed the plain
first-person view: GL33 writes a requested screenshot at the end of a later frame, and the
test switched back to first person in the same tick (`14e4478` waits two seconds). Arma 2's
own sights (`modelOptics="-"`: the camera at the weapon model's eye point) are not
modelled yet.

| Commit | Finding | Fix |
|--------|---------|-----|
| `767f07d` | first-person hands a flat salmon by day and glowing orange at night (MP_Paradrop, run 19) | Arma's `Skin` shader (hands, faces: `hhl.rvmat`) reads `emmisive` and `forcedDiffuse` as subsurface parameters (`{1,0.6,0.05,0.6}`, `{2.8,0.22,0.12,0.22}`), which `TexMaterial` took as colours; a Skin material keeps neither |
| `4562178` | Saiga 12K and M1014 slugs/pellets had no CWA magazine (the last unmapped rounds in run 19 besides laser batteries) | shotguns and their rounds go to CWA's hunting rifle |

### Focus run 10 (build `e783482`), 2026-10-15

7 missions, all entered, census 608 Arma, 0 CWA. The Skin fix holds (MP_Superpowers'
hands natural by day, MP_Paradrop's dark at night), TE02's Takistan bushes draw leaves
(`_can`), and the aiming view now engages: MP_Superpowers and C2B look over CWA's M16
iron-sight overlay. Findings, fixed after the run:

| Commit | Finding | Fix |
|--------|---------|-----|
| `8e1af97` | the M107's own 2D scope (`\ca\weapons\2dscope_mildot_14`, copied by `31df49c`) drew as a small black box with an ellipse in a screen of flat red-brown, no scene | Arma 2's 2D scopes are authored for its own screen mapping; the weapon keeps the CWA weapon's scope (M21) until that is handled. `OP_A2_SCOPES=1` takes them anyway |
| `949ee53` | under GL33 the HMMWV came out a striped light grey with no camouflage, the M1A1 and MTVR slate blue, the deck crew glossy gold (C2B, T03, C0) | Super's `specular[]` is the ceiling the SMDI green channel scales per texel; GL33 lights per vertex and has no SMDI, so it applied 0.69 everywhere -- the HMMWV's SMDI averages 8/255. `TexMaterial` records the SpecularDetail map and GL33 scales the specular by its TAGG AVGC green (or a procedural `color()`'s g). WGPU binds the map and is unchanged; `OP_GL33_SPEC_MEAN=0` for A/B |
| `fa764b4` | an Arma 2 rifle aimed through CWA's 2D sight: TE02's SCAR with its EOTech showed the M16 rear-sight mask | Arma 2 sights are part of the weapon model (`modelOptics="-"`, Rifle/Pistol/Launcher bases): the import writes `optics3D=1` and Arma 2's `opticsZoomMin/Max/Init`, `distanceZoom*`; the aiming view then draws the soldier as in first person and puts the camera at the weapon model's `eye` memory point along the barrel, with the weapon placed exactly as the first-person LOD draws it, near plane 0.07 m. A model without `eye` keeps the CWA overlay. `OP_A2_SIGHTS=0` turns it off |
| `56ba7d3` | the C-130J (on CWA's UH-60) carried ZuniLauncher38, the fishing boat (on the armed BoatW) a Browning (run 19 census) | an Arma 2 vehicle with no real weapon on itself or any turret (horn, smoke and flare launchers aside) gets `weapons[]={}`, `magazines[]={}` when its CWA class is armed; the seats stay |
| `33b7181` | why the 2D scope was a small box: an Arma 2 `2Dscope_*` frames its 4:3 scope image with black triangles out to +-20 x +-15, and `Object::Draw2D` fitted every vertex (and skipped triangles with a `Fail` per frame) | Draw2D fits the frame's inner edge to the screen height, square, sides blacked out; OFP optics (all quads) unchanged. Measured inner edges: +-3.94 x 2.95 (MilDot), +-5.5 x 4.12 (ACOG), +-4.52 x 3.39 (Hunter), +-6.56 x 4.92 (M1 gunner), all 4:3. Arma 2 scopes are on again by default (`OP_A2_SCOPES=0` for CWA's) |

Measured for `949ee53`: `hmmwv_body_co.paa` (A2 `wheeled.pbo`) averages RGB 81/78/67, a
dull woodland; `hmmwv_body_1_smdi.paa` (OA `wheeled_e.pbo`, Stage5 of `hmmwv_body_2.rvmat`)
TAGG AVGC `7f08ffff` (green 8), `hmmwv_body_1_as.paa` green 130. The HMMWV in C2B is OA's
`ca\wheeled_e\hmmwv\hmmwv.p3d` with Arma 2's woodland texture.

**Correction (focus run 11):** `949ee53` did not change a single pixel of the HMMWV
(C2B third-person crop identical to run 10, mean 67.3/61.9/54.1 both times). The grey and
the stripes are the GL33 shadow map, measured by A/B on the same build (`33b7181`):

| Run | Change | HMMWV / MTVR |
|-----|--------|--------------|
| 11b | `WGR_IGNORE_LODNOSHADOW=1` (cast from the drawn LOD) | still striped |
| 11c | `triShadowSetDarkness 1` (no shadow darkening) | woodland camouflage, no stripe |
| 11d | `triShadowSetBias 0.0002` (10x depth bias) | still striped, slightly fewer |

The depth pass stores back faces (front-face cull), so a surface turned from the sun is
its own stored depth and compares against itself: fine stripes, half shadowed. `faad391`
gives the GL33 receiver a normal offset (1.5 texels of the tier,
`POSEIDON_GL33_SHADOW_NORMAL_OFFSET`); the 18 vertex/pixel shader pairs compile and link
on Mesa llvmpipe. `949ee53` stays (it matches what WGPU does for an unmapped specular) but
fixed nothing visible.

### Focus run 11 (build `33b7181`), 2026-10-15

8 missions (C2B, TE02, MP_CivilWar, MP_Superpowers, T03, C0, SPE1_OneShotOneKill,
SPE1_Vehicles_US), all entered, **all 8 without a script error**, census 693 Arma, 0 CWA.

- **3D sights work.** TE02's SCAR looks through its EOTech (reticle in the window),
  MP_CivilWar's AK-74 through its own front-sight hood, MP_Superpowers' M16A4 through its
  ACOG -- logged `3D sights yes`, camera at the weapon's `eye`.
- C2B's M4A1 HWS GL still aimed over CWA's M16 mask (`3D sights no`): it rides CWA's
  two-muzzle `M16GrenadeLauncher`, whose inherited muzzle classes never saw `optics3D`.
  Fixed `7fe3564` (the first muzzle takes the weapon's sights and zoom).
- **The M107's Arma 2 scope draws right:** the round mil-dot scope fills the screen height,
  the sides are black (SPE1_OneShotOneKill opens at dawn, so the zoomed view is red sky).
- C-130J and the fishing boat carry no weapons any more (`56ba7d3`).

Offline check: a small program built from the repo's own ODOL and RTM readers poses
`USMC_Soldier_Light` with a move and scores the result by how much the mesh's edges stretch.

## Steps

1. **Measure first.** Per mission: an asset census in the log (every class created, what it
   became, which model, whether the model loaded with LODs and textures) and screenshots of
   the player in first and third person and of the nearest vehicle. Run on the laptop
   (`Downloads\a2test`, see below) over all 98 missions.
2. **Soldiers.** Find the first-frame crash with `OP_A2_SOLDIERS=1` (symbolised stack:
   the CI artifact now carries the PDBs). Then check the Arma 2 skeleton
   (`CfgSkeletons`, `OFP2_ManSkeleton`) against the bones the engine names (head, hands,
   weapon, launcher), moves (BMTR) for stand/crouch/prone/walk/run, entering and leaving
   vehicles, death. When a bootcamp and a campaign mission play cleanly, make it the default.
3. **Weapons.** Generate `A1x_` weapon and magazine classes like the vehicle ones: Arma 2
   model and proxy in the hand, optics (`modelOptics`), muzzle flash, sounds, magazines
   with the Arma 2 ammunition values (`hit`, `typicalSpeed`, `caliber`) on a CWA base of the
   same kind, so firing, reloading and damage work.
4. **Vehicles.** Wheels, tracks, rotors, turrets and doors animate on the Arma 2 model's
   own selections; crew sit in the model's proxies; lights; the vehicle's own values
   (`maxSpeed`, `armor`, `fuelCapacity`, `transportSoldier`) instead of the stand-in's.
5. **Aircraft.** Placed `FLY` starts with engine on and flight speed (the A-10 in
   MPE1_Dogfighters glides down with the engine off), Arma 2 flight values, helicopter
   cargo and doors.
6. **Objects.** Composite map objects placed by their model origin (Utes' LHD is drawn
   in pieces), destruction (`destrType`, ruins).
7. **Play it.** Per mission the start scene with the right people, weapons and vehicles,
   no script errors, and a few minutes played by hand on the laptop.

## Test setup on the laptop

The laptop has no toolchain; builds come from the `Arma 2 / Windows game build`
workflow as a zip (exe, `wgpu_renderer.dll`, PDBs) in `Downloads`.
`Downloads\a2test\go.cmd` (started from Notepad++, F5) reads `job.txt`:

```
DEPLOY=1                # install the newest OpenPoseidon-Windows-*.zip from Downloads
LIST=missions_all.txt   # one mission per line (the 98 of Arma 2 + Operation Arrowhead)
OUT=tests_run11         # one log per mission + exitcodes.txt
SECONDS=40              # --arma2-test-seconds
RENDER=gl33             # WGPU on Vulkan loses the device on this Intel GPU
OP_A2_SOLDIERS=1        # any OP_* / WGPU_* key is passed as an environment variable
```

`stop.cmd` ends a run after the current mission. The deploy writes `DEPLOYED-FROM.txt`
from the zip's `BUILT-FROM.txt` and gives exe and DLL the same time.
