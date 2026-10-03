# Stock uniform lighting comparison

`scripts/Test-OriginalOFPWeatherPlayerRuntime.ps1 -UniformLightingViews` adds
controlled lighting views to the existing installed-game, standard `SoldierWB` test.
The default run and its manual sand/mud contact gates are unchanged. Supply the
same required `-ExpectedCommit` and run through the existing game lock. The root
agent owns the installed-game run; this source-only change does not establish
visual acceptance.

For each original map, the optional run captures these seven views for the dry
and naturally rain-wet bands:

- A close, low front-quarter daytime view.
- Midnight with a real stock `StreetLampWood` switched off, on, and off again.
- Daytime with the stock roof absent, present, and removed again.

Both bands use the same actual controlled actor, world position, staged heading, lamp
position/model, and raw engine camera vector. The normal zero-duration overcast
command applies cloud lighting 1 while paused for both visual bands and restores
the original cloud lighting before simulation resumes. It needs no rain or
simulation-time change (`World::SetWeather`, `Weather::SetOvercast`). Rain wets
the soldier only through the existing real rain simulation. Neither rain history
nor Person wetness is overridden. Rain intensity differs between bands; ordinary idle animation
may differ after elapsed wetting time. These are lighting comparisons, not
claims of exact bone-pose or pixel equality.

The camera is derived from the actual settled dry `getDir`, anchored once for
both bands. Its horizontal offset is 2.5 metres along the actor's actual forward
direction and 1.5 metres along its right direction; raw camera coordinates remain
engine X/Y/Z while `getPosASL` supplies X/Z/Y. The wet band cannot re-anchor to a
changed heading. Once before each band, while already paused, ordinary
`player setDir` stages world orientation to the anchored actual dry heading.
The record separates the naturally observed heading before staging, requested
heading, immediate actual readback and staged state. `ObjSetDir` writes only
`SetOrient`; no animation, bone pose, position, rain history or Person wetness is
forced. Actual clock, position, physical stores/weather and the complete
read-only mud/sand source/geometry samples at the soldier must remain identical
across staging. Every paused capture and the final return must retain the staged
heading within .01 degrees, using circular compass distance near 0/360.

The initial 20261003-124512-40f8db installed capture failed before any optional
lighting image. Its saved harness requested `setDir 90` during placement, then
allowed 1.037 seconds of ordinary simulation; the actual paused `getDir` was
52.0466. `ObjSetDir` calls `SetOrient` only. Normal soldier turn/input/animation
simulation can subsequently rotate it, so the earlier request cannot substitute
for the settled heading. The fixture now uses that actual value for camera
geometry while keeping strict paused heading-retention gates. This
source repair does not retroactively accept the failed capture; a new installed
run and image inspection are required.

The subsequent e017 / b2a357 installed run produced all seven dry views with
actual lamp and roof guards, then failed the original dry/wet natural heading
equality: the soldier turned from about 52.0463 to 346.873 degrees during 91.12
simulation seconds of real rain. Position remained unchanged; actual Person
wetness was .705556 and GMud wetness .80306. That natural turn is recorded rather
than treated as an aligned wet capture. The paused orientation stage supplies
a concrete controlled comparison while preserving genuine wetting history.
It does not assert that normal simulation naturally retains orientation or
that dry/wet animation poses match. A fresh installed run must verify the staged
readback, state/source retention and wet images.

Every paused view records the actual clock, Person wetness, physical stores,
weather, class, body queue/draw/primary section evidence, lamp identity/position,
camera-effect state, `dayTime`, and `triGetShadowSunFactor`. The latter reads
`1 - MainLight()->NightEffect()`; `setDate` recalculates the normal engine sun.
Midnight and daylight controls must pass their corresponding actual sun gates.
No forced night effect, synthetic lamp flag, lamp tuning, or custom shader is
used. The on/off transition must add and remove exactly one scene light in the
existing `LAMP-001` receipt. The object query records its real stock shape name.
Object debug names use the registered `format ["%1",object]` operator.
`StrFormat` calls the object value's `GetText`, which returns its actual
the virtual `GetDebugName`; the controlled actor's EntityAI override supplies
its unit identity, such as `WEST Alpha Schwarz:1 (mail)`, rather than a model
filename. Its nonempty/non-null identity and dry/wet equality are required.
The player model is proven separately by actual stock class and body source rows.
Static lamp/roof shape-name checks keep their separate Object debug-name route.
The 20261003-131800-b7caf7 retry failed because this engine does not register
the assumed `str` function. That failure aborted script evaluation and required
owned-process forced cleanup; it provides no lighting acceptance. All three
optional player/lamp/roof queries now use the source-supported format operator.

The roof uses `TerrainPuddleStockRoof.ps1` and its audited `CampEastC` /
`stan_eastC.p3d` identity. RoofLift 3 suspends its solid collision hull above the
standing soldier while retaining the central floor-free visual region. Actual
ASL placement, captured master class/model, and null master after removal are
checked. The paused absent/present/removed sequence preserves real cloth wetness
and actor/ground state; it proves paused retention and supplies lighting images.
It does not prove shelter prevents additional wetting during live rain.

The fixture removes its own lamp and roof, releases the render-only camera,
restores daytime, and verifies that paused actor wetness and physical state have
not changed before returning to the existing ordinary movement tests. Helper and
script hashes are included in the result. Appearance remains pending review of
the actual dry/wet PNGs, including masked body pixels under the stock night lamp.

Source verification: `-SelfTest` passes existing tuple, real manual-contact,
current body, and encoded cloth falsifiers plus new camera coordinate, actual
day/night, live-lamp receipt, and stock shape-name falsifiers. No game, GPU,
profile, deployment, production shader, or executable is changed by that check.
Heading-derived cameras are checked at the captured 52.0466 degrees and all four
cardinal headings. Changed, nonfinite and out-of-range headings are rejected.
The actual rain-wait drift is also a heading falsifier; source-sample changes
across orientation staging are rejected by the pure helpers.
