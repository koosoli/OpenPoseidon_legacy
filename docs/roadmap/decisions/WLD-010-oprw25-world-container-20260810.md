# WLD-010 — Arma 3 world container (OPRW revision 25)

- **Ticket:** WLD-010
- **Date:** 2026-08-10
- **Scope:** world/WRP parsing only. No terrain rendering, no object instantiation,
  no material resolution.

## What was measured

Two locally installed Arma 3 worlds were used as the integration fixture. They are
licensed retail data, extracted under `packages/` (gitignored) and never committed.

| World | Archive | Entry | Size | SHA-256 |
| --- | --- | --- | --- | --- |
| VR | `a3-game:Addons/map_vr.pbo` | `vr.wrp` | 393,427 B | `536cf71047c478d5f84ac65f04ecba0f3555b5eb58725dc0a0171319c261bff4` |
| Stratis | `a3-game:Addons/map_stratis.pbo` | `stratis.wrp` | 24,518,659 B | `08f5a3dbe661c9b50951445158518577a1fbfa6646bf785ae2c3c926f488b2fe` |

Both declare `OPRW` revision **25**, app id **107410**.

Header facts, identical on both worlds:

```
land grid      256 x 256   @ 32.0 m
terrain grid  2048 x 2048  @  4.0 m
world extent  8192 m square
```

The two grid resolutions are the first thing that separates revision 25 from the
OPRW 2/3 the legacy `WrpReader` handles, which has a single grid. Conflating them
would size five different payloads wrongly.

## Why a new reader rather than extending WrpReader

Revision 25 shares its four magic bytes with the OFP-era container and essentially
nothing else:

| | OPRW 2/3 (OFP/CWA) | OPRW 25 (Arma 3) |
| --- | --- | --- |
| per-cell grids | dense arrays | 16-ary quadtrees, 4-byte leaves |
| bulk compression | LZSS | LZO (revision 23 onward) |
| elevation | `int16` scaled by 0.045 | `float` |
| grids | one | land + terrain, 8:1 |
| object placement | per-cell lists | flat table sized in bytes |
| app binding | none | Steam app/DLC id (revision 25's addition) |

`engine/Poseidon/Asset/Formats/World/Oprw25.hpp` follows the `Odol73` precedent: a
deliberately narrow header-only reader that accepts exactly one revision and throws
on anything else. Revisions 3, 17, 24 and 26 are negative-dispatch controls in the
unit test — accepting a v3 file here would read an OFP world's dense grids as
quadtrees and yield plausible-looking nonsense.

## How the layout was confirmed

Not "it did not crash". The container carries three independent self-checks, and
the reader enforces all of them:

1. **Road network size.** The file declares the byte length of the road network,
   then stores it as one variable-length list per land cell. Walking all 65,536
   cells must land exactly on the declared length — VR 262,144, Stratis 306,262.
   Both match to the byte.
2. **Map-info size.** The file declares the trailing map-info block's length long
   before the object table. What remains after reading the object table must equal
   it — VR 0, Stratis 1,482,744. Both match.
3. **Object table size.** Given in bytes, and must be a whole multiple of the
   60-byte record. Stratis: 9,837,180 / 60 = 163,953 records, and every one of them
   references a model index inside the 350-entry model table (0 out of range).

A wrong field width anywhere earlier could not satisfy all three on both files.

Cross-checks that are consistent but not proofs: the highest of Stratis's 1,408
stored mountain peaks (235.68 m) equals the maximum of its 4,194,304 elevation
samples, and every object position falls inside the 8192 m world square
(x 244.2–7983.7, z 11.7–8184.0).

## Compression rule

Revision 23 moved the bulk arrays to LZO. Unlike ODOL 73 there is **no per-payload
compression flag**: a payload is compressed exactly when its decompressed size
reaches the 1024-byte threshold the engine already uses elsewhere. This was not
assumed — it is the rule under which both files consume to the byte. Under any
other rule the very next field desynchronises.

Poseidon's existing `Foundation::Lzo1x` decodes every payload in both worlds
without modification.

## Quadtree leaf geometry

Each leaf is four bytes covering a small rectangle whose shape depends on the
element width. The reader derives it (2×2 for byte elements, 2×1 for 16-bit, 1×1
for 32-bit) rather than tabulating it. This reproduces the MIT-licensed
`bis-file-formats` reference wherever that reference is unambiguous, and resolves
the byte case — where the reference returns leaf offset 0 for all four cells,
leaving three of its four bytes unreachable — to the only layout that fills the
leaf. The world's byte-element grid (the sound map) is structurally consumed but
not retained, so nothing currently depends on that resolution.

## Provenance

Field order and version gating were read from `bis-file-formats` (MIT, ©2017
Braini01), studied as documentation. The implementation here is written against
Poseidon's own `BinaryReader`, `Lzo1x` and container idioms, with its own bounds
checking, error model and quadtree representation.

## Measured Stratis census (2026-08-10)

```
placements                163,953
model table                   350   (350 referenced, 0 unused)
static entities             1,272   (97 config classes)
surface materials             479   (478 distinct in use)
road links                    432   (2 segment models)
mountains                   1,408
elevation                -157.5 .. 235.7 m
below sea level         2,992,297 / 4,194,304 terrain cells
map info                1,482,744 bytes retained, not decoded
```

## What this does NOT claim

- No terrain geometry is built, and no world is rendered.
- No P3D referenced by the model table has been resolved or loaded through this
  path. "350 models referenced" is a count of strings, not of loadable assets.
- The map-info block is retained as bytes and not decoded.
- The sound-map, object-offset and map-info-offset quadtrees are consumed for
  stream position only; their contents are discarded.
- The per-material "major" flag is read and dropped.
- Revision 24 and earlier, and 26 (VBS), are not supported.
- ~~Two worlds is the entire corpus. Both are official Bohemia worlds of identical
  grid dimensions; a community world with different `landRange`/`terrainRange`
  ratios has not been tested.~~ **Resolved the same day (WLD-012):** Altis parses
  to the byte at 1024 land / 4096 terrain / 30 m cells / 30,720 m extent — a
  different ratio and cell size. Still official Bohemia content only; no community
  world or mod has been tested.

## Next blocker

Model-path resolution: turning the 350 model strings into loadable P3Ds through the
addon/PBO lookup, and classifying the failures. That is the measurement that turns
"163,953 placements" into a renderable-coverage denominator.

**Answered in WLD-011:** all 350 resolve and 304 parse as static revision-73
models, covering 163,059 of 163,953 placements. The remaining gap is three named
unimplemented ODOL 73 features, largest first: model-directory animations,
LOD proxies, vertex bone references.
