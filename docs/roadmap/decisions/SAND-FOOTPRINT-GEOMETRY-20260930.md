# Persistent sand boot geometry: source contract

This change prepares a separate mission-local SandField and actual animation
boot-contact producer. It does not yet establish installed sand deformation:
root owns the simulation lifecycle, physical terrain height/normal integration,
GPU buffer publication and CDLOD refinement. A colour or normal decal alone
does not satisfy this acceptance.

## Audited material admission

Read-only inspection of the previously extracted, decoded retail Bin/Config.bin
and AddOns/Noe.pbo config gives these exact identities:

| Surface class | files | soundEnviron | inherited character |
| --- | --- | --- | --- |
| Sand | ps?????? | sand | empty |
| SandAbel | pi?????? | sand | empty |
| SandDark (Noe) | pt?????? | sand | empty |

StockSandSurface accepts only those complete tuples. It does not infer loose
sand from colour, a filename substring, sand sound alone, a renamed class or a
Default fallback. SandBuilding/pisek01* is a separate identity and is excluded.
An altered mod retaining the same metadata remains admitted; this is authored
semantic matching, not content-hash enforcement.

The actual centre must qualify before the bounded support scan, cheaply rejecting
non-sand footsteps. Nine support probes at centre and +/-0.6m must stay inside
the map and source tile UV .12-.88. This 60cm guard exceeds the capsule extent
41.5cm plus the full 12.5cm interpolation support. Each distinct tile is checked
once, and **all four actual cached quadrants** must qualify. Unknown and mixed
tiles, any Enfusion surface table and authored A3 terrain materials fail closed.
The pure helper is available to root's renderer source-mask publication; native
coverage should be extended only with real per-point source masks.

All nine support probes additionally reject submerged terrain (ground <=
sea+2cm), snow cover above 2mm, road objects and support above terrain+5cm.
The centre rejects nonfinite or steep support gradient (>0.7) and boot height
disagreement above25cm. A roof alone does not
exclude dry sand; a raised floor does. No collision or gameplay friction is
changed by this producer. The current sand wetness history is global cosmetic
rain accumulation, not a spatial shelter/shore saturation simulation.

## Geometry and integration contract

SandField stores signed cell-centre metres at 12.5cm, in sparse 16x16 chunks.
The cap is 8192 chunks (8MiB raw cell data plus container overhead). At capacity,
new cells are rejected without evicting old tracks; existing cells remain
editable. Repeated animation edges do not dig additively. An overlap takes the
deepest depression or highest positive rim, with a depression always winning
over a rim regardless of contact order.

StampBoot uses the actual foot memory point and actor direction. The capsule
toe/heel segment is +/-7.5cm, depression radius 20cm and rim outer radius 34cm.
The finite radial profiles flatten smoothly at centre and outer edge. Defaults
are a 10cm loose dry depression and 1.4cm rim; actual liquid rain gradually
makes **new** impressions shallower (5.5cm) and rims lower (0.8cm). Hard caps
are -15cm/+2cm. This models loose compaction with partial grain displacement,
not conserved sand volume, a granular solver or sinking progressively every
animation cycle. Existing footprints survive rain/drying and camera movement.

Root must call GSand().Reset() on world/mission reset and
GSand().Advance(actualSimSeconds, UniformWettingDensity(actualWeatherRain,
effectiveParticleDensity, particleSnowflakes)) once per simulation update.
Zero/nonfinite/negative elapsed time is inert; render/getter calls cannot wet
or deform the field. The analytic wetting/drying rates are 30/180 seconds, and
dry sand remains contact-active without rain. No save/network fields are added.

Root physical terrain height must add HeightOffsetAt(x,z), and terrain support
derivatives must add HeightGradientAt(x,z) exactly once. Both use the same
cell-centre bilinear evaluator; derivatives are the exact derivative of that
surface, not a separately smoothed normal. Normal slopes are finite and bounded
by sqrt(2)*(MaxDepth+MaxRim)/CellSize (<1.924); the maximum is a conservative
cell bound, not a typical footprint slope. Do not clamp gradients independently
of geometry. Standing support/IK should evaluate the deformed terrain.

Snapshot(cameraX,cameraZ) returns exactly 262148 floats:
`[originX,originZ,.125,.15]`, followed by 512x512 **signed** offsets, row-major at
world cell centres. Disabled/invalid snapshots set header.w=0 and contain no
deformation. Positive rims must survive both upload and shader sampling; the
mud negative-only clamp must not be reused. Revision changes only for geometry
edits/reset, not rain, queries or snapshot creation. Upload caching must also
track window origin and enabled state. Camera movement never erases chunks.
Root plans to append this as the third view of the existing snow/mud storage:
absolute buffer sand header starts at float index524296; sand cells at524300;
total buffer786444 floats. The shader's snow.deficits begins after the first
four-float snow header, so **deficits-relative** indices are header524292,
cells524296 and deficits length786440. These indices follow two preceding
262148-float views and must be checked against root's actual layout before
publication.

## Actual Noe fixture candidate

Read the cached original Noe RVW4 export from the earlier retail source audit:
256x256 cells at 50m, checked palette7 is `o\pt.paa` (4964 assigned cells).
Noe's pt?????? SandDark entry resolves its uniform tile to four SandDark
quadrants, including the optional zero-length trailing six-question-mark rule.
Original Noe WRP SHA256:
`9b27b11661d3cb30f9f0749a9a4616bfc58d723c4750666d1e674dd4213598d2`.
RVW4 export SHA256:
`dbe1f29e300bb60c0b34c5f6467a842857147bed82495ef84f390a0871016a18`.

Candidate X2675/Z5125 is source cell53,102, with all four coarse height corners
14.985m. The neighbouring X2675/Z5075 and X2625/Z5125 cells are also uniform
pt and flat at14.985m. These are coarse source candidates, not runtime contact,
subdivision, roof/road or object-clearance proof. Query actual installed
SurfaceY before actor placement. A 6m northward walk centred at2675/5125 remains
inside the .12-.88 source interior including the 60cm guard; allowed centre
range is X2666.6-2683.4, Z5116.6-5133.4. No actor should be spawned from coarse
height alone. Keep exact source admission, actual walking and SAND_STEP trace,
negative height/gradient change, positive rim and camera-return/pause equality
as separate installed gates. Road/grass/source-border and snow are required
negative controls. Synthetic direct stamps are not actual boot acceptance.

## Verification

Focused CPU compilation uses C++20, -Wall -Wextra -Werror, the production
SandField.cpp and tests/unit/engine/Poseidon/World/Weather/test_sand_field.cpp.
Checks cover dry contact, analytic wetting and split-step agreement, invalid
inputs, orientation, negative depth/positive rim bounds, independently evaluated
packed height/gradient parity, finite-difference derivatives, no repeated digging,
order-independent overlaps, positive/negative coordinates, camera persistence,
disable/reset/pause behaviour, capacity/no eviction and exact source negatives.
These passed; no full build, game, GPU or deployment was performed by this agent.
Installed physical/visual acceptance remains pending root integration.

## Installed real-walking runner prepared

scripts/Test-SandFootprints.ps1 reuses the installed mud fixture's proven TCP,
private-profile, health-check, provenance and normal-exit structure. Only an
owned fresh process may run through scripts/with-game-lock.sh; an existing
OpenPoseidon/ColdWarAssault process blocks launch. DEPLOYED-FROM.txt is the first
installed file read. EXE/DLL hashes, sizes, timestamps and stamp must remain
unchanged through exit. Both stdout/stderr are retained. All changed/cleared
environment values are recorded and restored, with previously absent values
removed rather than assigned null (PowerShell7.6/.NET10 semantics).

The minimal perf_sand.noe mission puts its player at2686/5125, away from the
sampled lane. A fresh separate west group creates an ordinary SoldierWB leader
at2675/5125, verified through actual type/leader/ASL/pose/heading queries. It uses
normal doMove toward2675/5131 at LIMITED speed, and pauses after at least4m of
measured movement. There is no unsupported enableAI, reused subordinate,
synthetic sand stamp, direct deposit or wetness setter. Stationary placement and
the later paused worker relocation are explicitly excluded from walking proof.

The runner removes inherited weather/snow fixture overrides, sets actual rain,
fog and cloud cover to0, particle mode off/density0/snowflakes false, snow
disabled and snowline off. Actual weather_visibility authority must remain dry
through every state query. It starts time100/date1985-06-21 hour16 before actor
settling, uses brightness/exposure1, zero wind, temporal/jitter/adaptive exposure
off, grass/puddles off, native1280x720/MSAA4 and fps cap60. Captures are paused.

The actual centre must report noe/noe.wrp, o/pt.paa, SandDark/pt??????/sand,
empty character and all-quadrant sourceEligible. The fixed3445-point grid
samples X2673.53125-2680.03125 and Z5124.03125-5132.03125 every12.5cm, offset
3.125cm from cell centres to include nonzero derivatives. Every point must
remain source-admitted. The baseline must contain no signed field edits.
Every identical-coordinate before/after query must show physical SurfaceY
change matching signed sand offset within0.00006m, and both physical derivative
changes matching sand derivatives within0.0001. Acceptance requires at least
one depression >=3cm and one positive rim >1mm, with physical gradient change
on both. A normal decal or positive shader tint cannot pass these CPU physical
support gates. Actual SAND_STEP rows from the correct dry stock lane are also
mandatory. The actor's ASL support gap must stay within3cm at each recorded state.

The observed actor path must stay within X2674-2679.4 and Z5125-5131.6; at
every position poll, the four corners of its +/-60cm support rectangle must
still report all-quadrant sourceEligible. The pre-walk fixed grid is never
shifted around a result. This admits ordinary diagonal AI motion inside the
same audited pure-sand tile rather than requiring a synthetic straight line.

After the measured walk and geometry proof, the paused fixture moves the worker
to2686/5125, outside the views, then records a low view1m above actual ground,
a top view4m above ground, an away view and the same low view on return. The
after views centre on the measured start/end midpoint; their exact camera poses
are recorded. Before/after adaptive framing is descriptive, not pixel-isolated
appearance proof. Time, actor pose/heading/
ASL, uniform wetness, rain authority, sand history and every signed grid sample
must remain exactly unchanged through pause/camera return. Every PNG must be
1280x720 with hash, camera and accompanying state retained. Normal exit0 and
Shutdown complete are mandatory; any forced cleanup retains failed status.

scripts/Test-SandFootprintHelpers.ps1 parses the actual runner AST and extracts
its production assertion functions without installed-file/process/network/GPU
access. Its104 checks pass under bundled PowerShell7.6.5, covering signed limits,
height/gradient parity for depressions **and positive rims**, dry authority,
malformed numeric/trace/source data, minimum depth/rim evidence, pause changes,
source-coordinate consistency, locale-independent trace parsing and pair changes.
This is runner/helper preparation only: no game/GPU was run. Positive installed
physical geometry, GPU appearance and normal/standing support remain pending
root execution. Wet cohesion and hard road/grass/source-border/snow installed
negative controls are explicitly not covered by this first dry runner.

### First installed attempt retained as a failed fixture

Root's matching06a7c41e installed run
build/sand-footprints/noe-dry-current-20260930-212526-1e02a7 exited normally0,
with no reported validation error. The ordinary AI walker moved from2675/5125
to2678.44/5128.62, with end support gap-0.00745m. Five actual dry SAND_STEP rows
reported10cm depth/1.4cm rim and6 chunks/revision7. That proves the producer ran,
not the required physical depression/rim or installed appearance gates: the
fixture stopped before the after-grid because the original +/-60cm X lane was
too narrow for the actual diagonal AI path. That campaign remains failed.

The corrected baseline grid covers the bounded diagonal region before any
movement. Actual distance4m, dry authority, exact source, depth>=3cm, positive
rim>1mm, support/gradient equality, pause/camera return and normal exit/hash
requirements are unchanged. The expanded grid and adaptive cameras have only
CPU/helper/source verification so far; root must execute the corrected fixture.

### Corrected installed dry control

Matching06a7c41e `build/sand-footprints/noe-dry-gridfixed-20260930-213903-a0074c`
passes the installed physical/lifecycle gates. The stock actor walks4.336m on
actual Nogova SandDark (`o\\pt.paa`) with dry liquid-rain authority. The3445-point
before/after grid records a minimum signed support change of-0.081341m and
maximum rim+0.010383m, with21 samples deeper than3cm and87 positive rim samples.
Physical terrain heights and derivatives agree with the signed field. Actual
boot producer traces are retained. Paused simulation, sand/rain/uniform state
and all3445 physical samples survive the camera return exactly. Normal exit0,
Shutdown complete and unchanged matching installed provenance/hashes pass.

Root inspected after-low/after-top and the baseline: visible depressions and
soft raised shoulders beneath the existing boot-detail decals read as compacted
sand. Baseline and after cameras differ because the ordinary AI takes a diagonal
path, so this is descriptive visual acceptance combined with numeric geometry
proof, not a pixel-isolated before/after material measurement. Wet cohesion,
hard-ground/border/snow installed negatives, other maps and gameplay friction
remain unaccepted. This is persistent signed terrain compaction, not a granular
flow simulation.
