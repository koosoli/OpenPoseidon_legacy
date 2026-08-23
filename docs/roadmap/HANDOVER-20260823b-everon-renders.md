# Handover — Everon renders; finish the VertexIndex widening without breaking OFP

**Date:** 2026-08-23 (afternoon)
**Trunk:** `feat/consolidated`, pushed to `safety`. Read `git log e12efc7c..` for the session.
**Installed:** binaries `5595e278` + the `b175a60a` revert; Reforger tree + world from the `rf8` export
**Previous:** [HANDOVER-20260823-reforger-finish.md](HANDOVER-20260823-reforger-finish.md) — its
headline diagnosis was wrong, see below.

---

## READ THIS FIRST — rule 0c is new, and it is about you, not the binary

`CLAUDE.md` rules 0 and 0b already stop you measuring a stale binary and stop you leaving the
owner on one. **0c stops you starting from a stale branch**, which is the failure that costs
most, because its price is a hand merge rather than a re-run.

```powershell
.\scripts\Use-RepoHooks.ps1        # once per clone. Sets core.hooksPath, upstream, prints ahead/behind
git pull --rebase safety feat/consolidated   # at the START of a session
```

A `pre-commit` hook now REFUSES a commit onto a branch that is behind its remote and prints the
exact fix; `pre-push` does the networked version. `SKIP_BRANCH_GUARD=1` bypasses both, and
reaching for it is the signal to rebase instead. Verified in all three states (refuses when
behind, allows when current, escape hatch announces itself).

The mirror carries **29 branches**. Trunk is `feat/consolidated` and almost everything else is
history. `codex/a2oa-takistan-compat` is the one branch with real unmerged work (5 commits).

Two traps that cost time today and are now written into the rules:

- **`git reset --hard` with staged-but-uncommitted NEW files deletes them**, from index and
  working tree, with no reflog entry — content never committed cannot be recovered. I destroyed
  the hook files that way minutes after writing them. Commit first, then experiment.
- **`ColdWarAssault.exe` is the retail binary and is never touched by `Deploy.ps1`**, so it only
  gets older while sitting next to ours and sorting first alphabetically. A fix was reported as
  not working from a binary a day behind. `Deploy.ps1` now prints the age gap.

---

## What was actually wrong with Everon — five bugs, all shipped

The previous handover's prime suspect (`--max-models` dropping placements) was **wrong**, and
re-exporting to chase it would have cost hours for nothing. Every model the world asks for was
always present. What follows is what was really happening, in the order found.

### 1. Terrain-relative Y — placements written at sea level

Houses sat a median **29 m below terrain** while vegetation, rocks, walls and props sat on it.
The `.ent` genuinely stores `coords` Y ≈ 0 for them; Enfusion resolves it at load. The
discriminator is the `Flags` property, measured over 1,228,065 in-bounds placements:

| | n | within 0.5 m of terrain | median Y |
|---|---|---|---|
| `Flags` present | 872,185 | **96.3%** | 54.99 |
| `Flags` absent | 355,880 | 15.4% | **0.00** |

`Tree` alone splits 97.9% / 7.4% on the same test, so it is a property of the file, not of one
class. Rule: no `Flags` **and** |Y| < 2 → Y is an offset above terrain. The `|Y| < 2` guard is
load-bearing — `GenericEntity` is 99.9% flagless and its Y IS absolute.

### 2. `VertexIndex` was 16-bit — buildings rendered as a roof with no walls

`ShapeAdapter` rejected every face indexing past 32,767. In one frame **30 of 771 models** lost
LOD 0 geometry: `church_01` 140,041 faces, `house_village_e_1l03t` 44,154. Now `int32_t`, and
the wgpu index slice went `u16 → u32` with it — the geometry pool was always `Uint32` and merely
widened on upload. **Both had to move together**: widening only the first trips the second and
the model drops off the retained path entirely.

Watch the old clamp: `numeric_limits<VertexIndex>::max() + 1` is fine at 16 bits and **overflows**
at 32, which would reject every face in the world. It is `int64` now.

### 3. Nested entities' coords are local to their parent — 671 objects on the map origin

4,186 placements (0.34%) are entity blocks inside another entity block. Read as world
coordinates they collapsed onto the origin, including **128 houses**. Composition is applied when
the child block opens, which is safe: all 4,186 have their parent's `coords` already consumed by
then.

### 4. `resourceDatabase.rdb` was never read — 13,449 placements resolved to nothing

We rebuilt Enfusion's GUID→path map by text-scanning `.et`/`.emat`/`.conf` (51,452 GUIDs) and
structurally could not find one no text file mentions. Each addon ships the real index beside its
`.pak`: **110,860 entries**. That is where the owner's church was.

Format, established by byte accounting — both files close exactly (core 81,645/81,645, data
11,509,935/11,509,935):

```
u32 nameLen; char name[nameLen] NUL-terminated; u16 type; u32 flags;
u8 guid[8] little-endian;  u8 extra[8] ONLY WHEN type >= 6
```

That last clause is the whole trick: types 4/5 (directories) are 14 bytes past the name, 6/7/22
are 22. Assume one stride and the walk desynchronises a few hundred records in and then emits
**well-formed paths under the wrong GUIDs**. The reader reports `consumed`; the loader refuses a
database that does not close. The header before the table is not decoded and differs between the
two files, so the start is *found* by requiring closure, not hardcoded.

### 5. `autocenter` sinks every model by its bbox centre — and `.xob` orientation is inverted

Both were already root-caused in `28764db4`, whose own last line is **"Nothing built, nothing
run."** Two flags existed, default-off, never once exercised. Today was the first run of either.

- **`--fix-origin`** (fact 7). **1,083 of 1,248 models (86.8%)** were sunk, median 0.50 m. Worst:
  `transmittertower_01` 20.96 m, `church_01` 17.69 m, `watertower_01` 15.84 m. At 17.69 m only
  the church's tower cleared the ground, which reads as "the nave is missing" and sent me through
  four wrong hypotheses before I checked `originY`.
- **`--fix-orientation`** (fact 6). `.xob` normals are outward, this engine wants inward, and the
  winding is inverted with them. 59% of faces measured outward before; the flip swaps the counts
  exactly and winding goes `0.9984 → 0.0015`. Rocks go from dark-and-wrong-side-lit to solid and
  correctly lit.

**Both flags must be passed on every export from now on.** They are still default-off in the tool.

### The export command that produces the current world

```
PoseidonTools xob export "<Reforger>\addons" -w worlds/Eden/Eden.ent -o <out> -m <out>\p.txt ^
  --resmap-cache <cache>.tsv --max-models 20000 --max-placements 0 --textures ^
  --fix-origin --fix-orientation

PoseidonTools pak export "<Reforger>\addons" -w worlds/eden -o <out>\ev_DALL.wrp ^
  --terrain-assets <out>\assets --terrain-vdir "reforger\everon\data" --placements <out>\p.txt
```

Then **robocopy `<out>\reforger` into the game folder** — the model tree is part of the build.
1,248 models / ~2,000 textures / ~6 GB, about 45 minutes end to end.

Reforger lives at `C:\Program Files (x86)\Steam\steamapps\common\Arma Reforger` — 16 `.pak`. A
previous session reported it absent after checking only `D:\SteamLibrary`.

---

## THE NEXT THING: finish the 32-bit VertexIndex without corrupting OFP

**Reverted in `b175a60a`; the tree is back to `typedef short VertexIndex`. The direction is
right, the change is unfinished, and the missing piece is NOT yet known.**

### What it buys and what it breaks

Widening fixes imported buildings that render as a roof with no walls — 30 of 771 models in one
frame lose LOD 0 geometry at 16 bits, `church_01` 140,041 faces of 168,926 vertices,
`house_village_e_1l03t` 44,154. It also **corrupts every model in the shipped game**: OFP house
and menu textures, the player's weapon, tree cutouts. Roofs float as flat planes, walls vanish,
buildings melt, textures smear.

Evidence committed so it outlives the session scratchpad:

- `evidence/vertexindex-ofp-good-20260823.png` — 16-bit, intact
- `evidence/vertexindex-ofp-broken-20260823.png` — 32-bit, same pose, same hook

### Reproduce it in two steps

```powershell
# 1. flip Core/Types.hpp:265 to  typedef int32_t VertexIndex;  (git revert b175a60a)
.\scripts\Build.ps1 ; .\scripts\Deploy.ps1
# 2. capture OFP Everon and compare against the committed "good" image
<cap>.ps1 -Tag probe -World none -X 5760 -Z 3490 -Y 90 -Az 120 -El -22 -Hour 12 `
          -Delay 900 -LogLevel info -ExtraEnv "POSEIDON_TEST_RAIN=1"
```

### VERIFY WITH A PICTURE. This is the whole lesson.

When the widening first landed I checked OFP and reported it unaffected, on the strength of
`registered=98684`, `main inst=5648` and a byte-identical LOD histogram. **Every one of those
numbers is identical in the broken build**, because the same INSTANCES draw — it is the geometry
inside them that is scrambled. I never opened the image. A count is not a readback.

### Ruled out — do not re-walk these

- **The UV path.** `BuildVertices` indexes UVs with plain `int` via `src.UV(i)`; `VertexIndex`
  never touches them.
- **`Poly`'s stride.** `PolyVerticesSize` derives from `sizeof` throughout; no call site
  hardcodes 2; `Duplicate` strides via `ItemSize()`.
- **GL33's element type.** Moved to `GL_UNSIGNED_INT` in step, with a `static_assert` binding it
  to the typedef.
- **The wgpu index slice.** Widened on both sides of the FFI; the geometry pool was always
  `Uint32` and merely widened on upload.
- **`_pointToVertex` / `_vertexToPoint` in `MLODLoader`** — `int32_t` locals, unaffected.
- **`ShapeFile.cpp`'s save/load asymmetry.** REAL and now fixed (`26ee0001`): save wrote
  `face.N() * sizeof(VertexIndex)` while load read a hardcoded `LoadShort()`. They agreed only
  while the typedef was 16-bit. **Fixing it did not fix the corruption** — widening with it in
  place still melts OFP. Worth knowing, because it looks exactly like the culprit.

### The audit list, not yet examined

Every remaining `VertexIndex` site outside the ODOL readers. Something here assumes two bytes:

```
Graphics/Rendering/Draw/ClipDraw.cpp        375-376, 442-443, 687-688, 752-753
Graphics/Rendering/Primitives/Edges.cpp     29
Graphics/Rendering/Primitives/Poly.cpp      288-289
Graphics/Rendering/Primitives/Vertex.cpp    150      Vertex.hpp 269
Graphics/Rendering/Shape/Shape.cpp          581, 1387
Graphics/Rendering/Shape/Shape.hpp          34,37,48,58,66 (Selection::_sel, "short is enough")
                                            131 (const VertexIndex* faces), 326-327
Graphics/Core/Engine.hpp 490 · IGraphicsEngine.hpp 86 · Dummy/EngineDummy.cpp 212
```

`ClipDraw.cpp` is the strongest untested lead: it is the clipping path, it manipulates vertex
index lists directly, and clipping is exactly what would scramble geometry while leaving instance
and section counts untouched.

### Suggested approach

Consider whether the typedef needs to widen at all. The alternative is to keep 16-bit indices and
**split an over-large LOD into chunks**, each with its own sub-32768 vertex table and remapped
indices — the runtime already has a per-section concept (`MeshSection`, `begVertex`/`endVertex`
are already `int`). That confines the change to the adapter instead of reaching into every
consumer of the type, and it leaves the shape cache format alone.

Whichever way: **add a regression test with a real OFP model** that survives a save/load round
trip and compares face indices, so this cannot pass on counts alone again.

---

## AND THEN: correctly-lit rock blows out

Now that `--fix-orientation` lights rock correctly for the first time, it is **83% blown past
245** on the foreground boulder at `--test-world-freefly 3800 7960 4 40 -5`; whole frame 18.6%
near-white.

Controlled A/B, same rocks, same pose, only the flip differing:

| | foreground boulder |
|---|---|
| no flip | mean `93/98/55` — dark, wrong-side lit |
| with flip | mean `253/252/249` — solid, correctly lit, blown |

**It is not the albedo.** `granite_01_bcr` and `granite_02_bcr` measure identical before and
after the material work (0.315 and 0.455 sRGB, ordinary mid-grey). A ~0.17-linear albedo should
not reach 253/255. This has simply never been visible, because these surfaces have never been lit
correctly — they were showing their dark inner shell.

Start at the lighting/exposure path, not the materials. And build the A/B the same way: convert
one model group without a flag, deploy only its `.p3d`, capture the same pose. I twice
mis-attributed this — first to my own sRGB change, then to a comparison of distant shadowed rocks
against a close sunlit one that was never comparable.

---

## Also open

**Roads.** The street at the owner's town is missing because Reforger's road *surfaces* are
generated from splines and exist as no model — 262 kerb/pavement placements within 400 m and zero
road-surface models. Data model proven in
[RFG-001](decisions/RFG-001-everon-roads-are-splines-and-the-emat-item-was-stale-20260823.md):
`RoadGeneratorEntity` (2,207, real coords, `RoadWidth` 1.75–40 m) → `RoadEntity` (2,200) →
`ShapePoint` arrays, 2,491 points. Two traps that make the data look absent: the points live
inside **arrays** of entity blocks, and a `RoadEntity`'s own coords are ~0 with points local to
its parent.

**`.emat` is NOT 98.2% broken.** The previous handover says 1,275 of 1,299 materials resolve with
nothing. Measured on the current build over 2,475 materials: **30 with no albedo (1.2%)**, 122
with an empty shader family (4.9%). The real gap is 721 without a normal map and 1,184 without
specular. Do not re-plan against 98.2%.

**`MatPBRMulti` has no blend shader.** 4,191 files, the commonest class. Layers land in `Layer*`
slots and nothing samples them. The bake now approximates: tile from the BASE layer (lowest
present enabled index — taking it from the heaviest mask channel swaps a surface's material for
its weathering, which turned the church's ceramic roof into painted metal), colour mask-weighted
across layers, and the tint encoded to sRGB because `Color_N` is linear.

**10,278 placements still resolve to no converted model**, down from 13,449. 244 prefabs declare
no mesh and no parent; the rest are models the converter skipped. Worth a census — it is the same
class of problem that hid the church.

**Rain through windows and doors is unverified.** Roofs and walls are confirmed (interior capture
is dry with `POSEIDON_TEST_RAIN=particle`; `=1` enables the legacy overlay too, which is
shelter-unaware and makes any interior look like rain comes through the roof). `indoorKills`
counts drops KILLED, so a drop admitted through a portal leaves no trace. Needs a
portal-admission counter.

**A real Arma 3 render is unverified.** `--mod a3stratis` loads OFP content — my capture was
vacuous, 335 `data3d\` models. A3 is covered at the tool level (census: 350/350 models,
163,953/163,953 placements). A real render needs the substituted-world path with `--addon-root`,
like DayZ.

**MAT-049** (helicopters translucent on WGPU) and **`codex/a2oa-takistan-compat`** (5 commits,
the only unconsolidated branch) are untouched.

---

## Where things are

- Backups of every world generation: `%TEMP%\rf\world\ev_DALL.{buried,precompose,prerdb,premat}.bak.wrp`
- Capture harness: this session's `scratchpad\cap.ps1` — detached `cmd /c start`, never redirect
  stdout/stderr (os error 232). Takes `-Extra` for passthrough argv.
- `--test-world-freefly` takes **X Z Y**, altitude last. Use `-LogLevel info`: at `warn` the
  camera and shape-loader lines vanish and a capture becomes unverifiable.
- **No Reforger or DayZ data in the repo, ever.**
