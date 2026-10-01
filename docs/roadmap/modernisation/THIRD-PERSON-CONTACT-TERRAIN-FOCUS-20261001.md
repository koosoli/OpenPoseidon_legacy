# Third-person contact terrain focus, 1 October 2026

The installed Everon standard US walking campaign on `e5445111` produced real
sand depression approximately 9.37 cm and wet mud compression 5.64 cm with
the player body visible. Root reported weak relief in the third-person
pictures. Those physical samples do not establish visible geometry quality.

## Concrete source cause and scope

`TerrainWgpu::Draw` refines at most an eight-metre square around the actual
render camera, only when it is within 15 m of terrain and the existing snow,
mud or sand conditions apply. `Man::OutsideCameraDistance` returns 10 m.
`World` derives the external camera from the actual camera-attached entity;
the rendered third-person position and the soldier's feet can therefore lie
in different refinement patches. A real signed field still has its 12.5 cm
cells, but coarse terrain vertices can fail to sample the boot-sized hole.

`GWorld->CameraOn()` is the controlled or spectated owner in normal view, not
the offset render camera. Freefly/cutscene effects deliberately leave that
owner on the player; querying CameraOn without excluding CameraEffect would
focus unrelated terrain beneath the player while inspecting a different scene.

This candidate keeps the existing camera focus and adds **at most one**
eight-metre target focus, only with:

- normal external/group view and no CameraEffect;
- the actual CameraOn object classified as a living Man, not a vehicle;
- no Brain vehicle attachment, finite world coordinates and current terrain
  height within three metres of the owner position;
- actual three-dimensional camera/owner distance at most 20 m;
- actual stored negative mud/sand offset, or detailed active snow deficit,
  greater than 1 mm in a bounded two-metre boot neighbourhood.

The neighbourhood uses at most 81 quarter-metre points, stops on the first
actual local mark, and queries only existing fields. It does not stamp,
advance, allocate or reset field state. Current **local stored relief** is
the proof; no actor-specific producer timestamp is invented. A stopped or
paused soldier standing on persistent marks keeps their geometry visible.
Standing on another actor's real marks also qualifies. A recently moving
soldier on untouched hard ground, a distant spectator, a raised floor,
inside view or freefly does not create a second focus.

The 20 m cap plus radius eight leaves at least two metres inside the existing
camera-centred 64 m signed-height view after its two-metre chunk snapping.
The initial proposal of 24 m would touch that texture boundary and was
reduced. This changes neither snapshot origins nor the source-field lifetime.

## Geometry and existing authority

`RefineCdlodContactUnion` tests the union during **one recursive partition** of
each originally selected CDLOD parent. It does not submit overlapping
camera/actor overlays. At most two foci and radius at most eight are accepted;
invalid foci are ignored. The children retain the parent's level and material
ownership and the existing no-coarse-morph band for fine contact samples.
The minimum patch is unchanged at four metres, giving 12.5 cm spacing for
the existing 32-wide grid. Single-focus output is exactly the old helper's
ordered origin/size/level/morph tuples. Duplicate/overlapping foci do not
duplicate geometry.

Parent selection, native/legacy terrain, material/source classification,
frustum/visible rectangle, fog, extended border, cull displacement allowance
and all field admission/physics are unchanged. Refinement only partitions
the parents those existing rules selected. Union leaf count is bounded by
the sum of the two independent old single-focus counts minus one. No global
distance/detail increase, new geometry displacement or body rendering rule
is introduced.

The narrow source query runs at today's main-thread Landscape draw boundary,
the same thread that captures/consumes presentation snapshots. The existing
interactor snapshot has position/size/rotor facts but no Man/external/effect
classification. Root approved this bounded query rather than enlarging the
private snapshot in this slice. When simulation/render overlap is introduced,
the owner classification and contact-focus value must move to the world
snapshot producer before this path can run independently of simulation.
No public ABI, infantry, shader, source publication or physics code changed.

## Checks and outstanding runtime proof

`scripts/Test-TerrainContactFocus.ps1` compiles the actual production pure
header using clang++ C++20 with warnings as errors. Its source-only CPU run
passed single-focus exact equivalence (including outside/edge windows),
duplicate identity, overlapping and separated third-person-like foci,
partition area/parent bounds/non-overlap, 12.5 cm sampling within both foci,
sum-of-single geometry cap, focus-count cap, invalid radii/NaN rejection,
empty focus passthrough, finite/distance/height/view/body/contact gates and
200 actual snapped-upload margin cases. `git diff --check` passed.

Root still needs the combined game build and installed ordinary Everon
third-person sand/mud repetition with the same actual contact/source samples,
near low and top views, body visibility, paused camera return and normal exit.
The important positive is feet outside the camera's eight-metre window while
inside the new 20 m owner cap, with actual local signed cells and resolved
mesh relief. First-person, freefly, no-local-contact and distant/non-Man
negative controls should retain the existing selection. Geometry tests do
not accept material readability, shadows, grass occlusion or appearance.
No game, GPU, full build or deployment ran in this agent.
