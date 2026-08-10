# WLD-012 — Closing the ODOL 73 gap a real world exposes

- **Ticket:** WLD-012
- **Date:** 2026-08-10
- **Depends on:** WLD-011 (Stratis model coverage), AST-012B (the revision-73 reader)

## Result

| world | models | placements | parsed static |
| --- | ---: | ---: | --- |
| Stratis | 350 | 163,953 | **350 / 163,953** |
| Altis | 963 | 1,779,908 | **963 / 1,779,908** |

Every model referenced by either world now parses. Combined: **1,313 model parses
over 1,943,861 placements**, all ODOL revision 73, no failures, no unsupported
revisions, no missing assets.

The starting point (WLD-011, same day) was 304/350 on Stratis and Altis untested.

## What was blocking, in the order it was removed

Each was a deliberate refusal in the revision-73 reader — a section whose layout
was unimplemented, refused rather than skipped so that later offsets could not be
trusted by accident. Removing one exposed the next, which is exactly what the
refusals are for.

| step | blocked | after |
| --- | --- | --- |
| model-directory animations | 35 models / 519 placements | 304 → 304 (moved to LOD-level) |
| LOD proxies, sub-skeleton mapping and sets | 40 / 870 | 304 → 309 → 315 |
| per-vertex bone references | 38 / 532 | 315 |
| section-count bound of 64 | 3 / 11 | 315 |
| neighbour bone references | 35 / 421 | **350** |

The animation step is worth noting: it changed no count on its own, because the 35
models it unblocked immediately hit a *different* refusal one level down. That it
moved them from the directory reader into their LOD bodies at all is the evidence
it parsed correctly — the directory validates every LOD's declared byte range
against the file size before returning, so a mis-sized animation block cannot
produce a plausible LOD table.

## What "read" means for each block

Nothing here is applied, and nothing is skipped blind.

- **Animation classes** are decoded into their type-dependent payloads (rotation,
  translation, direct, hide). This is not decoration: types 8 and 9 carry their
  axis inline and therefore take *no* per-bone axis pair in the lookup tables that
  follow, so the type must be understood to get the block's length right. An
  unrecognised type is still a refusal.
- **Bone/animation lookup tables** are kept in both directions, per resolution.
- **Proxies** are read as declared attachments (model path, transform, selection,
  bone, section). They are not resolved — following one would pull in a second
  asset, and a census counts what a file declares.
- **Sub-skeleton mapping and sets** are kept as the two halves of one mapping.
- **Vertex bone references** are kept as a count plus eight bytes of paired bone
  and weight, the 12-byte wire element.
- **Neighbour bone references** are kept as raw 32-byte records, unconverted, in
  the same spirit as the existing `stCoords`: naming their fields on the strength
  of their width would be a claim, not a read.

Preserving rather than discarding follows the handover's rule for animated assets:
load the static representation, keep the animation information in canonical form.

### The one bound that was raised, and why that is not a loosened check

`sections > 64` was a fixture-sized limit from the original rectangle fixture; real
Stratis buildings exceed it. It is now 8192. This does not weaken validation,
because the section array was never bounded by that constant in any meaningful
sense: the LOD's own declared `end` offset is the check that the sections landed
where they should, and it is unchanged. The constant only exists to stop a
desynchronised stream driving a huge allocation.

## Why Altis matters more than the count suggests

WLD-010 recorded an explicit limitation: the OPRW 25 reader had been validated
against two worlds of *identical* grid dimensions (256 land / 2048 terrain / 32 m),
so it could not distinguish a correct two-grid reader from one that happened to
work at 8:1.

Altis is 1024 land / 4096 terrain / 30 m land cells / 7.5 m terrain cells, a 30,720 m
world — a different ratio and a different cell size. It parses to the byte under the
same self-checks (road network and trailing map-info sizes). That limitation is
resolved. The local-corpus unit test now asserts the grid *relationships* rather
than the literals it was first written against, and includes Altis.

## What this still does not claim

- These are parse results. Nothing here asserts that geometry is correct, that
  materials resolve, that renderer vertex buffers are built, or that anything draws.
- No proxy is resolved, no animation is evaluated, no skinning is applied.
- Two worlds, both official Bohemia content. No community world, no mod.
- The census reads models directly from archives; it does not go through the
  engine's bank *mount* path, whose prefix handling remains the open question
  recorded in WLD-011.

## Test status

`[OPRW25]` and `[Formats]`/`[odol]` suites pass (261 assertions). The full unit run
has 8 pre-existing failures unrelated to this work — GL33 rendering, dev-mode
gates, and stringtable localization cases; `Odol73.hpp` is not reachable from any
of them.

## Next blocker

Materials and textures. The world's 350/963 models parse, but the census does not
yet follow the RVMAT and PAA references inside them, and the terrain's own surface
materials (479 on Stratis, 14,731 on Altis — the `TerrainX`/`TerrainSNX` families)
are counted but not resolved. That is the measurement that turns "parses" into
"renderable".
