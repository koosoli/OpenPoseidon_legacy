# ARF-001 — Reforger's `.pak` is IFF, and two of its asset layers are already implemented

**Date:** 2026-08-14
**Branch:** `dayz-compat-layer`
**Corpus:** `C:\Program Files (x86)\Steam\steamapps\common\Arma Reforger`, build `23758462`,
16 `.pak` files / 25.7 GB / **222,566 files**.

> **Corpus state.** The install was mid-update when this work began (`StateFlags 1030`,
> 0 of 4,040,610,192 bytes fetched). It completed during the session (`StateFlags 4`,
> `BytesDownloaded == BytesToDownload`, empty `steamapps/downloading`). **The whole census was
> re-run after the update settled and every number below is identical across both runs** — so
> nothing here was measured against a half-written archive. No Reforger data is in the repo; the
> extraction lives in a scratchpad.

## 1. The `.pak` container, closed exactly on 16/16

```
"FORM" <u32be size>       size + 8 == file size
"PAC1"                    form type
"HEAD" <u32be 28> ...
"DATA" <u32be n>  ...     payload blob; every file's bytes live here
"FILE" <u32be n>  ...     directory tree
```

The directory is a pre-order walk of a single **nameless root**:

* directory — `0x00 <u8 nameLen> <name> <u32le childCount>`
* file — `0x01 <u8 nameLen> <name> <24-byte record>`

and the record is

| field | width | note |
|---|---|---|
| offset | u32le | **absolute file offset**; the first entry is 56, the DATA data start |
| storedSize | u32le | bytes occupied in DATA |
| realSize | u32le | after decompression; equals storedSize when stored |
| reserved | u32le + u16le | zero across all 222,566 entries — checked, not assumed |
| flags | u8 | 1 compressed, 0 stored |
| method | u8 | 6 = zlib when compressed, 0 when stored |
| stamp | u32le | |

**Only two `(flags, method)` pairs exist in the entire corpus: `(0,0)` and `(1,6)`.** There is no
third codec to discover.

### Closure

Four independent criteria, all of which must hold, on every archive:

1. `FORM` size + 8 == file size
2. the chunk walk lands exactly on EOF
3. the directory tree is consumed exactly — no trailing bytes, no truncation
4. the entries' extents **tile the DATA chunk with zero gaps and zero overlaps**

**16 / 16 archives pass all four.** 222,566 files, 12,763 directories. Criterion 4 is the one that
would catch a wrong field width that criteria 1–3 tolerate.

A fifth, independent check: extracting all 10,312 `.emat` produced **10,631,067 bytes**, matching
the directory's declared uncompressed total for that extension exactly.

## 2. What the corpus is made of

| ext | files | uncompressed | |
|---|---|---|---|
| `.ntile` | 93,337 | 10.5 GB | nav tiles |
| `.edds` | 29,026 | 14.1 GB | textures |
| `.et` | 19,448 | 33 MB | entity templates |
| `.wav` | 19,291 | 4.0 GB | |
| `.xob` | 15,841 | 4.1 GB | **models** |
| `.anm` | 12,623 | 258 MB | |
| `.emat` | 10,312 | 10.6 MB | **materials** |
| `.c` | 6,568 | 29 MB | Enforce script |
| `.ttile` | 5,282 | 215 MB | terrain tiles |
| `.conf` | 2,610 | 30 MB | |
| `.layout` | 1,239 | 4.8 MB | UI |
| `.ent` | 103 | 169 MB | **worlds** |

## 3. Enfusion has exactly two container idioms, and we implement one of them

**Everything binary is IFF `FORM`**, the same walker `.pak` needs — and the same family DZ-001
found behind DayZ's `FORM`/`ANIMSET5`. Sampled and walked with the identical code:

| ext | form type | chunks | sampled | closes |
|---|---|---|---|---|
| `.xob` | `XOB9` | `HEAD` `LODS` `VOLM` `COLL` `BSP3` `PRB2` | 938 | **938** |
| `.ttile` | `TERR` | `VERS` `HGHT` `TMAT` `LRS2` `BERR` `GCTD` | 262 | **262** |
| `.ntile` | `NAVD` | `TIHE` `DATA` | 480 | **480** |
| `.smd` | `SMDF` | `VERS` `HEAD` `TMAP` | 1 | 1 |

**1,681 / 1,681 close exactly.** `.ttile`'s `HGHT` chunk is the terrain heightfield; `.xob`'s
`LODS` is the model's LOD table. Neither is decoded yet, but neither needs a new container.

**Everything textual is the `.emat` brace syntax** — and that reader already exists.

## 4. The already-implemented layers, measured

### `.emat` — 10,308 / 10,312 (99.96 %), unmodified

The DayZ `.emat` reader (`Asset/Formats/Material/EmatSource.hpp`) was pointed at Reforger's entire
material corpus with **no changes**: **209,906 properties, 87,290 texture references**.

The **4 failures are malformed input, not a format gap.** All four (`Eyes.emat`,
`Hat_Knitted_01_{blue,brown,red}.emat`) genuinely end after their last property with **no closing
brace**; the reader's "no closing brace" is a correct diagnosis of shipped data. Effective closure
over well-formed files is **100 %**. (CRLF was the first suspect and is wrong: 789 files contain
CRLF and 785 of them parse.)

**The real gap is inheritance.** 1,891 of 10,311 files (**18.3 %**), across **547 distinct
parents**, use

```
MatPBRSkinProfile : "{3D8DD71AB39D2267}Assets/_BaseMaterials/Characters_MatPBRSkinProfile.emat" {
```

The reader parses the body but folds `: "` into the **class name** — the census reports
`MatPBRBasic` 211 times and `MatPBRBasic : "` 249 times as if they were different families — and
**drops the parent reference entirely**, so an inheriting material silently loses every property its
parent defines. This is the single highest-value fix in the material layer.

Family distribution, once the class name is split correctly: `MatPBRMulti` 4,170 and `MatPBRBasic`
3,568 are **75 %** of the corpus; then `MatPBRDecal` 636, `MatPBRTreeCrown` 467,
`MatPBRSkinProfile` 213.

### The same parser also reads `.et`, `.conf`, `.layout` and `.gamemat` — 1,177 / 1,177

A stratified sample of all four extensions, renamed so the existing census would walk them,
**parsed 100 %** and yielded 9,533 texture references. That extends one already-shipped parser over
`.emat` + `.et` + `.conf` + `.layout` + `.gamemat` = **33,800 files**.

*Caveat, not yet measured:* "parses without error and recovers texture references" is not the same
as "models the nested entity tree faithfully". The sample averaged 3 top-level properties per file
against `.emat`'s 20, which is consistent with deep nesting the flat census does not walk. Verify
structural fidelity before relying on it for `.et` prefabs.

### `.edds` — the container reads; the codec is the wall

The DayZ `.edds` reader was pointed at a **stratified 1,692-file sample** spanning 13 archives.
**118 decoded (7.0 %)** — and that number is much better news than it looks, because **1,570 of the
1,574 failures are raised after the container is parsed**, at the pixel format:

| reason | n | share of failures |
|---|---|---|
| BC7 (DXGI 98 / 99) | 1,372 | **87.2 %** |
| BC4 (DXGI 80) | 122 | 7.8 % |
| BC1 (DXGI 72) | 34 | 2.2 % |
| BC5 (DXGI 83) | 30 | 1.9 % |
| DXGI 91 / 93 | 6 | |
| pfFlags 0x40, 8 bpp | 5 | |
| raw level-0 size 16× expected | 2 | |
| FourCC `'p'` / `'$'` | 2 | |

Three consequences:

* **BC7 alone is 87 % of the gap.** DayZ needed DXT1/DXT5/ARGB8888 and nothing else; Reforger is a
  BC7-era corpus. One decoder moves 7 % → ~88 %.
* **BC1 (DXGI 72) and BC3 (DXGI 78) fail although DXT1 and DXT5 decode fine** — those are the *same*
  block formats reached through the DX10 header instead of a FourCC. The decoders already exist; only
  the DXGI-enum → decoder mapping is missing. That is pure plumbing and a free 35 files in the sample.
* Adding BC7 + BC4 + BC5 + the BC1/BC3 mapping reaches **~99.6 %** of the sample.

The two "level 0 stored raw as 16384 bytes, 16×16 requires 1024" are a 16× discrepancy — a cubemap
or array/volume texture is the obvious guess, and it is a guess, not a measurement.

## 5. Everon is `worlds/Eden/Eden.ent`

"Eden" is Reforger's internal name for Everon. Its world directory holds 10,385 entries:

| file | size | format |
|---|---|---|
| `Eden.ent` | 70.2 MB | **`EBIN`** — Enfusion binary; magic `EBIN`, u32 version 1, then a table of **u16-length-prefixed strings** (`GenericWorldEntity`, `BSP`, `boundMins`, `boundMaxs`, `blockSize`, `GenericTerrainEntity`, …). It is the binarised form of the same brace syntax `.emat` uses. |
| `Eden.smap` | 16.4 MB | heightfield-ish; see below |
| `Eden.topo` | 10.3 MB | **`TOPO`** — version 1 then absolute near-EOF offsets, with a named `ROAD` section (version 2). A section table, not IFF. |
| `Eden.smd` | 5.0 KB | **IFF `FORM`/`SMDF`** — `VERS`/`HEAD`/`TMAP`. Closes exactly. |
| `Eden_SMD.conf` | 454 B | plain brace text — shore-map generation parameters |
| `*.ttile` | 2,500 | `FORM`/`TERR`, with the `HGHT` heightfield chunk |
| `*.edds` | 7,584 | 542 MB |

`Eden.smap`'s header decodes cleanly and **self-checks**: grid `1275 × 1246`, cell size `10.0` m,
origin `(-100, -100)`, and a bound of `12650.0` on one axis — and `1275 × 10 == 12750 == 12650 −
(−100)` **exactly**, with the other axis agreeing to 1246 × 10. So Everon is a 12.75 × 12.46 km
world on a 10 m grid. The rest of the file (16.4 MB against 1,588,650 cells) is **not** yet
accounted for — a per-tile index table of `u32 index / 0xFFFFFFFF` pairs is visible after the header
— and no byte-accurate parse of `.smap` should be claimed until it closes.

## Order of work this implies

1. **BC7 decode** — 87 % of the texture gap in one codec.
2. **The DXGI→decoder mapping for BC1/BC3**, and BC4/BC5 — cheap, and takes the sample to ~99.6 %.
3. **`.emat` inheritance** — 18.3 % of materials currently lose their parent silently, and the class
   name is wrong for all of them.
4. **`.xob` `LODS`** and **`.ttile` `HGHT`** — the container is already open; only the chunk payloads
   need decoding.
5. `EBIN` string table → `Eden.ent`.

Note that 1–3 are all *inside* code this repo already ships, which is why "the texture and material
layers may largely already work" was the right first experiment to run.

## Tooling

`pak.py` / `pakx.py` in the session scratchpad — the IFF walker, the directory parser, the four
closure checks, and an extractor. **Not committed**: they are throwaway measurement scaffolding, and
a C++ home for the container belongs with the reader that will consume it, not in a scratch script.

**Harness note:** the session scratchpad path is 152 characters, so extracting Reforger's deeper
asset paths into it silently trips `MAX_PATH` — 16 of the first 1,200 `.emat` reported "unreadable
file" for that reason alone. Extract to a short root. The 99.96 % above is from the short-root run.
