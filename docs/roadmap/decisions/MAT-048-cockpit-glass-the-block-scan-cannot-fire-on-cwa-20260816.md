# MAT-048 — Cockpit glass: the block scan cannot fire on CWA at all, and the A/B would have lied

**Date:** 2026-08-16
**Branch:** `feat/texture-streaming`
**Status:** Prime suspect **exonerated by census**. Cause **not confirmed**; two ranked
hypotheses and a single discriminating run. **No code changed.**
**Related:** owner feedback §3a in `HANDOVER-PROMPT.md`, [MAT-047](MAT-047-paa-without-a-flag-tagg-disables-alpha-cutout-globally-20260815.md)

## The trap this record exists to stop

The handover's instruction was: cockpits went translucent, the alpha-class computation changed
this session (reading alpha off compressed BC2/BC3 blocks instead of decoding the whole file), so
**"`WGR_ALPHA_BLOCK_SCAN=0` — run that first"**.

Running it first would have produced a null, and the null would have been meaningless.

`IsBlockAlphaFormat` (`TextureWgpu.cpp:105`) accepts only `PacDXT2/3/4/5`. Census of the 2-byte
format magic of every `.paa`/`.pac` in the retail install — **6,976 textures**:

| format | AddOns/*.pbo | Dta/*.pbo | total |
|---|---:|---:|---:|
| DXT1 | 1212 | 4185 | **5397** |
| ARGB4444 | 158 | 604 | **762** |
| AI88 | 153 | 578 | **731** |
| P8/palette | 42 | 42 | **84** |
| DXT5 | 1 | 0 | **1** |
| ARGB8888 | 1 | 0 | **1** |
| **DXT2 / DXT3 / DXT4** | 0 | 0 | **0** |

**Zero BC2. Exactly one BC3 — `O.pbo Misc\startup_logo_CWA_RES_ca.paa`, a 2D startup logo.**

Every helicopter glass texture is ARGB4444 or AI88: Apache `apach_in_skla.paa` /
`apach_in_sklac.paa` / `apach_kokpit1-2` (ARGB4444); Kiowa's six `oh58_sklotest*` panes (AI88) plus
`kiowa_topsklo.paa` (ARGB4444); Cobra, A-10, Cessna, Ka-50 likewise. For all of them
`ScanTopMipAlphaBlocks` returns false and the class comes from the **original, unchanged**
whole-file decode.

So on CWA the flag is inert, and **a null from that A/B is not evidence** — it could not have been
anything else. The counter to read instead is `alphaBlockScans` in
`WGR_TEXTURE_STREAM_STATS`: it can only ever be 0 or 1, and seeing that closes the hypothesis
permanently in one run without an A/B.

Verified against raw bytes: `apach_in_skla.paa` begins `44 44` (ARGB4444), `TAGG AVGC = 0x2EFFFFFF`
(average alpha 46/255), `TAGG FLAG = 1`. It is a genuinely partial-alpha glass map and it has
**always** classified Blend.

## Where the block scan IS exact, and the one real behaviour change

Each divergence case the audit was asked about, checked against the reference decoder:

- **BC3/DXT5** — `DecodeBc4Palette` is character-for-character `decompressDXT5`.
- **BC2/DXT3** — the low-nibble-first expansion `(i&1) ? (b[i/2]>>4) : (b[i/2]&0xF)` then
  `(n<<4)|n` is the *identical expression* in both paths.
- **DXT1 / 1-bit** — never scanned by either; `ClassifyTextureAlpha` short-circuits to Cutout.
- **Non-block formats** — do **not** silently classify opaque; they fall through to the real scan.
- **Uniform-within-block vs varying-between** — cannot matter: the block path materialises every
  texel of the top mip and hands it to the same unmodified `ClassifyAlpha`. No per-block summary
  exists anywhere.

**The one genuine counterexample is not CWA.** `TextureSourceDDS` reports `PacDXT3/PacDXT5` for
BC2/BC3, so the block path *does* fire on `.dds`/`.edds`. The old path could never classify one:
`ScanTopMipAlphaStats` fed raw DDS bytes to `DecodePAABuffer`, whose `fgetiw` reads `'D','D'` =
0x4444 — the ARGB4444 magic, so it does not even bail — then `PacPalette::Load` reads `'S',' '` as
a palette count, fails, and the whole thing returns a default `AlphaStats` whose kind is
**Opaque**.

So **every DayZ/Reforger BC2/BC3 texture classified Opaque unconditionally before this change, and
by content after it.** That is a large, unreviewed behaviour change — arguably a fix — and it
disables the anti-translucency guard at `EngineWgpu.cpp:2804-2824`, whose own comment describes
"a solid head rendered semi-transparent". **If the cockpit sighting was in Chernarus or Everon
rather than CWA, invert this record's ranking: the block scan is back in play and the A/B is real.**

## Downstream verdicts

**`PassKindHint::Cockpit` — exonerated, and wholly inert on wgpu.** `routeAsCockpit`
(`BuildRenderPassDescriptor.hpp:141-169`) writes exactly one field, `d.pass`, and touches no blend,
depth, cull, alpha or shader state — so `Cockpit` and `None` produce byte-identical state. No
backend reads `d.pass`. And wgpu never sets `ctx.passKindHint` at all: both `BuildContext` sites
(`EngineWgpu.cpp:2415`, `:3057`) leave it `None`, while GL33 does set it. The hint
`SceneDraw.cpp:1637` writes is read by nobody, and has been that way since the root commit.

**Pass2's blend branch — live, on the exact path, and genuinely defective.** A pilot LOD is
`passNum == 1`, and the `NoDropdown` cockpit test at `Object.cpp:808` can never fire because
`ShapeSetup.cpp:787` masks `NoDropdown` out of `_special`. So the cockpit reaches
`SceneDraw.cpp:2406-2423` with `GSectionFilter = BlendOnly` and depth-write **on**. Then:

- the sort key is degenerate from inside — `zCoord = centreZ + radius`, so with the camera in the
  seat the whole aircraft sorts as one point ~8 m ahead while its geometry spans 0.5-4 m;
- **there is no intra-object section sort at all** — `Shape::Draw` walks sections in mesh index
  order. Back-to-front is a per-*object* algorithm, and the cockpit is one object spanning the view;
- with depth-write on, the failure is asymmetric: a **near** pane drawn first writes depth, and the
  **far** pane behind it then fails `GreaterEqual` under reversed-Z and is **discarded entirely**.
  You look through two panes and see one. The reverse order is correct by accident, and which you
  get is fixed by mesh section index — so the artefact is stable per model, not flickery.

**A third candidate neither brief named, and it is cheaper to test than either.** `cull_mode` is
hardcoded `Some(wgpu::Face::Back)` in the mesh pipeline (`gfx3d/mod.rs:3618`). `d.cull` is never
assigned and `RenderFlags.hpp` has no two-sided bit at all. A canopy authored single-sided for the
exterior presents its backface from inside and is culled outright — the most literal possible
reading of "you can see through the glass".

**Nothing downstream changed this session.** `ShapeDraw.cpp` last moved 2026-08-13,
`AlphaSortOrder.hpp` 2026-06-28; the Cockpit hint and `BlendOnly` are both in the root commit. The
only new input that can move a section between passes is `GetAlphaClass()` — and on CWA it cannot.
**So this may not be a regression at all**, which matters because three other items in the same
owner list are explicitly pre-existing.

## The single discriminating run

No A/B needed. Set `WGR_TEXTURE_STREAM_STATS=1` and `WGR_LEGACY_TEXTURE_TRACE=1`, sit in the pilot
seat and look **across** the cockpit — through the near pane at the far one, and up through the
overhead pane. Looking straight ahead through one pane will not show it; the defect needs two panes
in line of sight.

- `alphaBlockScans=0 or 1` → block scan is dead in this run, hypothesis closed permanently.
- glass textures appear with `blend=1` → they are being submitted and blended, so a missing pane is
  **ordering/depth-write** (hypothesis 1).
- a glass texture never appears in either trace line → it is never submitted, so it is **culled**
  (hypothesis 2). **Absence is the signal.**

**Best repro: the OH-58 Kiowa.** Its greenhouse canopy is seven separate panes — six
`oh58_sklotest*` (AI88, 8-bit alpha) plus `kiowa_topsklo.paa` — so from almost any heading you are
looking through two at once, which is exactly the case hypothesis 1 breaks. Apache is second.

## Two latent defects at `WGR_TEXTURE_MIP_BIAS > 0`

Default is 0 and it is not implicated, but both should be fixed before anyone sets it again —
and note Chernarus currently **only renders** with `WGR_TEXTURE_MIP_BIAS=1`:

- `handoff.topSize = _nMipmaps > 1 ? offsets[1] : total` (`TextureWgpu.cpp:760`) guards on
  `_nMipmaps` but indexes `offsets`, which has `levelCount` entries. When bias ≥ `_nMipmaps - 1`,
  `offsets[1]` is a **heap over-read**, and the garbage feeds a size check.
- `handoff.w/h` is the full-res size while the parked bytes start at `firstLevel`.

**Correction to the handover:** it says to confirm mip bias from the run's `meta.json` env block.
That does not work — `farfield-bench.ps1:457` writes only the `-Env` hashtable passed to the
script, so a `WGR_*` set in the shell will not appear. Check `Get-ChildItem Env:WGR_*` in the
launching shell instead.
