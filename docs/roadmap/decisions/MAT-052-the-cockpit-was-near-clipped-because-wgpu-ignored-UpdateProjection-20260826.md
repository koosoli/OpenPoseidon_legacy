# MAT-052 — the cockpit was NEAR-CLIPPED, because wgpu ignored UpdateProjection

- **Ticket:** MAT-052 (geometry half FIXED; shading half remains open)
- **Date:** 2026-08-26 (second record; supersedes the six-leads-die record's
  open verdict, which see for the doors and the eliminated theories)
- **Status:** the "transparent cockpit" structural defect is fixed. A separate
  over-bright interior shading defect remains, now cleanly isolated.

## The mechanism

`Transport::Draw` brackets every interior (zSpace) draw with
`cam->SetClipRange(0.01, 50); GEngine->UpdateProjection(); GEngine->Clear(true,...)`
(TransportCore.cpp:1905) — the canopy frame hugs the pilot's head, well inside
the world near plane, and this is the 2001 engine's way of drawing it anyway.

`Engine::UpdateProjection` is a virtual with a NO-OP default. GL33 overrides
it (EngineGL33_Mesh.cpp:26). **EngineWgpu never did** — the cockpit's
projection never reached the GPU, and every polygon nearer than the world
near plane (~0.096 m _43, but effectively the beams at grazing incidence)
was clipped away: canopy beams, pillars, roof, door frames, on every
helicopter of every generation, base game included. The dashboard (~0.5 m)
and the canopy glass survive, which is what made this read as "the cockpit
is transparent" rather than "geometry is missing": through the missing beams
you see glass tint over sky, terrain, and the volumetric cloud deck.

Fix: `EngineWgpu::UpdateProjection() { PushSceneCamera(); }` — a new camera
entry with the current projection, which is exactly what the multi-camera
plumbing already does for HUD and weapon views; the zSpace epilogue's second
UpdateProjection pushes the restored world projection for anything after.
Measured (stock UH-60, pilottest, 20 s capture): dashboard complete, right
door pillar solid riveted metal, near cockpit geometry all present.
CAMPUSH probe during diagnosis: camera 1 `_43=-0.010` (the 0.01 near) vs
camera 0 `_43=-0.0957` — the bracket fires twice per frame, correctly.

## How it was found — and the two instruments that mattered

`git bisect run` over 594 commits with an automated pixel oracle (build +
20 s capture + judge the roof-beam pixel), good endpoint = the owner's public
OpenPoseidon repo head `34c44ac5`, first-bad = `aa24c32b` — which then turned
out to be misleading in the details (its own diff contains nothing that
touches the cockpit path directly; the era changed the near plane / camera
handling enough to expose the missing override).

The decisive instruments, both kept in-tree:

- **`WGR_KILL_TEX=<substrings>`** (EngineWgpu::DrawSectionTL) — DROP matching
  draws. Whatever vanishes is what that path drew; a pixel that does NOT
  change was never drawn by that path. Five shading theories (fallback texel,
  glass sheen, emissive, alpha policy, culling) all died because their doors
  changed nothing — the fragments never existed.
- **`WGR_SECTION_SKIP_TRACE=<substring>`** (Shape::Draw) — one dump per
  matching shape of every section with its texture, flags, and skip reason.
  Proved all 60 cockpit sections were SUBMITTED with sane state, which moved
  the search from submission to the GPU stages — where clipping lives.
- **`WGR_PLAN_TRACE=1`** (Rust) — one line per frame of the plan's segment
  structure. Established the true op order (`terr grass 3d×155 CLEARDEPTH
  3d×66 RESOLVE 2d×20`), killing several ordering theories in one run.

## Traps recorded for the next reader

- A frame-count screenshot trigger (`500:`) compares DIFFERENT GAME TIMES
  across renderers (GL33 runs at 1.7 fps here); trigger by time (`20s:`).
- `WGR_MSAA=1` and `WGR_ABLATE=clouds` both LOOKED like fixes. Neither was:
  they removed the bright cloud deck behind the missing geometry, making the
  glass tint over darker sky read as "solid dark frame". Verify geometry
  presence (rivets, texture detail), not pixel brightness.
- The per-draw trace prints d.camera BEFORE it is assigned; it now prints
  `_currentCamera`. Its once-per-texture dedup also hides later contexts —
  a "cam=0" line can be a stale first-use from the planar reflection pass.

## What remains open (the shading half)

`uh60_pilot_plechmat.pac` (texture mean 46,43,43) renders at 202,198,186
where GL33 gives 24,25,27 — the interior is massively over-lit. Not emissive
(`WGR_NO_EMISSIVE` no change, retested after the geometry fix), not glass
sheen, not alpha routing. `WGR_GTAO=0` makes it WORSE (65 -> 171): GTAO is
currently the only term holding vehicle interiors down, i.e. the cockpit
receives (steered) sky ambient as if outdoors. Likely the interior-sky /
bent-normal ambient path, which knows about building volumes but not about
sitting inside a vehicle. That is its own ticket; the geometry fix makes it
measurable per-surface at last (WGR_KILL_TEX binary search: plechmat found
in four runs).
