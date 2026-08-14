# DZ-004 — The legacy terrain texture is picked by filename, and that has now failed on three generations

**Date:** 2026-08-14
**Status:** Fixed and measured, both backends.
**Related:** [DZ-002](DZ-002-dayz-authors-river-flow-in-a-texture-not-the-placement-20260814.md).

## The bug

Every terrain material resolves a *legacy* per-cell texture — the one the original OFP cell frame
samples, and the fallback the whole ground depends on when the authored satellite/mask/surface stack
is unavailable. `LandSave.cpp` picked it by scanning the material's stages for a filename containing
`_co.paa` and not containing `detail`.

**DayZ names every terrain surface colour `_ca.paa`** — `en_grass1_ca.paa`, `en_soil_ca.paa`, and so
on. The filter therefore matched nothing on any of Enoch's 9,432 terrain materials, `SetTexture` was
never called, and the legacy slot stayed null. The consequences:

* **GL33 drew the ground pure white** — measured over a 180,000-pixel ground window: mean luminance
  **1.0000**, σ **0.0000**, **1 unique colour**.
* On WGPU the authored path covered for it, so nothing looked wrong — but the ground was a **single
  point of failure**. Disabling the authored stack (`WGR_TERRAIN_A3_SURFACES=0`) gave the same white:
  mean **1.0000**, σ **0.0002**, **2 unique colours**.

## Why this is the third time

The comment block immediately above the filter already records the same failure on Arma 1: *every*
Sahrani surface colour is named `<surface>_detail_co.paa`, so the "not a detail layer" clause rejected
all four of them on all 4,859 tiles and that terrain drew white too. The fix then was to stop
pattern-matching and take slot 0's colour from the family record — but that was applied only to the
branch for families whose shader id names their slots, and DayZ's does not take that branch.

So the same class of bug has now bitten Arma 1 (`_detail_co`) and DayZ (`_ca`), and the remedy was
already written down beside it.

## The fix

Prefer the colour the authored pass has **already resolved from the family record**. By the time the
legacy slot is chosen, `authored.surfaces[0]` holds slot 0's colour, obtained by stage index rather
than by name. Using it costs nothing, needs no new suffix, and generalises to whatever the next
generation calls its files. The filename scan stays as the last resort for materials that resolve no
authored surface at all.

The lesson the existing comment drew — *take the colour from the family, not from the name* — is
simply now applied to both branches instead of one.

## Measured

Enoch, WGPU, identical camera, ground window of 180,000 px:

| | mean lum | σ | unique colours |
|---|---|---|---|
| `A3_SURFACES=0`, before | 1.0000 | 0.0002 | **2** (white) |
| `A3_SURFACES=0`, after | 0.2013 | 0.0208 | **6,051** |

GL33 — the backend where this was visible in ordinary use:

| | mean lum | σ | unique colours |
|---|---|---|---|
| before | 1.0000 | 0.0000 | **1** (pure white) |
| after | 0.1328 | 0.0131 | **1,394** |

Unit suite: 11 failures, exactly the 11 that already failed.

## What this does not claim

* **It does not change the WGPU look in normal use.** With the authored stack on, WGPU already
  composited a correct ground; this removes the hidden dependency, it does not alter the picture.
* **It is not a judgement on whether `_ca` is the right texture to sample.** It is the colour the
  material's own family record names as surface slot 0, which is the same thing the authored path
  binds. Whether DayZ's terrain reads at the right brightness is a separate question — the authored
  surface stack darkens the ground by 1.6–2.5× through a `×2` normalisation that assumes an average
  albedo of ~0.5, and DayZ's surfaces measure 0.105–0.144. That is unfixed and separately recorded.
