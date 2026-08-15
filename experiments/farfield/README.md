# farfield — large-world terrain / far-field rendering experiments

Isolated prototypes backing
[`docs/research/large-world-renderer-reconstruction.md`](../../docs/research/large-world-renderer-reconstruction.md)
and the Phase 3 recommendation.

**This crate is deliberately quarantined.** It declares its own `[workspace]`, so it is not
a member of the engine workspace and cannot affect the shipping build, its `Cargo.lock`, or
its `target/`. It links nothing from Poseidon and renders a procedurally generated world, so
it needs no game data and carries no asset licence. Deleting `experiments/farfield/` reverts
every trace of it.

The one piece of engine code it reuses is *ported*, not linked: the CDLOD quadtree in
`src/cdlod.rs` is a direct port of `engine/Poseidon/World/Terrain/TerrainCdlod.hpp`, so the
raster arm measures the algorithm the renderer actually ships rather than an idealised one.

## Running

```bash
cargo run --release -- far     --n 8192 --grid 12.5 --width 1920 --height 1080
cargo run --release -- raster  --n 8192 --grid 12.5 --width 1920 --height 1080
cargo run --release -- scale
cargo run --release -- veg     --n 8192 --grid 12.5
cargo run --release -- vegcards --n 8192 --grid 12.5
cargo run --release -- columns --n 4096 --grid 4 --veg-cell 8
```

`--n` is heightfield texels per side and `--grid` their spacing in metres, so world extent
is `n * grid`. Those are separate knobs on purpose: "100 km of terrain" says nothing until
you also say at what resolution it is stored, and the difference between 12.5 m and 50 m
over the same 102.4 km is 613 MB vs 38 MB.

Other flags: `--frames` (samples per measurement), `--far`, `--max-steps`, `--fixed-step`,
`--seed`, `--out`, `--no-shots`.

Screenshots are written as 24-bit BMP (no image-codec dependency). `bmp2png.py` converts
them if you want to view them somewhere fussier.

## Experiments

| id | command | question |
|---|---|---|
| EXP-FAR-001 | `far` | Cost of a naive fixed-step heightfield march |
| EXP-FAR-002 | `far` | Cost of a hierarchical max-mipmap march — can a ray cross 100 km cheaply? |
| EXP-FAR-003 | `far` | Adding a volumetrically-marched canopy band |
| EXP-FAR-004 | `far` | Adding a hard canopy/roof shell to the acceleration pyramid |
| EXP-TERRAIN-002/003 | `raster` | CDLOD raster cost on the same world at 5 / 20 / 100 km |
| EXP-VEG-001 | `veg` | Storage arithmetic: per-tree instances vs clusters vs density field |
| EXP-VEG-002 | `vegcards` | GPU-placed vegetation cards from a density field — cost of a 1/2/5 km ring |
| EXP-COL-001 | `columns` | Bake explicit trees into 2.5D occupancy columns; compare bilinear vs nearest reconstruction |
| — | `scale` | Memory and cost against world extent and sample spacing |

## Measurement hygiene

Every figure is the **cheapest steady frame** of N, never the mean. This follows the rule
established in `docs/roadmap/decisions/RND-033`: a shared GPU inflates every timed region by
a common factor, so the minimum is the only statistic that is not contaminated by another
process. GPU timing uses `TIMESTAMP_QUERY`; if the adapter lacks it the harness says so and
reports no times rather than reporting wall clock and calling it GPU time.

Known limits of these numbers, stated so nobody over-reads them:

- One machine, one GPU, one resolution, **no shadows**, synthetic content. Sound for
  relative comparisons within a run; not absolute frame budgets.
- EXP-FAR-001 is capped at 512 steps, so at an 8 m step it only reaches 4.1 km and never
  crosses the world. Its cost is the cost of giving up, which is the finding — but it is not
  the cost of a *correct* fixed-step march (that needs ~12,500 steps per ray at 100 km).
- The synthetic world's roads are a proxy (low-gradient noise ridges), not a road network.

## Two things that are easy to get wrong

**The max-mipmap skip condition.** A cell may be skipped only if the ray segment *inside*
it stays above the cell's max height along its whole length. For a straight ray the segment
minimum is at one of its endpoints, so `min(y_entry, y_exit) > cell_max` is exact. Testing
only `y_entry` — the obvious implementation — tunnels through ridges on any descending ray.

**Vertex stages cannot bind `read_write` storage in baseline WGPU.** `src/veg_place.wgsl`
and `src/veg_draw.wgsl` are separate modules for exactly this reason: the same buffer is
bound read-write to the placement compute and read-only to the draw. The alternative is the
non-portable `VERTEX_WRITABLE_STORAGE` feature, which a production implementation should not
depend on.
