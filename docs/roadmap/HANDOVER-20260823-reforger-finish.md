# Handover — finish Arma Reforger's Everon

**Date:** 2026-08-23
**Branch:** `feat/consolidated` (pushed to `safety`)
**Installed build:** whatever `DEPLOYED-FROM.txt` says — **read it before you do anything else.**

---

## READ THIS FIRST — the mistake that cost the previous session five hours

Several worktrees deploy into **one** game folder, and `wgpu_renderer.dll` is shared by **every**
`.exe` in it. On 2026-08-22 a session deployed a DLL built from a branch **152 commits behind** the
installed one, over a matched pair. Nothing failed. Roads stopped going dark at night, house
textures came apart, and vegetation went white — and all three were written up as fresh rendering
regressions, with shader ablations run to explain them, before anyone compared file dates.

**The ABI check does not catch this.** It compares an ABI version and a struct-layout hash, both of
which move only when a struct moves. A DLL 150 commits stale passes it cleanly.

Before you measure anything, and before you deploy anything:

1. Read `D:\SteamLibrary\steamapps\common\ARMA Cold War Assault\DEPLOYED-FROM.txt`. **First file you
   open**, not the one you reach for after a failed diagnosis.
2. `git merge-base --is-ancestor HEAD <installed-commit>` — if it succeeds, your build is *behind*.
   `git rev-list --count HEAD..<installed-commit>` says how much.
3. The exe and the DLL must have **matching file times**. Mismatched-but-loadable is the failure
   mode and there is no error.
4. Deploy **only** via `scripts\Deploy.ps1`. It enforces 1–3 and stamps the file. A hand-rolled
   `Copy-Item` does none of it.
5. If a capture disagrees with the source, **suspect the binary before the code**.
6. Every log now prints `Wgpu: build provenance — exe <sha>, wgpu_renderer.dll <sha>`. Quote it
   next to any number you report.

This is also `CLAUDE.md` rule 0. Four more traps that each cost real time last session:

- **`--test-world-freefly` takes `X Z Y` (altitude LAST)** but the log prints `(x, altitude, z)`.
  A pose copied from a log must be transposed.
- **`cp -p` in PowerShell** is `Copy-Item`, where `-p` is ambiguous. Say which shell you mean.
- **Resolve a merge conflict by reading the whole file, not the conflicting hunk.** Dropping four
  declarations whose *uses* auto-merged elsewhere cost twelve compile errors.
- **A guard that cries wolf gets ignored.** Two guards written last session both false-positived on
  their first run. Test the *negative* case before you ship a check.

---

## The task: Everon is missing most of its buildings

Owner report, and it reproduces: at freefly `4914.01 3989.78 91.31`, azimuth `90.2`, elevation
`-24.3` there should be a town. There are a couple of houses.

Glow and white trees are **fixed** (MAT-050's grey floor plus the consolidated branch). Terrain,
materials, roads and sky look good. What is missing is buildings.

### What is already measured — do not re-measure this

| | |
|---|---|
| objects placed in `ev_DALL.wrp` | **1,216,138** |
| — vegetation | 824,896 (67.8%) |
| — rocks | 307,127 (25.2%) |
| — **structures** | **66,303 (5.5%)** |
| — props / vehicles / decals / terrain / items | 17,748 |
| **distinct models the world references** | **1,123** |
| — of which structures | **695** |
| distinct models present on disk | **1,123 / 1,123 — none missing** |
| `.xob` models in the Reforger corpus | **15,809** |

Read those last three rows together: **every model the world asks for is deployed, and the world
only asks for 1,123 of 15,809.** So this is not a broken model path and not a failed export of the
models that *are* referenced. The placements themselves are thin.

### The prime suspect

The world was built with `--max-models 5000` (see
`HANDOVER-20260815-reforger-renders.md`):

```
PoseidonTools xob export "<Reforger>\addons" -w worlds/Eden/Eden.ent -o C:\...\rf ^
  -m C:\...\rf\everon_placements.txt --resmap-cache C:\...\rf\resmap.tsv --max-models 5000 --textures
```

The hypothesis to test first: **placements whose model was not in the exported set are dropped from
the `.wrp` rather than kept as dangling references.** That would explain 695 distinct structure
models carrying 66,303 placements across a 12.8 km island — far too few kinds of building for
Everon, and exactly what "a couple of houses where a town should be" looks like.

Note also `everon_placements.txt` in `%TEMP%\rf` contains **only vegetation and rocks (19,646 rows,
zero structures)** — it is *not* the manifest `ev_DALL.wrp` was built from. Find the one that was,
or regenerate.

### Suggested order

1. **Confirm the drop.** Count structure placements in the Reforger source world (`EBIN` /
   `worlds/Eden/Eden.ent`) and compare with the 66,303 in the `.wrp`. If the source has far more,
   the export is dropping them and the cause is `--max-models`.
2. **Re-export without the cap** (or at 20,000), rebuild `ev_DALL.wrp`, redeploy the model tree.
   Expect this to be slow and large — budget for it.
3. **Re-capture at the owner's coordinates** and count visible buildings before/after.
4. **Only then** look at the object streamer. `WGR_OBJECT_STREAM_MAX_OBJECTS` was 400,000 for the
   captures above and vegetation is 68% of placements, so a budget that fills with bushes before it
   reaches structures is a *second*, independent possibility — but it cannot explain models that
   are not in the world file at all. Establish (1) before spending time here.

### Also open on Reforger — `.emat` materials

**1,275 of 1,299 materials the renderer touched (98.2%) resolve with no albedo, no normal map, no
specular map and an empty shader family.** `TranslateEmat` reads DayZ's `AlbedoMap` / `NormalMap`
keys, while only ~2.2% of 10,312 real Reforger `.emat` files name any key it reads — the real ones
are **`BCRMap`, `NMOMap`, `MaskMap`**. MAT-050 puts a neutral grey floor under the symptom so it no
longer blows out white; the cause is untouched. This is the difference between "Everon renders" and
"Everon looks right".

---

## Other open items

**DayZ furniture is still missing.** MAT-048 fixed one real cause — `GetShapeName` was prefixing
the OFP model folder onto addon-rooted proxy paths, so `dz\structures\furniture\beds\...` became
`data3d\dz\...` and failed. That fix is in the deployed build and DayZ textures are confirmed good
again, but interiors are still empty, so **MAT-048 was not the whole story**. Next step: pick one
house model at the owner's spot `1958.50 2272.82 8.95` az `232.9` el `-14.8`, run
`PoseidonTools model selections <p3d> --proxies`, and check whether its furniture proxies resolve
and are drawn. Note that A2-era house models list interior furniture as `\ca\...`-style proxies
*with* a leading backslash, which never hit the MAT-048 bug at all — so the failure may be in the
retained draw path, not in path resolution.

**`codex/a2oa-takistan-compat`** — 5 commits (Arma 2 OA worlds, ODOL 49/50/52), the only branch not
consolidated. The merge hits 5 conflicts in `ModCollection`, `AppConfig`, `LandSave`, `Landscape`
and `test_odol_revision`. Self-contained with its own tests.

**MAT-049** — helicopters render translucent on WGPU and solid on GL33. Root-caused to WGPU turning
a texture's alpha histogram into a real blend state while GL33 uses the section's authored spec;
951 of 4,184 CWA textures (22.7%) classify BLEND. Not fixed, because the obvious fix reroutes 22.7%
of the corpus and aircraft canopies legitimately sit in that set. Needs a discriminator, measured.

**The `openposeidon` rename is parked** by owner decision, along with its icon. Adding a second exe
name to a folder several worktrees already share widened the ambiguity the guards exist to close.

---

## Where things are

- `feat/consolidated` — the trunk. Superset of all eight branches plus both sessions' previously
  uncommitted work. Pushed to `safety`.
- `wip/preserve-20260822` — the pre-consolidation working tree, two commits split by author.
  Pushed to `safety`. Nothing was lost; if something looks missing, look here first.
- Capture harness: `%TEMP%\claude\...\scratchpad\cap.ps1` (detached `cmd /c start`, never redirect
  stdout/stderr — that kills the renderer with os error 232).
- Reforger world + assets: `%TEMP%\rf\`. **No Reforger or DayZ data in the repo, ever.**
