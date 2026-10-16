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

### Focus run 12 (build `faad391`), 2026-10-15

6 missions (C2B, TE02, T03, T06, SP_TrialByFire, MP_Superpowers). C2B's M4A1 HWS GL now
looks through its EOTech (`7fe3564` holds). The 1.5-texel normal offset lightened the
HMMWV and brought part of its camouflage back, **but the stripes stayed**: a side turned
from the sun is its own stored back face, and at a grazing angle one texel spans more
depth than an offset covers. `e0bbaac` adds the rule that settles it -- for a receiver
turned from the light a stored surface up to ~1 m behind it shadows it
(`POSEIDON_GL33_SHADOW_SELF=0` turns it off). T06's view through the same M4 came out
entirely black; `964ec29` logs where the 3D-sight camera goes (in T06 the player stands
half a metre behind another soldier, whose back may simply fill the view) and no longer
builds the camera from a zero muzzle direction.

Full run 20 (`33b7181`) was stopped after 21 missions for this build: 21 entered, 20 clean
(MP_Paradrop's own error), census 2357 Arma, 0 CWA. It continues on `faad391` as run 20b.

Offline check: a small program built from the repo's own ODOL and RTM readers poses
`USMC_Soldier_Light` with a move and scores the result by how much the mesh's edges stretch.

Run 20b (`faad391`) played the remaining 48: 48 entered, 48 clean, census 5663 Arma, 0 CWA.
Runs 20 + 20b together: 69 of 98 missions, 68 clean, 8020 Arma, 0 CWA.

### Focus runs 13 and 14 (builds `d508f9e`, `4646571`), 2026-10-15

Focus 13 (`d508f9e`, with the shadow rule `e0bbaac`): 6 of 6 clean, census 452 Arma, 0 CWA.
The census now lists the mission's own objects within 25 m of the player with each section's
texture and flags; it showed the camouflage nets of T07 and C2B drawn without their holes
(black where the net is open). Cause: an Arma 2 ODOL section keeps its alpha in the raw
`commonFaceFlags` (0x100 alpha, 0x200 transparent, 0x20000 alpha-ordered), and the
conversion masked those bits off. `4646571` keeps them.

Focus 14 (`4646571`): T07, C2B, TE02, T03, C0, SP_TrialByFire -- 6 of 6 clean, census 600
Arma, 0 CWA. T07's camouflage net is see-through; C2B's and TE02's HMMWV show their
woodland/desert camouflage with no stripes; C2B's M4 and TE02's SCAR look through the EOTech.
C0 wrote no picture through the sights (the sheet shows the gap as black): the test's
screenshot steps run in the in-game display's update, and after the third-person shot at
14 s it logged nothing more, so something covered that display -- not checked further.

### Sounds, music and conversations: inventory, 2026-10-15

Read-only scan of exactly the archives the engine mounts (`AddOns`, `Expansion\AddOns`,
`Missions`, `MPMissions` of Arma 2 and the Arrowhead folders; OA's `Common\` is not mounted,
so its stripped `missions.pbo` does not hide Arma 2's). 92 archives, 143 mission folders.

| Command in mission scripts | Uses | Missions |
|---|---|---|
| `kbTell` (conversation lines) | 2501 | 50 |
| `fadeMusic` / `fadeSound` | 94 / 76 | 35 / 36 |
| `setIdentity` | 91 | 25 |
| `playMusic` | 55 | 35 |
| `playSound` / `say` / `say2D` | 34 / 32 / 14 | 10 / 8 / 2 |
| `sideRadio` / `groupRadio` | 8 / 3 | 2 / 1 |
| `createSoundSource` | 2 | 2 (C4_BitterChill, MPE_SectorControl) -- **not implemented** |
| `say3D`, `playSound3D` | 0 | -- |

Trigger and waypoint effects add music tracks (`track`, 12 missions), sound effects
(`soundDet`, 9) and environment sounds (`soundEnv`, 2).

What resolves, measured against the data:

- Every name a mission uses resolves to a class in its `description.ext` (with its
  `#include`s), the campaign's or the merged config, except 12: `playMusic "longdark"` (C0's
  `onPlayerKilled.sqf`), `"EP1_Track_03/05/08"` (SPE1_Vehicles_*), `say "expl"/"door"` (C2,
  C3), `say2D "pain06"` (CE0), `groupRadio "BIS_EW_sentcheering"` (STR_EW2), `say "hi"` (C3),
  and three the scan should not have counted (`say "plastic"` and
  `playSound "placeholderShhh"` are commented out, one is chat text). No class of these
  names exists in the scanned configs or descriptions, so the original plays nothing there
  either (not checked in the original game).
- **Conversations:** 4800 recorded lines are named in the missions' `.bikb` files, 4611 exist
  (189 missing in the data, e.g. T05 names T04's recordings).
- **Conversation timing was wrong.** `kbWasSaid` reported a line said after a time guessed
  from its text, and the conversation FSMs start the next line on it. Of 4495 recorded lines,
  2032 run more than 1 s past the guess (438 by more than 5 s; C3's radio news 66 s against
  2.3 s), 945 end more than 1 s before it. `9e46264` uses the recording's own length;
  each `kbTell` is logged with which of the two it used.
- **Config sounds the CWA path rule missed.** `GetSoundName` puts `sound\` before a relative
  name and `.wss` after one without extension. All 55 `CfgRadio` entries (45 of them
  SP_Counterattack's radio dialogue), 31 `CfgSounds`, 47 `CfgEnvSounds` and 2 `CfgSFX`
  entries found no file: their configs name the archive path without the leading backslash,
  or say `.wss` where the Steam data holds `.ogg`. `fb88b7c` looks such a name up again as
  the archive path and with `.ogg` -- only under `--arma2`, only when nothing was found. That
  the original reads these files the same way is a conclusion from the data (it ships only
  the `.ogg`), not checked against the original game.
- `CfgMusic`: all 67 entries resolve.

Not checked yet: whether music and sounds start at the right moment in game, volume and
distance of `say`, the radio protocol (Arma 2's `CfgVoice` words; CWA's own radio voices
speak), and `createSoundSource`.

**Campaign flow (from the code, not run):** `--arma2-mission` with a campaign mission is
meant to start the campaign at that mission (`StartArma1CampaignMission`, WorldImpl.cpp), but
the campaign name is cut from the path as if it began with `campaigns/` (Arma 1). For Arma 2's
`ca/missions/campaign/` that gives `s/campaign`, so the mission plays on its own: no
campaign `description.ext`, no next mission after the end. The test harness never takes this
path (test runs skip the briefing and play the mission alone).

### Focus run 15 (build `fb88b7c`), 2026-10-15

C0, SP_Counterattack, T01, CE0, C1, C3, 90 s each, with `OP_A2_EVAL` playing a Counterattack
radio line and an Eagle Wing sound every 80 s: 6 of 6 clean, census 838 Arma, 0 CWA, no
"not found in any CfgSounds". The log shows both sound rules at work
(`Eag_cdfrun.wss` read as `.ogg`, `credits_boom_1.wss` as its archive path). C1's opening
radio conversation now runs at the pace of its recordings: each next line starts 0.0-0.6 s
after the one before ends (O_0 1.8 s -> M_0 2.3 s later, M_1 6.1 s -> M_2 6.7 s later). CE0
told four lines of one speaker in the same millisecond -- see below.

### Faces, identities, conversations, procedural textures, 2026-10-15

- **Identities never applied.** `setIdentity` looked only in the mission's and the campaign's
  `description.ext`; Arma 2 keeps Cooper, Miles, Sykes, O'Hara, Rodriguez, Shaftoe, Armstrong
  and Zemanek in `ca\missions\config.bin` (`CfgIdentities`), and its missions call
  `setIdentity` 91 times in 25 missions. `fc9f077` looks there too. An Arma 2 voice
  (`Male01EN_A2`) is not in CWA's `CfgVoice`: the man keeps his own voice instead of CWA's
  first one (deviation: no Arma 2 voices).
- **Faces.** A man drawn with his Arma 2 model wore a CWA face texture (CWA's `CfgFaces`) on
  the Arma 2 head's `osobnost` section (from the code; the face screenshots of focus 16 show
  it). Arma 2: `CfgFaces >> <class faceType> >> <face>` -- 12 faceTypes (Man with 765 faces,
  CooperHead, MilesHead, SykesHead, OharaHead, RodriguezHead, HeadMask1Black, WomanHead, ...),
  and a man without an identity gets a random face matching his class's `identityTypes`
  (`Head_USMC`, `Head_RU`, `Cooper`, ...). `fc9f077` does both; `OP_A2_FACES=0` restores CWA's
  faces. Not done: glasses (Arma 2's `CfgGlasses` are models), face wounds, face materials
  beyond the head's own.
- **Conversations.** `kbAddTopic`'s third and fourth arguments were dropped: the FSM an AI
  runs when it hears a line of the topic, and the player's event handler (his conversation
  menu). 1135 of the missions' 1297 `kbAddTopic` calls pass both, 158 the FSM alone; the
  conversation editor's FSMs make the listeners reply through them. Those FSMs also tell
  several lines of one speaker in one state, which Arma 2 says in turn. `ce961ea`: a
  speaker's lines queue, and the listener reacts once a line has been said (an AI by the FSM,
  the player by the handler). There is no conversation menu: choices the player makes in
  Arma 2 are still not offered (deviation).
- **Procedural textures** (`#(argb,8,8,3)color(...)`) named in configs got `data\` and
  `.paa` added (run 21: MPE_SectorControl's UI). `9872760` passes them through.

### Heads: Arma 2 soldiers had none, 2026-10-15

Focus 16 (`d15a573`, 8 missions, 8 clean, census 658 Arma, 0 CWA) added face pictures from
0.9 m in front of the player and the man nearest him: **no head at all** -- cap or helmet,
headset and collar, nothing between. Run 19's pictures (`f77d01a`) show the same, so this is
old, not from the face work. Offline (`diag35`/`diag36`/`diag37`, the repo's ODOL reader):

- `ca\characters2\blufor\soldier.p3d` LOD 0 has no head: its `hhl` skin section holds only
  the hands. FR Rodriguez's body is the same.
- Both carry a proxy `\ca\characters\heads\bysta`, bound to **no bone**, at the model's
  source origin. `Man::DrawProxies` only draws proxies on a bone (weapons, NV goggles), so
  it was never drawn -- and `bysta.p3d` is only a placeholder bust anyway.
- Arma 2 draws the head model of the man's face there: `CfgFaces >> faceType >> face >>
  head` -> `CfgHeads >> <head> >> model` (`DefaultHead`, `Cooper`, ...). Those models are
  skinned to the same 132-bone skeleton (head, neck, jaw, lips, brows, eyes ...) and modelled
  in the body's source coordinates (head-bone vertices centre at y 0.717 in DefaultHead,
  0.672 in the soldier's helmet).

`e01cf5c` loads that head model with the face and draws it moved by the man's head bone
(face texture on its `osobnost` section). Focus 17 (`4bf48c7`) showed the heads -- turned
round: the face looked out of the back of the head. The body is a reversed model (turned at
load); `b5727b2` loads the head the same way. Deviation: the head moves as one piece -- no
grimaces, no lip sync (the face's own bones are not posed), the neck follows the head.

### WGPU on the laptop (Intel Iris Xe), 2026-10-15

`WGPU_BACKEND=dx12` (`d15a573`, T01, C2B, TE02): the adapter is found (Intel Iris Xe, Dx12).
T01: renderer creation panicked ("failed printing to stderr: Zugriff verweigert (os error
5)") and the game refused the automatic GL33 fallback (exit 1). C2B and TE02: after the
renderer's start-up log nothing more, the test watchdog ended them after 60 s without a
simulation step (exit 3) -- it counted the loading. `4bf48c7` gives a test 180 s to load
before the first step. Still open: whether the DX12 start-up finishes at all, and the stderr
panic (149 `eprintln!` in the renderer; Rust panics when stderr cannot be written).

### Faces and heads measured (focus 18, 19), 2026-10-15

Focus 18 (`b5727b2`): the heads face forward. Face pictures of the 8 missions, and of T01
alone after its run there ended early (rerun: entered, clean -- not reproduced):

- **Custom heads draw right.** Cooper (`Cooper.p3d`, his own `cooper_co.paa`) and O'Hara
  have eyes, brows and skin detail. Offline (`diag38`, the repo's ODOL reader): Arma 2's
  `Cooper.p3d` has no `osobnost` selection, so the face texture `CfgFaces >> CooperHead`
  names (`hhl_27_co`) never replaces his own -- the same as in Arma 2's data.
- **Default heads were blurred, pale and eyeless** (T01's soldiers, TE02's player:
  `defaulthead.p3d` with a random `hhl_NN_co`). An Arma 2 ODOL selection names its
  sections and keeps no face list; `Shape::RegisterTexture(texture, selection)` returned on
  the empty list, so the face texture was never registered with an area and stayed at its
  smallest mip -- the violet of the texture's unused background bled into the face.
  `3e4dc87` takes the replaced texture from the selection's first section. (This was the
  "violet tint" noted after focus 18.)
- **Polygonal light and dark patches on faces are the GL33 shadow map**, measured on the
  same build: with `triShadowSetDarkness 1` (focus 19) they are gone on every face; with
  only the self-shadow rule off (`POSEIDON_GL33_SHADOW_SELF=0`, `e0bbaac`) O'Hara's face is
  clean and Cooper's, bent forward under his cap, keeps them. The shadow is cast from the
  body's low-polygon shadow LOD onto the detailed head. Not changed -- part of the open GL33
  shadow decision.
- Not done: the face `material` of `CfgFaces` (camouflage paint, the black faces' own
  rvmat) -- the head's own material is used; glasses; wounds; the face's own bones (no
  lip sync, no expressions).

`createSoundSource` (`3e4dc87`, link fix `5422672`): of the scanned mission archives only
C4_BitterChill's `houseBurn.sqf` calls it (`"Sound_Fire"`); it was an unknown command. It
now creates the class's CfgSFX sound source where a mission.sqm sound source would be
placed. Not run yet.

WGPU on the laptop, second try (`b5727b2`, DX12): T01 and TE02 lost the renderer to the same
stderr panic -- std's `eprintln!` panics when the write fails, and the game is a GUI program
whose stderr a launcher may leave unwritable. `5422672` makes the renderer's `eprintln!` and
`println!` drop a failed write. C2B created the renderer after 44 s (DX12, Iris Xe) and drew
no frame for two minutes before the watchdog; the stall report printed garbage as the last
expression (a pointer the evaluator had freed; `6e89b47` keeps a copy).

Focus 20 (`6e89b47`, 5 missions, all clean, census 614 Arma, 0 CWA): the default heads draw
sharp (skin detail, brows, stubble; the violet tint is gone). `createSoundSource` made the
`fire` source in all five missions through `OP_A2_EVAL` (whether it is heard: not checked).
The face's `CfgFaces` material was checked against the data: of the enabled faces only 8
(the black faces, `hhl_black.rvmat`) name a material of their own -- not used yet.

WGPU, third try (`6e89b47`): DX12 now creates the renderer in all three missions (~60 s) and
then never reaches a simulation step (watchdog after 180 s). Vulkan on the same laptop
created it in 60 s, entered T01 and played it to the end -- at a few frames per second, with
no screenshots written. The stall report now also takes stacks at 90 and 170 s.

### Decisions (owner, 2026-10-16)

- **C-130J stays carried by the UH-60** (helicopter stand-in, deviation kept).
- **GL33 shadows stay global** (normal offset and the self-shadow rule for all content), and
  **heads are not exempted from self-shadowing**: the patches on faces are accepted as they are.

### Run 22 follow-ups, 2026-10-16

- **Campaign flow, played mode** (`27737ef`, C0 without `--arma2-test-seconds`): the log names
  the mission `'C0_FirstToFight.Utes' of campaign 'ca_missions_campaign'`, and the campaign's
  intro scene ran (`c0introshaftoe`). The screen itself could not be seen from here (the game
  window is outside what the test session may capture); the end of the mission and the next
  one are still unchecked.
- **WGPU DX12** (`27737ef`, T01): the stacks at 90 and 170 s are all `<unknown>` frames --
  the main thread is inside code without symbols (the GPU driver) for minutes after the
  renderer was created. Not an Arma 2 matter; left there.
- **"Closed eyes" on default heads:** offline (`diag37`), the upper-lid vertices of
  `defaulthead.p3d` sit at the eye centre's height (0.693 vs 0.694), as in Cooper's head
  (0.673 vs 0.677), whose eyes look open in the pictures. The rest pose is not closed; the
  pictures look down on the faces from above. Nothing changed.
- **Face materials:** the only enabled faces with a material of their own (`hhl_black.rvmat`)
  use `BlackHead`, whose model already carries that material. Nothing missing.
- **Glasses** (`CfgGlasses`): 4 identities wear them (Miles: BlackSun, Maddox: Sunglasses,
  two in the armory) and Arma 2 would also give random ones by weighted `identityTypes`
  (`USMC_Glasses`: Tactical 100, Sunglasses 30, Spectacles 20, None 825). The glasses
  models carry no bones and no proxy marks their place on the head, so where Arma 2 puts
  them could not be established from the data. Not done.
- **MP_Paradrop's script error is the mission's own:** `spawnunits.sqf` passes
  `_targetmarkerdir`, which it never assigns (it sets `_Markerdir`).
- **Backpacks** (`574ee4a`): 40 Arrowhead soldier classes carry one by config
  (`TK_RPG_Backpack_EP1`, `US_Patrol_Pack_Specops_EP1`, ...); the body only has an unbound
  `us_bag` placeholder, so none was drawn. The pack models are skinned to the skeleton
  (spine..spine3) in the body's source coordinates; they are now posed with the man's bone
  matrices. Not yet measured.

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
8. **Sound.** Conversations paced by their recordings (`9e46264`) and config sounds found
   (`fb88b7c`) -- confirm both in game logs; then `createSoundSource` (C4, MPE_SectorControl),
   lip sync from the `.lip` files next to the recordings, and music/sound timing over a
   whole mission.
9. **Campaign flow.** Start an Arma 2 campaign mission inside its campaign
   (`StartArma1CampaignMission` cuts the name as if it were Arma 1's `campaigns/<name>/`),
   so the campaign `description.ext` applies and the next mission follows the end.

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
