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
