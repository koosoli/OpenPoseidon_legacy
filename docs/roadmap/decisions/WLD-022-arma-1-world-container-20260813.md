# WLD-022 — The Arma 1 world container (OPRW 18 and 20), and a hole in how WLD-010 was verified

- **Ticket:** WLD-022
- **Date:** 2026-08-13
- **Branch:** `grass-lod-dissolve-photo-cards`
- **Scope:** world/WRP parsing only. No A1 model reading, no terrain material
  families, no rendering.
- **Related:** WLD-010 (OPRW 25), WLD-014 (one-grid assumption), WLD-019
  amendment (A1 is the third mask witness), MAT-037 (section stride)

## Why Arma 1

The owner's framing, and it turned out to be right: A1 sits between OFP and A2, so
reading it says *where* each later change actually happened rather than leaving a
five-year gap between two containers that share only four magic bytes. The
answer is the headline of this record, and it is not what the layering of
WLD-010 implied.

## What was measured

Three worlds, extracted from the licensed local A1 packages into `packages/`
(gitignored, never committed):

| world | archive | revision | land grid | terrain grid | land cell | extent |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| Sahrani | `sara.pbo` → `sara.wrp` | **20** | 512² | 2048² | 40 m | 20,480 m |
| Sahrani (Queen's Gambit) | `Sara_DBE1.pbo` → `sara_dbe1.wrp` | **18** | 512² | 2048² | 40 m | 20,480 m |
| Southern Sahrani | `saralite.pbo` → `saralite.wrp` | **18** | 256² | 1024² | 40 m | 10,240 m |

The known gate was that `sara.wrp` is revision 20 and `LoadOprwModern` accepted
only 24 and 25. That was half the story: **the corpus also contains revision
18**, and the two are byte-identical in layout — the same reader consumes all
three files exactly. A version check written for 20 alone would have loaded one
world in three.

Note also the ratio: **4:1** at a **40 m** land cell. Every previously-read world
is 8:1 (Stratis, VR, Takistan, Zargabad, Chernarus) or 4:1 at 30 m (Altis). A1 is
an independent witness for the two-grid header.

## The four differences from revision 24

Everything else is already the revision-24 layout. That is the finding.

| | revision 18 / 20 | revision 24 / 25 |
| --- | --- | --- |
| bulk array codec | **LZSS** (`SSCompress`) | LZO (from revision 23) |
| after the material-index quadtree | **dense per-land-cell `uint16` randomization array** | absent |
| per-terrain-cell byte arrays before elevation | **one** | two |
| road link record | positions → objectId → path | positions → **one type byte per connection** → objectId → path |

Identical in revision 18 already: the two-resolution header, all four quadtrees
(geography, sound map, material index, and the object/map-info offset trees), the
mountain list, the terrain material table *with* its trailing `major` byte, the
model table, the static-entity records, the object- and map-info-table byte
counts, the `persistent` and `subdivision` arrays, the **60-byte** object record,
and the trailing map-info block.

### Evidence, field by field

**Codec.** Revision 20 is below the documented revision-23 switch, so LZSS was
the hypothesis; it is confirmed by the payloads themselves. An `SSCompress`
stream ends in a 32-bit checksum the decoder verifies, so every bulk array in
these worlds proves its own declared size at the array rather than desynchronising
several fields downstream. This is a *stronger* per-payload check than the LZO
path has, and it is why the layout below could be reversed one payload at a time.

**The randomization array.** 524,288 bytes on `sara.wrp` — exactly
`512 × 512 × 2` — decoding with a valid checksum, sitting between the material
index quadtree and the terrain grids. Its content names it: the low byte spans
0–255 uniformly, and the high byte's two nibbles take values `{0,1,2,3,4,5,
11,12,13,14,15}` and nothing else, which as signed 4-bit is **−5..+5**. That is
`Landscape::RandomInfo` — `colour:8, uOff:4, vOff:4`, the comment in
`Landscape.hpp` giving the offset range as −7..+7.

The clincher is the field order. `Landscape::Serialize` (the OFP-era OPRW 2/3
path, `LandSave.cpp:1378-1388`) writes:

```
_geography  _soundMap  _mountains  _tex  _random  _data
```

and revision 20 writes:

```
geographyQT  soundMapQT  mountains  materialIndexQT  random  <terrain byte>  elevation
```

Same order, same array, with the four per-cell grids that could be sparse
promoted to quadtrees and `_random` — which is noise and compresses to nothing —
left dense. Revision 24 dropped it and the engine regenerates the equivalent at
load time in `Landscape::InitRandomization`. **A1 is the last generation that
shipped it in the file.**

**One terrain byte array, not two.** Structural: the byte after the randomization
array's checksum begins a payload that checksums at exactly `2048 × 2048`, and the
payload after *that* checksums at exactly `2048 × 2048 × 4` and decodes as
plausible terrain heights (first samples −70.0, −40.1, −30.1 m: sea floor). There
is no room for a second byte array.

Which of revision 24's two is missing is **inferred, not proved**: A1's array has
35 distinct values with a long tail (max 34), which matches the shape of the
*first* array on every later world (Takistan 42 / max 41, Chernarus 59 / max 58)
and not the second, which is capped at 5 everywhere (Takistan 6, Chernarus 6,
Stratis 5). It is read as the first. Nothing in the engine consumes either array,
so nothing depends on the choice.

**No road connection types.** See the next section — this one is the interesting
part, and the size check does not decide it.

### Self-checks, all three worlds

The same standard WLD-010 held revision 25 to, and it is not "it did not crash":

| | sara | sara_dbe1 | saralite |
| --- | ---: | ---: | ---: |
| road network declared bytes | 2,191,865 | 2,260,476 | 835,019 |
| road network consumed bytes | 2,191,865 | 2,260,476 | 835,019 |
| bytes after object table vs declared map info | 8,390,876 = 8,390,876 | 8,440,564 = 8,440,564 | 2,875,632 = 2,875,632 |
| object table ÷ 60 | 36,148,860 / 60 = 602,481 | 36,279,120 / 60 = 604,652 | 10,129,020 / 60 = 168,817 |
| placements with an out-of-range model index | 0 | 0 | 0 |

Plus a cross-check the file gives for free: the highest of the stored mountain
peaks equals the maximum of the elevation grid, on all three worlds
(757.13 m on both 512² worlds, 391.97 m on Southern Sahrani). Those are two
independent encodings of the same number.

---

## The correction to WLD-010: a self-check that cannot see what it was cited for

WLD-010 rests on three self-checks, and the first is:

> **Road network size.** The file declares the byte length of the road network,
> then stores it as one variable-length list per land cell. Walking all 65,536
> cells must land exactly on the declared length […] Both match to the byte.

That is true, and it is real evidence for the *link count*, the *connection
count*, the position widths and the 4×3 matrix. **It is not evidence for the
per-connection type byte**, and that is the field it was implicitly covering.

The reason is the segment path. A road link ends with a null-terminated `p3d`
path. If the reader consumes `connections` bytes too many or too few before it,
the `asciiz` read simply stops at the same terminator, absorbing or yielding the
difference — the walk **resynchronises inside every single link**. Measured
directly: reading `sara.wrp` with the revision-24 rule and without it *both*
consume exactly 2,191,865 bytes over 11,530 links. Reading `stratis.wrp` with the
rule and without it both consume exactly 306,262.

The discriminator is the string, not the size:

| world | correct rule | wrong rule |
| --- | --- | --- |
| sara (rev 20) | `ca\roads\ces_d10 100.p3d`, 73 distinct segments | `\roads\ces_d10 100.p3d`, 107 distinct |
| stratis (rev 25) | `a3\structures_f\bridges\bridge_01_f.p3d`, 2 distinct | `\xNN\xNNa3\structures_f\…`, 4 distinct |

Two characters eaten off the bank prefix, and a distinct-segment count inflated by
the garbage prefixes — which is the cheap tell, because a world does not have 107
road segment models.

**Our Arma 2 and Arma 3 handling is correct.** Read with the connection-type byte,
every A2/A3 segment path is intact and the distinct count is sane; read without
it, they are not. But that was never actually evidenced, and would not have been
caught by the check that was cited. This record supplies the evidence, and the
unit test now asserts the *path*, not only the size.

The general lesson, worth carrying to the next format: **a length field validates
everything up to the last self-terminating element in the record, and nothing
between that element and the previous one.** Null-terminated strings, and any
sentinel-delimited field, are resynchronisation points that a byte-count check
cannot see past.

---

## Lineage: what changed in the world container, OFP → A1 → A2 → A3

Every row is something observed in this corpus, not recalled.

| | OFP / CWA (OPRW 2/3) | **Arma 1 (18/20)** | Arma 2 / OA (24) | Arma 3 (25) |
| --- | --- | --- | --- | --- |
| grids | one | **two, 4:1** | two, 8:1 | two, 8:1 or 4:1 |
| land cell | 50 m | **40 m** | 32–50 m | 30–32 m |
| per-cell grids | dense arrays | **16-ary quadtrees** | quadtrees | quadtrees |
| elevation | `int16` × 0.045 | **`float`** | `float` | `float` |
| bulk codec | LZSS | **LZSS** | LZO | LZO |
| `_random` per-cell array | stored | **stored** | dropped (runtime) | dropped |
| object placement | per-cell lists | **flat 60-byte table, sized in bytes** | same | same |
| named static entities | — | **present** | present | present |
| terrain material table | — | **present, with `major` byte** | present | present |
| road connection types | — | — | added | present |
| map-info block | — | **present** | present | present |
| Steam app / DLC id | — | — | — | added |
| terrain surface family | none | `Terrain1..15`, 3-stage (`_mco`/`_nohq`/`_co`), 4 slots, `m_*_lco` mask | `TerrainX`, 2-stage, 6 slots, `_lca` | `TerrainSNX`, 5 slots + per-tile normal |
| model container | ODOL 7 | **ODOL 40** | ODOL 48/49/50/52 | ODOL 73 |

(The terrain-surface row is WLD-019 and its amendment, restated here so the
lineage reads in one place.)

**The headline: the "modern" world container arrives fully formed in Arma 1, not
in Arma 2.** Two grids, quadtrees, float elevation, the flat object table, named
static entities, the terrain material table and the map-info block are *all*
already present in revision 18. Between A1 and A2 the container changed in four
small ways: a codec, one dropped legacy array, one added array and one added byte
per road connection. Between OFP and A1 it changed in every structural respect
there is.

This reframes the WLD-010 table, which contrasted "OPRW 2/3" with "OPRW 25" and
attributed the whole distance to the later generation. The distance is real; it
was travelled in 2006, not 2009.

## Design: extended, not forked

`Oprw25.hpp`'s reader became `ReadOprwModern(reader, revision)` with four gates
set from the revision, and `Oprw20.hpp` joins `Oprw24.hpp` as a thin profile
header. The judgement, explicitly: a fork would have duplicated roughly 150 lines
of reader — header, four quadtrees, three string tables, two byte counts, two bulk
arrays, the object table and the map-info tail, all bit-identical — to avoid four
`if`s, and would have let the two copies drift the first time a bounds check was
tightened in one of them. The profile entry points (`ReadOprw18`, `ReadOprw20`,
`ReadOprw24`, `ReadOprw25`) each still refuse every revision but their own, so a
caller that means one generation is not silently handed another.

Accepted revisions are an explicit list, `{18, 20, 24, 25}`. **21, 22 and 23 are
deliberately refused.** The codec switch is documented at 23 and the other three
differences are only *bracketed* between 20 and 24 — no file of those revisions
was read, so accepting one would be a guess wearing a version check.

`PeekOprwModernRevision` replaces the per-revision probe chain at all four
dispatch sites (`Landscape::LoadOprwModern`, and `terrain census` / `objects` /
`render`).

## Measured census

```
sara.wrp          OPRW revision 20 (app id 0)
  land 512x512 @ 40.0 m   terrain 2048x2048 @ 10.0 m   extent 20480.0 m
  Elevation        -218.8 .. 757.1 m
  Below sea level  3,174,782 / 4,194,304 terrain cells
  Mountains        3,432
  Surface materials 5,126 declared, 5,125 distinct in use
  Placements       602,481
  Model table      893  (893 referenced, 0 unused)
  Static entities  5,101 (204 config classes)
  Roads            11,530 links, 73 segment models
  Map info         8,390,876 bytes retained, not decoded
```

| world | rev | placements | models | entities (classes) | road links (models) | materials (used) |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| sara | 20 | 602,481 | 893 | 5,101 (204) | 11,530 (73) | 5,126 (5,125) |
| sara_dbe1 | 18 | 604,652 | 919 | 4,955 (211) | 12,152 (78) | 5,125 (5,124) |
| saralite | 18 | 168,817 | 687 | 1,395 (78) | 5,793 (67) | 1,731 (1,730) |

Referenced-model census against an A1-only addon root (95 archives, 57,870
entries):

```
outcome                 models  placements
unsupported_revision       893      602481
TOTAL                      893      602481

ODOL revisions seen: 40 (893 models)
```

**Model resolution is 100 %.** Zero missing, zero unreadable, zero `not_p3d`,
zero `mlod_source`, zero `parse_failed`. The histogram has exactly one bar.

A note on how that number was obtained, because it is a trap: `--addons` must be
pointed at an **A1-only** root. Arma 1 and Arma 2 both use the `ca\` bank prefix,
so indexing `inspiration/officialarma` whole silently resolves A1 model paths to
A2 archives (first archive wins) and the census measures the wrong generation.
The local root is `packages/a1-compat/addons/`, 95 hardlinks, gitignored.

## Regression

All six previously-read worlds re-censused after the change, numbers unchanged:
Stratis 479 materials / 350 models / 163,953 placements; VR; Altis 14,731 / 963 /
1,779,908; Takistan 3,293 / 534 / 438,906; Chernarus 1,025 / 1,245 / 1,055,157;
Zargabad 1,351 / 406 / 93,516.

Unit tests: `[OPRW20]` and `[OPRW25]`, 17 cases, 148,591 assertions, all pass.
The synthetic revision-20 fixture uses 32×32 grids rather than the revision-25
fixture's 4×4, deliberately: at 4×4 every bulk payload falls under the 1024-byte
compression threshold and the fixture would pass **with the codec gate inverted**.
At 32×32 they all cross it, and the builder emits a legal all-literals LZSS
stream with its checksum. A `[GameData]` case covers all three real worlds and
skips where they are absent.

## What this does NOT claim

- No Arma 1 model loads. Every one of the 893 is ODOL revision 40, which has no
  reader; see the next section.
- A1 terrain **materials** are not implemented. The mask needs no new logic
  (WLD-019 amendment: A1's constant-255 alpha lands on the RGB group by
  construction), but the stage stride (3 vs 2), the first-stage offset (2 vs 4)
  and the `_lco` mask suffix are unwritten.
- The map-info block is retained as bytes and not decoded, as on 24/25.
- The randomization array is read and stored but nothing consumes it; the engine
  still regenerates its own. Whether feeding the authored values back in changes
  the look is untested.
- Which of revision 24's two terrain byte arrays A1's single one corresponds to
  is inferred from value distributions, not proved.
- Revisions 21, 22 and 23 are refused, not supported.
- No A1 world has been entered in the engine. `Landscape::LoadOprwModern` now
  accepts it, and WLD-014's one-grid workaround applies unchanged (terrain
  resolution for both grids), but nothing has been rendered or walked.

## Next blocker

**ODOL revision 40.** 893 of 893 referenced models, 602,481 of 602,481
placements. It is the only revision Arma 1 ships — 2,684 models across all 95 A1
archives in the corpus, revision 40 without exception, the single non-ODOL P3D
found being an MLOD source. It sits between OFP's 7 and Arma 2's 48, so it is a
genuine intermediate container and not a relaxed version check on the A2 family.
`DescribeOdolRevision` now names it (`Recognised`, "Armed Assault (Arma 1)") so
that an A1 census reads as a named, countable gap rather than as a corrupt file.
