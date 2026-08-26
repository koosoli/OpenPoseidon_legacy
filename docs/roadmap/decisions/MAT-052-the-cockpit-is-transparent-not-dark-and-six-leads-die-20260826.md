# MAT-052 — the cockpit is TRANSPARENT, not dark, and six leads die measuring it

- **Ticket:** MAT-052 (open)
- **Date:** 2026-08-26
- **Status:** OPEN — mechanism NOT found. Six candidates eliminated with a
  same-binary A/B each. One candidate identified and untested. Read this
  before touching the alpha classifier again.

## The witness, and why it is the right one

`dev-missions\pilottest.abel` — **stock** UH-60, `data3d\uh-60.p3d`, ODOL v7,
no mods. This is base-game content: the defect is not a bridge problem, and
the owner says it is a long-standing regression that was once improved and
then came back.

Capture both renderers at the SAME GAME TIME or the comparison is worthless:

```
--test-mission <dir> --window --render wgpu|gl33 --no-splash --oldpaths \
  --auto-screenshot 20s:<png> --log-level debug --log-file <log>
```

A FRAME trigger (`500:`) is a trap here: GL33 runs this scene at ~1.7 fps, so
frame 500 is 300 s of game time later, with a different sun and different
weather. Every brightness comparison across such a pair is meaningless.

`.tmp-rotor/g20.png` (GL33) vs `.tmp-rotor/w20.png` (WGPU) are the pair.

## What is actually wrong

At 5x zoom on the left canopy pillar (`.tmp-rotor/zoom_pair.png`): on WGPU the
grass texels, the horizon line, the distant hedgerow and a bush all run
**continuously across the pillar**. The pillar is transparent. GL33 draws the
same pillar as solid dark riveted metal that occludes completely.

It is NOT "the cockpit renders dark/pale". An earlier reading of this session
took the pillar's top-bright/bottom-dark gradient for a lighting term; the
owner corrected it, and the zoom confirms the correction — the pillar is
bright where SKY is behind it and dark where TERRAIN is behind it, because you
are seeing the background.

Sample points used throughout (full-res pixel coords on the 800x600 capture):

| point | coords | GL33 | WGPU |
|---|---|---|---|
| roof beam (sky behind) | 120,125 | 24,25,27 | 201,197,184 |
| pillar low (terrain behind) | 175,300 | 17,20,19 | 65,66,63 |

## Six leads, each killed by a same-binary A/B

Every one of these left the roof-beam pixel within ±2 of 201,197,184.

1. **Missing texture / fallback texel.** `WGR_TEXTURE_FALLBACK_DEBUG=1`
   recolours slot 0 magenta. The pillars did not turn magenta. The texture is
   bound.
2. **MAT-051 glass sheen.** `WGR_GLASS_SHEEN=0`. Changes the WINDOW pixel a
   lot (137,149,166 -> 52,59,66) and the frames not at all. The sheen is a
   real whitening term on genuine glass; it is not this.
3. **Material emissive.** `WGR_NO_EMISSIVE=1`. No change. (MAT-052's earlier
   record put "emissive 2.0 on a luminance texture" first; it is dead.)
4. **MAT-049 format-alpha policy on tagged textures.** `WGR_ALPHA_POLICY_TAGGED=1`.
   No change. This is the fix that exists-but-is-off pending validation on
   aircraft canopies; it has now been validated against one and does not
   address this defect.
5. **Alpha routing, at all.** New door `WGR_FORCE_OPAQUE_TEX=<substrings>`
   forces every matching draw to the opaque route at the end of
   `EngineWgpu::DrawSectionTL`. With `=data` — i.e. EVERY texture in the game
   — the window becomes opaque (137 -> 202) and **the frames do not change**.
   Whatever draws the frames does not take its blend decision there.
6. **Backface culling.** New door `WGR_NO_CULL=1` (both object pipelines in
   `gfx3d/mod.rs`). No change. The frames are not single-sided faces with a
   flipped winding.

## Two things that are true and are NOT the mechanism

- **The AI88 decode is correct.** Both paths were read line by line:
  `Pactext.cpp`'s AI88->ARGB4444 nibble fold and its AI88->ARGB8888 fold both
  put intensity in RGB and alpha in A, matching `Image.cpp`'s `ai88ToRGBA`.
  Lead (b) of the previous record is dead.
- **No section is missing.** Every texture the pilot-view LOD (LOD 8, res
  1100) declares is drawn, bar one rotor-blur sheet — measured by diffing
  `model inspect -t` against the distinct texture list from
  `WGR_LEGACY_TEXTURE_TRACE=1` (69 textures drawn). The frames are submitted.
- **Every cockpit texture is dark.** Means 12–161 over all 40 drawn cockpit
  textures; the brightest is the glass at 161,171,168. Nothing in the set can
  produce a cream 201,197,184 surface by lighting alone.

## The separate, real defect this uncovered

`WGR_GTAO=0` makes the cockpit interior dramatically WORSE: pillar-low goes
65,66,63 -> **171,165,157**, the whole interior washes out. Screen-space GTAO
is currently the ONLY thing holding the cockpit interior down. Whatever the
transparency turns out to be, interior geometry is receiving sky irradiance as
if it were outdoors, and only a screen-space term is compensating. That is its
own ticket, and it is why "turn GTAO off" must never be offered as a
workaround here.

## The one candidate NOT yet tested

`aff5310c` (2026-08-15, "Cockpits are translucent, and the alpha scan change
had no A/B") named two next candidates and neither was ever checked:

1. **The cockpit-specific `PassKindHint` path.** `BuildRenderPassDescriptor`
   routes a cockpit draw with `Backend::IsAlpha` to
   `PassKind::CockpitTransparent`. On WGPU `PassKind` is consumed only by
   `ValidateFrame` — but `Pass 3` as a whole is a separate submission, and if
   the cockpit pass composites with blending enabled at the PIPELINE level,
   then every cockpit surface is see-through, per-draw `d.blend` is irrelevant
   (lead 5), culling is irrelevant (lead 6), and every texture is still
   "drawn" (the section census). That is the only hypothesis left standing
   that explains all six negatives at once.
2. **Pass2's depth-write-on blend branch.**

Note the dashboard looks correct in the same frame — which is consistent with
(1), because the dashboard blends against the dark cockpit interior behind it
and the error is invisible there. Test (1) FIRST.

## Diagnostics landed with this record

- `WGR_FORCE_OPAQUE_TEX=<comma-separated substrings>` — force matching draws
  to the opaque route. Answers "which surface is this pixel, and would opaque
  routing even be right" in one run. The existing `WGR_LEGACY_TEXTURE_TRACE`
  names every texture that blends but cannot say which covers a given pixel.
- `WGR_NO_CULL=1` — disable backface culling on both object pipelines.
  Separates "not drawn because culled" from every other reason.
