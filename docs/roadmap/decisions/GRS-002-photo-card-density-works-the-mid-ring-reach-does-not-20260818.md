# GRS-002 — The card density control works; the mid ring's *reach* was the bug

**Date:** 2026-08-18
**Branch:** `feat/wgpu-decal-billboards`
**Status:** **Handover
[GRASS-photo-card-density-20260818](../handover/GRASS-photo-card-density-20260818.md) CLOSED.**
Its three outcomes resolve to outcome 3, with a mechanism it predicted but had not measured.
**Related:** [GRS local A3 clutter/tuft compatibility](GRS-local-a3-clutter-tuft-compatibility-20260810.md)

## The result

Malden, `dev-missions/devtest.abel`, hour 11, 1600×900, mission camera, cards forced on
(`WGR_GRASS_CLUTTER_SOURCE=2`). **Same binary, same pose, same frame. Only
`POSEIDON_GRASS_CARD_DENSITY` — the panel slider's own code path — differs.**

| density | card grid | near grid | mid grid | mid **reach** | vs 210 m asked | looks like |
|---|---|---|---|---:|---|---|
| 1x (default) | 1.11 m | 80² | 384² | **212 m** | spans it | scattered tufts, metres of bare ground |
| 25x | 0.22 m | 376² | 1896² | **210 m** | spans it | **continuous meadow** |
| 300x (slider max) | 0.06 m | 1280² | 2440² | **78 m** | **132 m short** | dense, and the field ends at 78 m |

Captures and logs: `.tmp-grass-reach/after-{1,25,300}x.{png,log}`.

**The control was never broken.** 1x is a sparse tuft field and 25x is the meadow the owner is
asking for, from one slider, on their own world.

## What WAS broken

`MID_GRID_MAX` caps the mid ring's candidate grid at 2440 cells. Its reach is therefore
`dim/2 × spacing`, and on the **card** grid — which the density control drives from 1.11 m down to
0.06 m — that cap bites well inside the requested radius:

```
reach = 1219 × card_spacing     50x → 191 m    100x → 135 m    300x → 78 m
```

against a `midRadius` default of 210 m. `mid_ring_end()` still returned 210, so the field was
bounded by the grid's **square** instead: a hard camera-locked edge with no dissolve, and grass that
gets **shorter** the denser you ask for. Past ~45x, "turn the density up" made the meadow *smaller*.

The comment in `grass/mod.rs` said the mid grid "needs no equivalent cap" — true of the 0.82 m
ribbon grid (2440 × 0.82 / 2 = 1000.4 m ≥ `MID_RING_MAX`), and false of the card grid. The **near**
ring has clamped its radius to its grid's reach since the projected-size work; the mid ring never
got the same treatment because until the card grid existed it never needed it.

## The fix

* `grass.wgsl` — `mid_ring_end()` clamps to `max(place.y × 0.5 − 1, 1) × mid_placement_spacing()`,
  the same one-cell-of-jitter margin the near ring uses. The ring now ends in its own circular
  dissolve strictly inside the square, and `cs_place_far` — which starts from the same function —
  no longer leaves a bare annulus between the last card and the far ring.
* `grass/mod.rs` — the announce line now reports the reach:
  `mid ring cards at 0.06 m (2440x2440 grid, reach 78 m)`. Predicted 78, measured 78.
* `DebugOverlay.cpp` — the Grass tab prints the same number next to the density slider, in orange
  once the reach falls short of the radius. **The trap is now visible before you fall into it.**
* Regression test `mid_ring_radius_is_clamped_to_the_capped_grid_reach` pins both the shader clamp
  and the arithmetic, including that the ribbon grid still spans the whole 1000 m ring.

## What this does NOT say

- It does **not** claim 1x should be denser. 1x is deliberately the map's own `clutterGrid` — one
  card per clutter cell, thinned by the surface's authored probability. **The default look is an
  owner decision, not a bug**; the panel now states the multiplier that reaches procedural parity.
- The panel → engine link was never instrumented end to end by a human hand on the slider. It is
  ruled out by inspection instead: `DebugOverlay.cpp` reads `GetGrassSettings()` once (2997) and
  writes `SetGrassSettings()` once (3762), in the same scope on the same local, with no shadowing;
  and `SetGrassSettings` is called from **nowhere else in the tree**, so nothing can stomp it.
  Outcomes 1 and 2 of the handover are dead.
- The 300x field is still 78 m deep. The fix makes that honest and legible; it does not make the
  2440-cell grid bigger. Raising `MID_GRID_MAX` is a cost decision nobody has measured.
