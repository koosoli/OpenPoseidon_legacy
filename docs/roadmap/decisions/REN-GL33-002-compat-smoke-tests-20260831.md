# REN-GL33-002 — the compat smoke bullet now has tests, and they catch a regression the other 239,765 assertions do not (2026-08-31)

Follow-on to `REN-GL33-001-sole-renderer-gate-evidence-20260831.md`, whose gate table said
of bullet 2:

> "Arma 1/2/3/DayZ/Reforger compatibility smoke tests pass — **NOT SATISFIED — the tests do
> not exist.** … This bullet cannot be *failed* by a run; it has to be *built* first."

They exist now. One new file,
`tests/unit/engine/Poseidon/Asset/Compat/test_compat_smoke.cpp`, three test cases, 336
assertions, 8.7 s on this workstation. **No game was launched for this document** — another
agent held the single allowed instance, and none of this needs one.

## Verdict up front

**Gate bullet 2 goes from NOT SATISFIED to PARTIAL, and it is now a bullet a run can fail.**

What is now covered: the *asset-compatibility* half — the container readers for all five
generations, against the real corpora, with measured counts. What is still not covered: the
*rendering* half — nothing here proves a Takistan frame still draws on WGPU. That needs a
capture, and a capture needs the game. §5 says exactly what it would take.

Calling bullet 2 fully satisfied on this work would be dishonest. Calling it unsatisfied
would now also be wrong: the thing every hand-run compat capture in
`docs/roadmap/decisions/` silently depends on is under test for the first time.

## 1. What already existed (the inventory the task asked for)

### Evidence in the tree

There is a large body of recorded compat work and it is all hand-run: `WLD-010/011/012`
(Arma 3 worlds), `AST-007` (revision census over 545 PBOs), `AST-012A/B/C` (ODOL dispatch,
revision 73, revision 75), `AST-013` (Arma 1 revision 40, 18,697/18,697 LOD bodies),
`DZ-001` (DayZ is an Arma 2 descendant), `ARF-001/002` (Reforger `.pak`, `.xob`),
`MAT-030`, `COL-001`. Plus the memory notes `a1-odol40-harness`,
`takistan-oa-compat-harness`, `a3-world-compat-harness`, `dayz-compat-harness`,
`reforger-compat-harness`, `stratis-material-census-harness`.

### What of it was under test

Almost none. `tests/unit/engine/Poseidon/Asset/Formats/` is 54 files and is **synthetic
nearly throughout** — fixtures assembled byte by byte so they can live in the repo and run
on CI. Those tests pin what a format *is*. They cannot notice that a reader stopped reading
Chernarus, because no Chernarus is ever handed to them.

Two exceptions, and neither closes the gate:

* `test_oprw25.cpp`, bottom case — the one existing real-corpus test. Arma 3 only, and its
  assertions are deliberately *relational* ("landRange == landRangeY", "terrainRange is a
  multiple of landRange") rather than counted, so it passes on any revision-25 world.
* `test_odol40_arma1.cpp:565`, "ODOL 40 corpus survey" — gated on
  `POSEIDON_ODOL40_CORPUS`, and it ends in `WARN(...)`. It reports; it cannot fail. That is
  precisely the shape REN-GL33-001 objected to.

Every Enfusion test (`test_pak_archive`, `test_xob_model`, `test_ebin_world`,
`test_resource_database`, `test_xob_collision`) is synthetic. **Not one test in the repo
opened the installed Arma Reforger corpus.**

### Corpora on this machine

Both Steam libraries were checked, because assuming one drive is a documented past mistake
here (CLAUDE.md rule 0b).

| Generation | Where | State |
|---|---|---|
| Arma 1 | `D:\SteamLibrary\...\ARMA` (install), `packages/a1-compat/addons` (98 PBOs), `inspiration/officialarma/ALDP_A1_*` | present |
| Arma 2 / OA / DLC | `inspiration/officialarma/ALDP_A2_*`, `ALDP_A2OA_*`, `ALDP_A2OADLC_*` — the licensed ALDP corpus, **no Steam install** | present as archives only |
| Arma 3 | `D:\SteamLibrary\...\Arma 3` (+ Arma 3 Tools, Samples), `packages/a3-compat/world/{vr,stratis,altis}` | present |
| DayZ | `D:\SteamLibrary\...\DayZ` (133 PBOs), `packages/dayz-compat/world/{chernarusplus,enoch}` | present |
| Reforger | `C:\Program Files (x86)\Steam\...\Arma Reforger` — 16 `.pak`, `StateFlags 4`, `BytesDownloaded == BytesToDownload` | present, fully installed |

One gap was closed while doing this: `packages/a1-compat/world/` did not exist, so
`test_oprw20.cpp`'s own local case had been silently skipping since it was written. The
three Sahrani worlds were extracted from the A1 PBOs already in `packages/`:

```
python  # scratchpad pboscan.py, index-only PBO reader
sara.pbo      -> packages/a1-compat/world/sara/sara.wrp            (63,086,451 bytes)
saralite.pbo  -> packages/a1-compat/world/saralite/saralite.wrp    (17,996,504 bytes)
Sara_DBE1.pbo -> packages/a1-compat/world/sara_dbe1/sara_dbe1.wrp  (62,947,541 bytes)
```

`packages/` is gitignored; nothing from any corpus is committed by this change.

## 2. What the tests assert

### Case 1 — "every generation's world container parses to its measured shape"

Nine worlds across four generations. Per world: `PeekOprwRevision` names the revision
without disturbing the stream; the matching profile reads it; then

* **structural** — square grids, terrain a whole multiple of land, positive cell size,
  `elevation` sized `terrainRange²`, `geography`/`materialIndex`/`roadNet` sized `landRange²`,
  zero out-of-range model indices;
* **measured** — exact `landRange`, `terrainRange`, `landCellSize`, and exact **model,
  placement and terrain-material counts**.

The counted triple is the load-bearing part. Get one field width wrong early and the object
table no longer closes on its declared byte size; get the compression gate wrong and the
string tables come out as noise. Neither survives an exact count.

| Generation | World | OPRW | land | terrain | cell | models | placements | materials |
|---|---|---|---|---|---|---|---|---|
| Arma 1 | sara | 20 | 512 | 2048 | 40 | **893** | 602,481 | 5,126 |
| Arma 1 | saralite | **18** | 256 | 1024 | 40 | 687 | 168,817 | 1,731 |
| Arma 2 | chernarus | 24 | 512 | 2048 | 30 | 1,245 | 1,055,157 | 1,025 |
| Arma 2 OA | takistan | 24 | 256 | 2048 | 50 | **534** | **438,906** | 3,293 |
| Arma 2 OA | zargabad | 24 | 256 | 2048 | 32 | 406 | 93,516 | 1,351 |
| Arma 3 | stratis | 25 | 256 | 2048 | 32 | 350 | 163,953 | 479 |
| Arma 3 | altis | 25 | 1024 | 4096 | 30 | 963 | **1,779,908** | 14,731 |
| DayZ | chernarusplus | 29 | 256 | 2048 | 60 | 2,352 | **2,971,218** | 7,193 |
| DayZ | enoch | 29 | 256 | 2048 | 50 | 2,484 | 2,932,069 | 9,432 |

A finding fell out of writing it: **Sahrani and Sahrani Lite, two worlds of the same game,
do not share a container revision** — `sara.wrp` is OPRW 20 and `saralite.wrp` is OPRW 18. A
smoke test that had only ever been pointed at `sara.wrp` would never have learned that.

### Case 2 — "each generation's models are the revision that generation ships"

One representative archive per generation, walked with a small read-only PBO index local to
the test file, reading eight bytes from the head of every `.p3d`. Asserts: exact `.p3d`
count, **zero** entries off the expected revision, zero unreadable, and that the engine's own
`DescribeOdolRevision` resolves that revision to a named generation rather than `Unknown`.

| Generation | Archive | ODOL | `.p3d` |
|---|---|---|---|
| Arma 1 | `packages/a1-compat/addons/buildings.pbo` | 40 | 611 |
| Arma 2 | `ALDP_A2_PBOs_APL-SA_part1/buildings.pbo` | 48 | 340 |
| Arma 2 OA | `ALDP_A2OA_PBOs_APL-SA/structures_e.pbo` | 49 | 462 |
| Arma 3 | `Arma 3/Addons/structures_f.pbo` | 73 | 454 |
| DayZ | `DayZ/Addons/structures_industrial.pbo` | 54 | 318 |

The PBO index is deliberately a *second* implementation rather than a call into the engine's
FileBank machinery (which wants a mounted world and a running file server). When it and
`OdolRevision.hpp` agree about a histogram, two independent readers agree.

### Case 3 — "Reforger's whole pak corpus opens and closes"

Every `.pak` under the install's `addons/`, opened with `Enfusion::PakArchive`. Asserts all
four closure criteria **separately** (FORM size, chunk walk, directory consumption, DATA
tiling) plus zero gap bytes, zero overlaps, zero non-zero reserved fields — because each
catches a different class of mistake and `complete()` alone hides which one broke. Measured:
**16 archives, 222,566 entries**.

## 3. Where every expected number came from

Three kinds, annotated in place in the source and never mixed silently:

* **MEASURED** — read off the corpus on 2026-08-31 by the reader under test, and
  cross-checked against a standalone Python probe (`pboscan.py` + an OPRW header probe, in
  the session scratchpad) that shares no code with the engine.
* **RECORDED** — established earlier by a decision document or a memory note, cited by name,
  and re-measured here.
* **STRUCTURAL** — derived from the format definition, not from any one file.

**Four of the counts had an independent witness in the tree before they were measured here,
and all four agree exactly.** This is the strongest provenance available short of a third
implementation:

| Number | Prior witness | Measured here |
|---|---|---|
| Sahrani, 893 models | `test_odol40_arma1.cpp:4` — "every one of the 893 models sara.wrp references failed at the container" | 893 |
| Takistan, 534 models / 438,906 placements | memory note `takistan-oa-compat-harness` — "534 of 534 referenced models over 438,906 placements" | 534 / 438,906 |
| Altis, 1.78 M placements | `test_oprw25.cpp:396` — "Altis has 1.78 million of them" | 1,779,908 |
| Chernarus+, 2.97 M placements | memory note `dayz-compat-harness` | 2,971,218 |
| Reforger, 222,566 entries | `PakArchive.hpp:23` — "zero across all 222,566 entries in the local corpus" (ARF-001) | 222,566 |

The remaining counts (saralite, chernarus, zargabad, stratis, enoch; the five `.p3d`
histograms) had no prior witness. They are stated as first measurements, by two independent
readers, on the date above. **No number was chosen to make the test pass** — the saralite row
was written from a guess first, and the guess (397/156,212/1,290) was wrong and replaced by
the probe's 687/168,817/1,731 before the assertion went in.

## 4. Proof that the tests can fail — and that nothing else catches it

A test that cannot fail is what this whole exercise was about, so it was measured rather than
asserted. One character in the Arma 2/DayZ profile was mutated —
`Oprw25.hpp:478`, `hasPrimaryTextureIndex = version >= 24` changed to `>= 25`, which
mis-reads one per-terrain-cell byte array on revisions 24 and 29 — and the suite rebuilt.

**With that regression in place:**

| Run | Result |
|---|---|
| the entire pre-existing suite (`~[ren-gl33-002]`) | **2,995 cases, 239,765 assertions, 0 failures** — passes clean |
| the new compat smoke (`[ren-gl33-002]`) | **FAILS**, naming the generation and the file: `Arma 2 -- …\chernarus.wrp` / `OPRW 25 LZO payload did not terminate at the expected size` |

That is the finding in one line: **it was possible to break the Arma 2 and DayZ world readers
without a single one of the project's 239,765 assertions noticing.** The mutation was
reverted and the full suite re-run green.

Before / after, same machine, same commit lineage:

```
at f389fe00 (before the rebase, where the mutation experiment was run)
before   test cases: 2995 | 2975 passed | 20 skipped     assertions: 239,765 | 0 failed
after    test cases: 2998 | 2978 passed | 20 skipped     assertions: 240,101 | 0 failed

at 4c5e0a17 (after rebasing onto safety/asset-obs-001, 9 commits later)
before   test cases: 3005 | 2985 passed | 20 skipped     assertions: 240,319 | 0 failed
after    test cases: 3008 | 2988 passed | 20 skipped     assertions: 240,655 | 0 failed
```

+3 cases, +336 assertions, 8.7 s, on both lineages.

One tagging note that is easy to get wrong and would have made all of this
decorative: these cases are **not** tagged `[GameData]`. `OFPR_CATCH_TEST_SPEC` in
`tests/unit/engine/Poseidon/CMakeLists.txt` excludes `[GameData]` from the entire ctest
run whenever `packages/Remaster` is absent � and Remaster is absent on this machine,
which has all five corpora. Tagging them `[GameData]` (the obvious choice, and what
`test_oprw25.cpp`'s local case does) would have made the compat smoke silently never run
under ctest, which is the exact failure mode it exists to end.

## 5. What is NOT covered, and what it would take

**Do not read this file as "the five generations render".** It does not say that.

1. **No rendering.** Nothing here draws a pixel. Bullet 2's own wording ("compatibility
   smoke tests") sits inside a *renderer* gate, and the renderer half is untouched. Closing
   it needs a capture per family under `--render wgpu` and a reference image to diff against,
   which is REN-GL33-001's open question R5 and remains open. Note what is already recorded
   against it: `dayz-compat-harness` says Chernarus draws on GL33 and is **black on WGPU** for
   scale reasons (2.97 M placements, ~28 k textures exhausting the device). So bullet 2's
   rendering half is not merely untested, it is known-failing for one of the five.
2. **No materials.** `RvMaterialSource` (A1/A2/A3) and `EmatSource` (DayZ/Reforger) are
   tested only synthetically. A "material translator produces a bound base colour for each
   family" case is the obvious next slice and was cut for scope; the corpora for it are all
   present.
3. **Reforger has no world or model case here** — only the archive layer. `.xob` (15,809
   models) and `EBIN` worlds (103) are recorded closed by ARF-001/002 but are asserted only
   against synthetic fixtures. Adding them means decompressing entries out of a 2 GB `.pak`
   inside a unit test; the reader supports it, the runtime cost was not measured.
4. **CI covers none of it.** Every case skips without the corpora, and the corpora are
   licensed data that cannot be committed and are not on any runner. These tests protect this
   workstation and any other machine with the installs. That is a real limitation and it is
   the same one every compat claim in this project already has.
5. **Arma 2 has no Steam install here** — only the licensed ALDP PBO corpus under
   `inspiration/officialarma`. Anything needing a *running* Arma 2 install (there is nothing
   in this file) is not possible on this machine.
6. **Only one archive per generation** in case 2, not the whole corpus. Whole-corpus scans
   are what `AST-007`/`AST-013`/`DZ-001` did; repeating them per test run would cost minutes.
   The archives chosen are the largest structure sets, so an off-by-anything in the walk shows
   up, but a revision appearing in some *other* archive would not be seen.

## 6. Files

* `tests/unit/engine/Poseidon/Asset/Compat/test_compat_smoke.cpp` — new, 3 cases.
* `tests/unit/engine/Poseidon/CMakeLists.txt` — registers it.

Nothing else changed. No engine code, no shaders, no renderer, no deploy — this change
cannot alter a single pixel, which is why it needed no capture to land.
