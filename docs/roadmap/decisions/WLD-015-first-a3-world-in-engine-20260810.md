# WLD-015 — First Arma 3 world running in the engine

- **Ticket:** WLD-015
- **Date:** 2026-08-10
- **Depends on:** WLD-010, WLD-012, WLD-013, WLD-014

## What now happens

Stratis loads into `Landscape` and renders through the WGPU backend. From the run log:

```
LoadOprw25 stratis.wrp: land 256 terrain 2048 ratio 8 cell 4m extent 8192m
LoadOprw25 stratis.wrp: 163953 objects created, 0 refused, of 163953 placements
--test-world-pos: moved player to 1699, 7.3, 5053
```

Terrain geometry is built from the world's own 4.2 M elevation samples, the camera
stands on it at a requested world coordinate, and the frame renders without error.

## The adapter, and that it is an adapter

WLD-014 recorded why this could not work: `Landscape::Dim` asserts the terrain grid
equals the land grid, and Arma 3 worlds have two.

This mounts the world at **terrain** resolution and hands Landscape a single grid.
A land cell's geography applies to every terrain cell inside it, so replicating its
value across the 8×8 (Stratis) or 4×4 (Altis) block it covers is what the format
means rather than fabricated data, and elevation keeps its authored detail because
it was already at terrain resolution.

**This is explicitly a temporary compatibility bridge, not the target terrain
architecture.** It is confined to `Landscape::LoadOprw25` so it can be removed. Do
not spread the single-grid assumption into new systems, and do not treat the
expanded grid as the canonical A3 representation. The target remains genuine
multi-resolution support with explicit grid descriptors and World↔Land↔Terrain
conversions, replacing implicit `x & LandRangeMask` arithmetic where the spatial
domain of `x` is unstated. That is core terrain-data modernization and is
independent of the far-terrain HOLD, which this does not touch.

## Geography is translated, not copied

The two 16-bit layouts differ: Arma 3 carries a min *and* a max water depth where
the engine has one, and the engine has `forestInner`/`forestOuter`/`track`/`slow`
where Arma 3 has a single forest bit and no equivalent for the rest. Blitting the
word across would land a max-water-depth in the engine's forest and road bits.
Fields with no counterpart are left zero rather than guessed. Geography drives AI
pathing, so a wrong bit is a wrong path; none of it affects whether the world draws.

## A latent renderer crash this exposed

`TerrainWgpu::UploadGeography` dereferenced `land.GetTexture(i)` without a null
check when building its grass-layer name list, while the same call forty lines
earlier is guarded. Any world whose terrain layers carry no texture reaches the
"no named grass" fallback, which enables *every* layer including empty ones, and
the next line takes the renderer down inside `DrawGround`:

```
UNHANDLED EXCEPTION 0xC0000005
  #00 TerrainWgpu::UploadGeography+0x9f5
  #01 TerrainWgpu::UploadIfNeeded
  #02 TerrainWgpu::DrawTerrain
  #03 Landscape::DrawGround
```

Fixed. This is a pre-existing null-safety bug, not an A3-specific one — an OFP
world with an unresolvable ground texture would have hit it identically.

## What does NOT work yet

The world renders **white**. Two causes, both unresolved:

1. **Terrain materials do not resolve.** `0 of 479 terrain materials resolved to a
   satellite texture`. Each RVMAT is parsed and its satellite `_co` layer taken as
   the cell texture, but the files never open — the A3 archives are not mounted in
   the running game.
2. **Object models do not resolve, for the same reason.** "163,953 objects created"
   means `ObjectCreate` returned an Object for every placement; it does *not* mean
   their P3Ds loaded. With no A3 banks mounted they are empty shells. This count
   must not be read as a rendering result.

An attempt to mount a targeted subset via `-mod=@a3stratis` (12 hard-linked
archives: map_stratis_data, the layer set, roads_f, structures_f, rocks_f,
plants_f and related) changed nothing visible, and the run was made at a log level
that does not print the bank-mount lines, so **whether those banks mounted at all
is unverified**. That is the next thing to establish, and WLD-013's note about the
`AutoBank` de-duplication check still testing the filename-derived key is a
candidate cause.

Even fully mounted, the terrain material reduction is lossy by construction: one
satellite layer per cell, with the mask and detail layers dropped.

## New flags, both dev-only

- `--test-world <path.wrp>` — substitutes the landscape the mission asked for. The
  only way to reach an A3 world today: they are named by `CfgWorlds` entries this
  build has no configs for, so nothing would ever route to them.
- `--test-world-pos X Z` — moves the player once gameplay starts. A mission written
  for one world starts at coordinates meaningless in another; the OFP training
  start is open sea on Stratis.

## Regression status

The OFP mission capture smoke passes and its capture was opened and compared —
terrain, buildings, foliage, grass and shadows render identically to before. The
`--test-world` path is gated on the flag being set, so the OFP load path is
untouched.

## Next

1. Verify whether `-mod` actually mounts the A3 archives; fix if not.
2. Then re-measure: how many of 479 terrain materials and 350 models resolve at
   runtime, as a census like WLD-011's rather than a screenshot.
3. Then plan the multi-resolution `Landscape` migration in measured steps.

## Addendum — why nothing resolves (partly answered)

Traced the "0 of 479 terrain materials resolved" to bank mounting, and found the
first gate. Every addon bank passes `CheckProductCallback` →
`IsAddonMetadataAccepted`, which refuses any archive whose declared `product` is
not in `ProductList`. That list was:

```cpp
{"OFP: Cold War Crisis", "OFP: Resistance", "VBS", nullptr}
```

Measured across the local Arma 3 install: **485 archives declare `Arma 3` and one
declares `ArmA 3`** — so all 487 were refused at mount. (`StringInList` compares
case-insensitively, so one entry covers both spellings. No archive anywhere sets
`pboVersion`, so that gate in the same function is inert.) `Arma 3` is now
accepted, since loading Arma 3 content is the point of the work.

**This was necessary but not sufficient.** With the product gate open, the count is
still `0 of 479`. The mod directory is confirmed being scanned
(`-> Checking mod directory: ...\@a3stratis`), and `map_stratis_data_layers.pbo`
is confirmed to declare prefix `a3\map_stratis\data\layers` and to contain exactly
the RVMAT paths the world names. So something after the product check is still
dropping these banks.

**Unverified hypothesis for the next session:** addon banks are loaded with
`afterOpen = ParseAddonConfigCallback`, and `BankList::Load` calls
`Resize(oldSize)` — dropping the bank — when that callback returns false. An Arma 3
`config.bin` is unlikely to parse through CWA's `ParamFile`, which would discard
every A3 bank *after* it opened successfully. That is a guess from reading the
code, not a measurement; confirm it before changing anything. The `AutoBank`
de-duplication note in WLD-013 remains a second candidate.
