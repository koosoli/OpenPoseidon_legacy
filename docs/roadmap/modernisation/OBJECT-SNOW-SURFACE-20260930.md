# Raised object snow: source slice, 2026-09-30

Source branch `codex/object-snow-20260930`, base `c9024bb4`, implementation
`423e3974`. Parent session owns integration, compilation, serial installed-game
captures, authoritative ledger and deployment. This document records source
decisions; it does not accept an appearance or certify a loaded roof.

## Deposit, surface height and shelter

`Terrain::set_snow` retains the actual finite deposit from an accepted mission
snow header, independently of the ground deficit grid. That grid contains both
tracks and CPU roof shelter: applying it at a raised roof's X/Z would wrongly
erase snow on the roof itself. Object shading never reads that grid.

The internal camera UBO appends `snow_surface = (deposit, snowline height,
snowline ramp range, snowline depth)`. Surface altitude is the actual absolute
fragment Y, rather than `Landscape::SurfaceY` or the object origin. The getter
rejects nonfinite parameters and disables malformed/negative/zero-range
snowlines. Accepted deposit zero resets object coverage as the existing mission
snapshot resets terrain snow; no wall clock or separate accumulation is added.

Upward coverage uses the shared filtered powder helpers. Authored outward
normal/view sidedness rejects a roof's visible underside before any powder
normal is applied. Vertical walls, degenerately authored normals, and sources
without explicit receiver authority remain uncoated. Opaque and A2C normal
prepasses use the same coverage/powder normal helper as colour; partial cover
retains the existing prepass limitation that original tangent-space normal maps
are colour-only. Full powder coverage has matching powder normals.

Nearby admission uses the existing zenith depth layer, independently of artistic
AO strength/floor. `interior_rain_coverage` separately proves map validity.
Disabled/out-of-box maps fail closed for unproven surfaces. In the outer 5%
map band, the shader removes the lighting helper's open fallback before mixing
known far exposure; the open fallback must not paint a moving white edge on a
sheltered floor.

Retained models have existing sky-volume metadata and complete voxel storage.
Only an actually present full volume can supply a far fallback, using its raw
visibility, with `smoothstep(0.98, 0.995, raw_visibility)`. Artist strength/floor
never enter this test. Side-window diffuse reach around 0.72 is rejected. This
is a conservative diffuse approximation, not a new directional bake or proof
for every small canopy opening. Direct draws lack those model-volume bindings,
so unproven direct surfaces outside the near map remain dry. Coarse-volume
surface interpolation and missing/dropped/pending volumes may leave far roofs
dry; movement/return captures are still needed before declaring stable coverage.

`WGR_INTERIOR_SKY=0` removes the active near depth maps through the existing
`prepare_interior_sky` gate. It does not itself disable default-on model baking
(`WGR_SKY_BAKE_VOLUMES`), so a retained model with complete raw baked metadata
can retain conservative far admission. Direct surfaces and models without such
data fail closed. This is distinct from strength zero or floor one, which leave
physical maps available. No all-settings acceptance is claimed. A later change
could maintain a dedicated physical zenith map while ambient AO is disabled;
this slice does not silently re-enable ambient shading or five extra depth views.

## Explicit source eligibility

Direct receiver bit `WGR_DRAW3D_SNOW_RECEIVER=2048` is set from the actual
`GetDrawObject` owner, rather than assuming `NO_PALETTE` means scenery.
The mesh must be one of that owner's LODs, preventing a proxy from inheriting
the enclosing object's authority. Intact Primary scenery or TypeVehicle
Thing/Vehicle owners are allowed; Person, temporary/network objects, dynamic,
skinned, CPU-posed, destroyed/animated, local/first-person, blended/cutout,
vegetation and conformed/OnSurface geometry are excluded. Material shader gates
also exclude glass, reflective water, monitors, emitters and nonzero emission.

Retained bit `WGR_INSTANCE_SNOW_RECEIVER=128` requires the actual static intact
rigid owner, no terrain conform and no animation-capable source. Child proxies
and authored far DTOs do not inherit it. Retained vegetation, cutouts,
reflective/emitting materials and zero normals are additionally rejected in
the shader. Vehicle surfaces marked dynamic/animated therefore remain an
explicit unsupported case; the installed UAZ control must be interpreted from
source-admission telemetry rather than assuming every vehicle roof is admitted.

The new coat replaces underlying albedo, normal and material gloss/cavity under
coverage. It suppresses authored direct/local highlights and uses a small rough
powder sky-specular response, with shared footprint filtering. It adds neither
geometry thickness nor displacement/collision and is HDR-only.

Private streaming-page gates now allow only the added benign snow bit alongside
their existing exact view-authority bits. Page replacements copy the original
receiver bit; transaction and stable-frame validation require it to match,
including restore/rollback. All other formerly unsupported flags remain
unsupported. Page models still need their own bake metadata for far fallback;
no global shelter authority is transferred with this bit.

## Layout, diagnostics and verification

The public camera/draw/instance byte layouts and FFI exports are unchanged.
The appended camera lane is renderer-private, and all eight tail writes
(GI, origin, material debug, water, relief, fog, render scale, snow) moved
together. Both new eligibility meanings occupy unused public flag bits.
An old producer leaves receivers dry; an old DLL ignores the new bits and lacks
the coat. Matching installed EXE/DLL provenance remains necessary even though
the unchanged ABI handshake cannot distinguish those semantic versions.

`WGR_OBJECT_SNOW=0` disables only effective object coating, including snowline,
without changing terrain or the saved profile. `WGR_SNOW_SURFACE_FIXTURE=1`
enables bounded game-log evidence:

```text
wgpu object snow: enabled=1 deposit=0.1800 snowlineHeight=-1.00 snowlineRange=0.00 snowlineDepth=0.0000
Wgpu object snow receiver: model=...stan_eastc... path=direct|retained eligible=true|false static=... type=... scope=source-owner-material-and-facing-gates-remain
```

Parameter logs are emitted before camera preparation, on initial/change rows,
capped at 32. Receiver logs identify actual loaded CampEastC/UAZ model paths,
capped at 32 unique source/path/admission rows. These are transmission/source
evidence, not pixel acceptance.

Authored checks (parent must execute after shared `snow_material` integration):

- `gfx3d::object_snow_tests`: actual-helper composition and GPU directional-depth
  falsifier for open roof, covered upward surface despite side-window light,
  ineligible/underside/back-side, unknown/outside/edge map, known complete bake,
  artistic strength zero/floor one, and exact disabled normal preservation.
- `object_snow_uses_actual_deposit_and_finite_surface_altitude_params`: actual
  getter policy for deposit, disabled/malformed snowline and bounded line depth.
- Existing normal-relief lane test now pins all eight tail offsets and lane order.
- C++ `[object-snow]` receipt tests: takeover/update/restore and stable returned
  frames retain matching snow proof; unknown/vegetation/far-authoring flags and
  changing the proof are rejected.
- Existing shader composition/native preview combinations and streaming receipt
  checks must remain passing, followed by a matched installed dry/coat-on/off
  roof capture and covered-floor control. Vehicle top is exploratory unless
  source eligibility and positive pixel controls both pass.

Agent executed only new-test-file `rustfmt` and clean `git diff --check`; no
compilation, GPU test, game launch, deploy or push was run in this worktree.

## Direct animated-owner current-section proof

Follow-up based on `52e6dc09`: the previous blanket `!isDynamic` exclusion
mistook a whole-LOD allocation hint for proof that every roof vertex deforms.
`ShapeDraw.cpp` chooses `VBDynamic` from `GetAllowAnimation` (also for the tent
damage property); `VertexBufferWgpu::Update` hashes/reuploads actual current
vertices. `Car::Animate` changes selected wheel, steering, turret and configured
animation vertices, so unrelated roof vertices can retain their original pose.

The direct draw now accepts a dynamic section run only after all previous
source-owner, foreign-proxy, person, skin/palette, CPU-pose, destroyed mesh,
cockpit, vegetation, conform, blend/cutout and reflective/glass/monitor gates
pass. Every position and normal component within its conservative section
vertex union must be finite and exactly equal to saved originals. Saved normal
count is independently validated by the new `OriginalNormValid` accessor;
absent or incomplete originals fail closed. Unordered section ranges are
unioned rather than assuming index order implies vertex order. Unused vertices
inside the union can cause a conservative false negative.

The proof allocates nothing and examines at most 16,384 vertices and section
records per run. It is skipped with `WGR_OBJECT_SNOW=0`, with no actual enabled
deposit, and when the object bounding sphere cannot reach an enabled finite
positive-depth snowline. The sphere uses the actual owner scale; it is only a
cheap possible-cover bound. Fragment coverage remains the authority at actual
surface height. Static direct draws retain their previous admission; retained
animated-owner registration stays excluded by its existing rigid source gate.

This is a current-pose proof, not a declaration that a selection can never
animate. A neutral articulated section may pass and becomes excluded if its
positions or normals change; no persistent coating follows an articulated
selection. UV-only changes are not geometry deformation. A section sharing a
vertex span with animated wheels can remain dry. No displacement, collision,
model ownership, material or streaming authority is changed.

`scripts/Test-ObjectSnowSurfacePolicy.ps1` passed in the isolated worktree:
unchanged roof versus moved wheel, independently changed position/normal,
nonfinite source data, missing originals, invalid bounds, read-free over-budget
rejection, the exact budget edge, disabled/zero deposit and finite snowline
height/depth/range admission. `git diff --check` passed. No game, GPU, full
engine build, deployment or push was run. Parent must build the matched pair
and verify source-admitted stock UAZ roof dry/coat-on/master-off controls,
without treating a passing CPU proof as pixel acceptance.

## Keep-only roof admission correction

Read-only inspection of the exact stock `stan_eastc.p3d` fixture found 396
first-LOD vertices with flags 2111 and 74 with flags 210765887: every vertex
contains `ClipLandKeep` (0x800), none contains `ClipLandOn` (0x100). Root's
matched `e0bb256a` Eden fixture recorded the tent's direct and retained receiver
as false even after actual deposit reached 0.1800. UAZ direct true/false rows
were present, ruling out a blanket vehicle `GetAnimated` exclusion; that method
only tests destroyed-phase deformation, unlike `Car::IsAnimated`.

The zero-conform source/shader gates excluded the intact tent's raised roof.
Mode 2 Keep vertices use `SurfaceY + undeformedWorldY - bcSurfaceY` and keep
their height above ground; mode 2 On vertices are pinned to SurfaceY. Keep is
a per-vertex vertical shear over terrain, not necessarily a uniform rigid
translation. Existing inverse-transpose normal handling and final world-space
roof height remain authoritative; snow does not change that conform behavior.

CPU admission now permits mode 0 or proven mode 2 Keep-only source meshes.
Direct admission checks the drawn mesh hints. Retained admission checks every
normal visual LOD, fails closed on missing source levels, and preserves the
existing static/intact/no-authored-animation requirements. Any On hint rejects
mode 2, including a mixed Keep/On source; mode 1 forest planes and unknown modes
remain excluded. The retained vertex shader accepts only mode 0 or mode 2
rigid/Keep selectors with the explicit CPU receiver bit. Its colour and both
normal prepasses use the same resulting receiver flag. Direct Keep vertices
already leave `conform_w` zero; its existing pinned-On rejection remains correct
for all three material consumers. Existing vegetation, proxy, glass,
translucency/cutout, emissive, cockpit and ON_SURFACE exclusions remain.

The standalone CPU policy runner passed the additional rigid/Keep-only,
mixed/pinned, missing-Keep, forest, unknown and nonfinite mode cases, as well as
all earlier current-pose tests. Source assertions pin the actual Keep position
equations, direct pinned weight exclusion and retained selector gate. Parent
must run shader composition/renderer tests and repeat matched roof dry/coat-on/
master-off pixel controls. No game, GPU, full build or deployment was run by
this agent. The real tent coverage result remains pending that installed test.

## Remaining stock tent gate diagnostic

Root's matched `2ac545ef` Keep campaign still reports no stock tent receiver
and visibly bare fabric under actual deposit 0.1800. The Keep correction alone
does not establish successful roof coverage. The follow-up adds only opt-in
source diagnostics under `WGR_SNOW_SURFACE_FIXTURE=1`, scoped to actual loaded
`stan_eastc` and UAZ model identities; production admission policy is unchanged.

`Wgpu object snow direct gates:` reports section range and actual texture,
source eligibility, rigid proof attempted/pass, buffer dynamic/skin/CPU-pose,
pose-copy, palette/pass kind, owner/mesh ownership and destroyed-animation
gate, blend/depth/alpha reference, vegetation/material flags, conform mode and
Keep/On source hints, original position/normal validity and source/buffer
vertex/normal counts, with actual enabled deposit. `rigidAttempted=false` means
an earlier source gate prevented scanning; it must not be interpreted as a
failed vertex comparison. Static-buffer `rigidPassed=true` is existing policy,
not a newly executed original-data proof. Zero and positive deposit are separate
dedup states; at most 128 unique rows per matching model can be emitted.

The direct `denied` bitmask is: owner 1, foreign mesh 2, destroyed animation 4,
skin 8, CPU posed 16, pose copy 32, palette 64, nonstandard pass 128, blend 256,
depth 512, alpha reference 1024, conform 2048, vegetation 4096, prohibited
material flags 8192, attempted-but-failed rigid proof 16384. Actual individual
values remain printed so the reason can be checked independently.

`Wgpu object snow retained gates:` reports static/owner/destroyed/allow-animation
eligibility, conform result, visual/missing LOD counts and aggregate Keep/On
hints. It is capped at 64 unique rows. These registration values may precede
deposit activation; retained admission is independent of deposit, which is
uploaded through its separate effective snow parameter telemetry.

The standalone CPU policy runner and `git diff --check` passed. Root owns full
compilation and the next focused deposit-only installed run; this agent ran no
game, GPU, full build or deployment and claims no roof appearance acceptance.

## Retained current visual pose admission

Root's matched `6a388c62` focused capture identified the remaining CPU gate:
the tent's retained instance is static, owner-eligible, Keep-only across all four
loaded visual LODs and conform-allowed, but `GetAllowAnimation` is true. Its
opaque roof belongs to retained draws. The direct complement contains rope and
chimney soot with actual transparent blend/pass/material flags and is correctly
excluded. A blanket allocation/animation hint cannot establish roof deformation.

The retained flag now checks each loaded visual source for actual
`DeformsSharedShape` and `GetAnimated`. MAYANIMATE models additionally require
finite exact current position/normal equality to saved originals across all
visual LODs, with a shared 16,384-vertex budget, complete original streams and
no allocation. Normal non-animated static sources retain their zero-vertex-scan
fast path. Missing visual source, deformed/destroyed source and budget excess
fail closed. This is evaluated at existing instance registration/update, not
in every colour draw. Updates recompute the receiver bit.

Because `Object::DeformsSharedShape` currently recognizes destruction rather
than all configured animations, neutral configured Entity geometry animations
are excluded too. The new read-only `HasConfiguredGeometryAnimations` accessor
uses the actual Entity animation array, with no field/layout change. Built-in
Church/Fountain procedural animation owners remain excluded until a reliable
retained pose update authority exists. They lack independent legacy casting
IDs, so their exact exclusion uses existing supported C++ RTTI, not `dyn_cast`
with inherited Building IDs. Actual Camp Building->VehicleSupply ownership is
retained; no Thing-only assumption is introduced.

Standalone CPU tests passed multi-LOD unchanged versus deformed source,
missing visual/original data, actual deformation/animation gates, neutral
configured-animation rejection without vertex reads, unchanged no-animation
zero-scan behavior, one shared budget and empty/nonvisual source rejection.
Earlier source/section/snow controls still pass; `git diff --check` passed.
Root must build and check the retained tent receiver and physical roof pixels;
no runtime or appearance acceptance is claimed by this source-only agent.
