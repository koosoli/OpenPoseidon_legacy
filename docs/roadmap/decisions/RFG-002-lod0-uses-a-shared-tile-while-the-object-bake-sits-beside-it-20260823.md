# RFG-002 — Reforger LOD 0 draws a shared library tile while the object's own baked albedo sits unused beside it

**Date:** 2026-08-23
**Status:** diagnosed, not yet fixed
**Reported as:** "when I am close to for example the church the textures are missing, but when I am
further away, they do load" — freefly `5168.77 3929.13 39.12`, az 19.1, el -15.1 (Reforger Everon).

## It is not streaming, and it is not a missing texture

The obvious reading is a mip/residency fault. It is not. Measured on the reported pose, the run's
own material coverage line says:

```
Objects material coverage: baked sections=3795 untextured=95 (2.503%) | tris=5382816 untextured=13137 (0.244%)
```

95 sections, 0.24% of triangles, and not one of them belongs to `church_01` — they are decal
materials (`*_Decal.emat`, `ST_decal_Broken_Edge`, `Common/Materials/Default.emat`). The church is
fully textured in the renderer's own accounting. What changes with distance is **which material**.

## The split

`church_01` ships two material sets, and both load in the same run:

| | family | albedo it binds |
|---|---|---|
| `church_01_extwalls_yellow_v2.emat` | `MatPBRMulti` | `_shareddata\brick\st_bricks_03_bcr_17.paa` |
| `church_01_roof_v2.emat` | `MatPBRMulti` | `_shareddata\roof\st_roofing_tiles_ceramic_01_bcr.paa` |
| `church_01_extdetails_v2.emat` | `MatPBRMulti` | `_shareddata\stone\st_castlewall_01_bcr_5.paa` |
| `church_01_ext_mlod_yellow_v2.emat` | `MatPBRBasic` | `church_01_ext_mlod_yellow_v2_bcr.paa` |

The last row is the object's **own baked albedo, in the object's own UV layout**, 174,824 bytes,
sitting in the same directory. The lower LODs use it. LOD 0 does not.

The three `MatPBRMulti` rows carry the GUID `{0000000000000000}` on their `BCRMap`, which is the
converter's marker for a synthesised binding: `TranslateEmat` found no `BCRMap` of the object's own,
so the bake substituted the lowest enabled layer's shared tile. That tile is a one-metre library
square (`_shareddata`), authored to be blended through `MaskMap` by a shader this engine does not
have — see the `MatPBRMulti` note in
[HANDOVER-20260823b](../HANDOVER-20260823b-everon-renders.md). Worse, the material says
`UVSrcGlobalMaps "UV set 2"`, and the converted MLOD carries one UV set, so the tile is not even
sampled in the frame it was authored for.

So: **close = a stretched shared brick tile under a pale layer tint; far = the real church.** Which
is exactly, and only, what was reported.

Evidence, same church, same hour, camera distance the only difference:

- `evidence/rfg-church-lod0-flat-near-20260823.png` — LOD 0. Flat cream walls, flat cream roof, no
  tiles, faint vertical streaking where the tile is stretched.
- `evidence/rfg-church-baked-albedo-far-20260823.png` — from 200 m. Red-brown ceramic roof, yellow
  walls, window and trim detail.

## The fix that is available without writing a blend shader

Prefer the object's own bake. When a `MatPBRMulti` material has no `BCRMap` of its own and a sibling
`*_MLOD*_bcr` **does** exist for the same object, bind that instead of a `_shareddata` layer tile.
It is the same texture the object's own lower LODs already use, it is in the object's UV layout, and
it needs no new shader. The layer tint can still multiply it.

The real fix is still a `MatPBRMulti` blend shader (4,191 files, the commonest class in the corpus);
this is the part that can land first and takes the whole visible regression with it.

Not attempted here: repointing the three `.emat` files by hand in the game folder would have proved
the UV assumption in one capture. It was refused — the game install is the owner's — so the UV-layout
claim above is inferred from `MaskMap` being the object-UV slot, not measured.
