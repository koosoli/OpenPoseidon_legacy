# MAT-020 — the RVMAT render state reaches nothing, and must not be wired through whole

**Date:** 2026-08-09
**Ticket:** MAT-020
**Status:** Defect established by measurement; the obvious fix is measured to be wrong.

## The defect

`TranslatedMaterial` carries `ambient`, `diffuse`, `forcedDiffuse`, `emissive`, `specular`
and `specularPower`, described in the ledger as carried straight through rather than
reinterpreted. Nothing consumes them. `EngineWgpu::DrawSectionTL` folds `_curMaterial`,
which for an Arma 3 section with an authored RVMAT is the constructor default
(`ambient = HWhite`, `diffuse = HWhite`, `emmisive = HBlack`).

Proved rather than read off the code. The shipped pine's two RVMATs were rewritten as
text with `emmisive[]={1,0,0,1}` — bright red — and the night capture changed by **less
than 0.01 per channel** against the unmodified original. Nothing red appeared. The
authored render state is not reaching the draw.

Consequence: these objects light by a default white material regardless of what the
asset says, which is why the A3 trees do not follow the day/night curve while CWA trees
beside them go black.

## Why the obvious fix is wrong

Feeding the translated values into the existing fold looks like a two-line change. The
corpus says otherwise. `emmisive` rgb across all 45,304 Arma 3 RVMATs:

| family | n | black | white | other |
|---|---:|---:|---:|---:|
| `TerrainSNX` | 20,403 | 20,403 | 0 | 0 |
| `TerrainX` | 17,071 | 17,071 | 0 | 0 |
| `Super` | 4,580 | 4,226 | 9 | 339 |
| `Multi` | 1,628 | 1,605 | 0 | 10 |
| `TreeAdv` | 381 | 4 | 187 | 188 |
| `TreeAdvTrunk` | 174 | 61 | 112 | 1 |
| `Skin` | 347 | 0 | 0 | 347 |

Emissive is black in roughly 97% of non-tree materials and non-black in roughly 99% of
`TreeAdv`. If the slot meant literal self-illumination, every Arma 3 tree would glow at
night in Arma 3 itself. They do not. So the tree families use it for something their
shader consumes, and applying it as unconditional self-illumination — which is what the
current draw does with `mat_emissive`, deliberately unmultiplied by the sun enable —
would make the reported night brightness worse rather than better.

`Skin` is the same shape of warning from the other direction: 347/347 non-black, on a
family with no schema yet.

## What may be wired through

- `ambient`, `diffuse`, `forcedDiffuse`, `specular`, `specularPower` — these fold exactly
  as `_curMaterial` already folds them, and the fold is not being changed, only fed.
- **Not** `emissive`, for any family, until a family's use of the slot is established the
  way its stage table was. Leaving it at the current default is the behaviour-preserving
  choice.

## Hazard for whoever wires it

14 `Normal` materials carry **expressions** rather than numbers in these arrays — e.g.
`0.32*(5.5/sqrt(100))`. A reader that assumes a float list will throw or silently take a
zero. The census hit this on the first pass.

## What this does not explain

Wiring the four safe terms through does not obviously fix the night brightness on its own:
the pine trunk's authored `ambient` is `[1,1,1,1]`, the same white the default already
supplies. The brightness therefore has a second source, still unidentified, and this
ticket should not claim it as the fix until a night capture measures the difference.
