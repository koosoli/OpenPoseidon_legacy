# WLD-011 — What a real Arma 3 world actually asks the model reader for

- **Ticket:** WLD-011
- **Date:** 2026-08-10
- **Depends on:** WLD-010 (OPRW revision 25 reader)
- **Scope:** resolution and parsing of the models a world references. No rendering,
  no materials, no instantiation.

## Result

Stratis, resolved against the local Arma 3 install (487 archives, 503,887 indexed
entries) and parsed with the existing revision-73 reader:

| outcome | models | placements |
| --- | ---: | ---: |
| static | 304 | 163,059 |
| parse_failed | 46 | 894 |
| **total** | **350** | **163,953** |

**99.45% of placements** and **86.9% of distinct models** reach a parsed static
model. Nothing is missing, unreadable, MLOD, or of an unsupported revision: all
350 are ODOL revision 73, and all 350 are present in the install.

Both denominators are reported because they answer different questions. 304/350
looks like a gap; 163,059/163,953 says the gap is in the long tail, not the bulk
of the world.

## The whole remaining gap, by what would unlock it

| missing capability | models | placements |
| --- | ---: | ---: |
| animations declared in the model directory | 35 | 519 |
| proxies in a LOD | 5 | 351 |
| vertex bone references in a LOD | 6 | 24 |

These are the reader's own refusals, not crashes. `Odol73` throws a named error
rather than guessing at a section it does not implement, which is why the census
can attribute every one of the 46 failures to a specific unimplemented feature.

The ordering is the useful part. Animated statics — doors, containers, wind socks,
runway lights — are the single largest category by both measures, and are the
natural next piece of work. Proxies affect only five models but 351 placements
(fences and slum walls, which repeat along a boundary). Vertex bone references
affect 24 placements and can wait.

## Note: 350 of 350 resolve only if the archive prefix is honoured

An Arma 3 archive stores its entries relative to a `prefix` property in its
header-extension block: `a3\rocks_f\water\w_sharpstone_01.p3d` is on disk as
`water\w_sharpstone_01.p3d` inside `rocks_f.pbo`, whose declared prefix is
`a3\rocks_f`. Measured directly: ignoring the prefix resolves **0 of 350**
Stratis models; applying it resolves **350 of 350**.

`QFBank::GetPrefix()` is *not* this value — it is the mount point the caller
assigned, and is empty for a bank merely opened for inspection. The archive's own
declaration is `GetProperty("prefix")`. `PboInfo` now carries it, because an entry
list without it cannot be resolved against later-generation content.

**Follow-up, not yet investigated:** `BankList::Load` derives a bank's mount prefix
from the bank *filename* plus a caller-supplied prefix, never from the archive's
declared property. For OFP-era archives those coincide. For Arma 3 they do not
(`rocks_f` vs `a3\rocks_f`). Whether that produces a wrong mount at runtime depends
on what the caller passes, and has not been tested against a live A3 addon load —
the census sidesteps the mount path entirely by indexing archives directly. This
should be settled before any runtime A3 asset loading is attempted.

## What "static" does and does not mean

It means `ReadOdol73StaticModel` consumed the file without error, with the reader's
existing internal size checks satisfied. It is a parse claim only. It does not
assert that the geometry is correct, that its materials resolve, that it builds
renderer vertex buffers, or that it draws. Those are separate measurements and are
absent from this census rather than assumed.

## Method

`PoseidonTools terrain census <world> --addons <install>`:

1. Walk the install for `*.pbo`; read each archive's header and declared prefix.
2. Build `prefix + entry` → archive index (first archive wins).
3. For each model the world references, read its bytes from the archive, check the
   container signature, peek the ODOL revision, and attempt a full parse.
4. Report by distinct model and by placement count, with a complete histogram of
   blocking reasons.

`--json` emits the same numbers machine-readably, so this can become a regression
metric rather than a one-off.

## Known limitations of the measurement

- One world. Altis is ~4x the placements and has not been run.
- The index takes the first archive providing a path. Arma 3's official set has no
  conflicts that matter here, but a mod list would need real load-order rules.
- Models referenced only by the road network or by static-entity records are not
  in the 350; those tables are counted separately by the world census.
- Textures and RVMATs referenced *by* the models are not followed.
