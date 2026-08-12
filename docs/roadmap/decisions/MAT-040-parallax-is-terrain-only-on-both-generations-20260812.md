# MAT-040 — Parallax has authored height on terrain, on both generations, and nowhere else

**Date:** 2026-08-12
**Related:** MAT-038 (retained normal maps), MAT-039 (Multi composition), WLD-017,
WLD-018 (terrain normals bound)
**Status:** Measured. Implementation scoped, not started.

## The question

Is parallax occlusion mapping worth implementing, and does Arma 3 support it?

## What the corpus says

Parallax needs a height field. Real Virtuality's normal maps are DXT5nm — Y in
green, X in alpha — which leaves red free. The `_nopx` suffix is the one that
uses it: "NO" plus a "PX" payload. Measured R channel, 256x256 resample:

| texture | generation | role | R |
|---|---|---|---:|
| `wall_01_nohq` | OA | object (rock) | **0 → 0** |
| `wall_brick01_nohq` | OA | object (brick) | **0 → 0** |
| `stonewall_01_nohq` | OA | object | **0 → 0** |
| `tk_hlina_nopx` | OA | terrain surface | 106 → 187 |
| `gdt_beach_nopx` | **A3** | terrain surface | 94 → 163 |
| `gdt_concrete_nopx` | **A3** | terrain surface | 96 → 124 |

Every object normal map holds R flat at zero. Every terrain surface map varies it.

And the split is not incidental. Counting normal-map suffixes across
`structures_f`, `rocks_f`, `plants_f`, `data_f` and `map_data` (3,874 entries):

```
_nohq   263      _nopx   31      _no   1
```

All 31 `_nopx` are in `map_data.pbo`. Not one object addon ships a single one.

## Conclusion

**Arma 3 supports parallax on terrain and not on objects — identical to Arma 2.**
The answer to "implement it for A3" is therefore the same piece of work as
implementing it for OA: one terrain implementation serves both generations, and
the `gdt_*` / `tk_*` surfaces are the same slot in the same composition.

Parallax on objects would have to synthesise height from the normal map. That is
not an upgrade — it swims under camera motion and softens exactly the detail it
claims to add — and this record exists partly so it is not attempted.

## Why it cannot be done yet, and the order it forces

The terrain fragment shader has no per-surface normal *or* height sampling at all.
Its normal is a central difference of the heightmap (`sample_normal`), which is
geometry, not surface detail. The authored per-surface maps reach the GPU as of
WLD-018 but nothing samples them.

They can only be sampled inside `sample_authored_cell`, because a surface's height
and normal are meaningless without the mask that says which surface a texel is.
And that function is gated: `a3_surface_blend` defaults to 0, is clamped to 0.35,
and fades out entirely between 25 and 120 metres.

So the order is forced, and each step is shared between the generations:

1. sample the per-surface normals already bound, inside the authored composition;
2. resolve the authored composition's own status — the 0.35 cap and the 25-120 m
   fade are an experiment's guard rails, and WLD-017 changed what it composites,
   so it has never been judged since it started binding correctly;
3. parallax on top, reading `_nopx` red, once there is a correct surface to
   displace.

Doing 3 before 2 would displace a surface that is still mostly the satellite
image, which is not a test of anything.

## Also worth recording

`ShaderSchema`'s suffix table already classifies `_nopx` as a normal map and notes
the PX payload is "unsampled here". That comment is now backed by a cross-generation
measurement rather than a single-corpus observation.

---

# Amendment — BI's own account, and a lead on the composition

The owner supplied https://www.bohemia.net/en/blog/show-me-the-light, Bohemia's
write-up of the Arma 2 renderer. Two things in it bear directly on this record.

**It confirms the measurement.** Parallax occlusion mapping is described as "a
double ray casting computed on the GPU", applied **to terrain surfaces** —
adapted from ATI sample code. Reached here from the corpus alone (objects hold
`_nohq` red flat at zero; only the `gdt_*` / `tk_*` terrain surfaces vary it), and
the vendor's own description agrees: parallax in Real Virtuality is a terrain
feature. Two independent witnesses now, one measured and one authored.

Note it is a *ray cast*, not the single-step offset implemented in the previous
commit. That was chosen for cost inside an already four-way-blended function; the
gap between the two is a known and deliberate difference, not an oversight.

**It names something the composition does not implement.** On the LCA mask:

> "by using all four channels of the ARGB mask and with some encoding tricks
> employed, the total number of different surfaces on a single satellite segment
> was increased from four to six"

The renderer treats the mask's four channels as four plain per-overlay weights
plus an unpainted remainder for the base. If the shipped masks instead *encode*
six surfaces into four channels, then reading them as direct weights is wrong
wherever that encoding is in play — and reading an encoded channel as a weight
would produce exactly the symptoms measured against the legacy path: energy in
the wrong places, and structured banding rather than noise.

This is now the leading hypothesis for why the authored composition looks worse
than the legacy path, ahead of the TexGen values. It is also checkable without
guessing: sample a shipped `*_lca.paa`, histogram its four channels, and see
whether the values cluster in a way a plain weight would not — a six-way encoding
cannot be uniformly distributed per channel.

The satellite-frame hypothesis is separately eliminated: forcing the satellite
through the legacy cell frame tiles it into a repeating grid across the terrain,
so its own worldPos TexGen is correct and is not the fault.

---

# Amendment 2 — the LCA mask is not a weight field

The histogram the previous amendment called for, run on a shipped mask from each
generation:

```
A3  m_003_000_lca  1024x1024
  R  distinct=2    0 (664,699)  255 (383,877)
  G  distinct=2    0 (954,693)  255 (93,883)
  B  distinct=3    0 (792,401)  255 (255,283)  8 (892)
  A  distinct=2  255 (948,809)  128 (99,767)     <- min is 128, never 0
  R+G+B+A ~= 255 in 28.6% of texels

OA  m_011_010_lca  512x512
  R  distinct=4    0 (252,011)  255 (8,379)  8 (1,517)  170 (237)
  G  distinct=4    0 (252,662)  255 (8,858)  4 (387)     85 (237)
  B  distinct=4  255 (242,820)    0 (18,545) 8 (542)      3 (237)
  A  distinct=1  255 everywhere
  R+G+B+A ~= 255 in 0.7% of texels
```

Two distinct values per channel. Not three, not a ramp — **0 and 255**, with a
few hundred transition texels at the seams. A blend weight cannot look like this.
These channels are *flags*, and the mask is an indexed surface selector, not
fractional coverage.

The alphas settle it. A3's never goes below 128 and takes exactly two values;
OA's is constant 255. Neither is a coverage channel. A3's two-state alpha is
exactly the shape of an extra selector bit — the "encoding tricks" BI describe
for taking one segment from four surfaces to six.

And the sums say the channels are not mutually exclusive: R+G+B+A totals ~255 in
28.6% of A3 texels and 0.7% of OA's, so multiple channels sit at 255 over the
same texel. The current shader reads those as several simultaneous weights,
renormalises them, and *averages the surfaces together* — which is precisely the
measured symptom, a washed-out ground with structured banding along the mask's
hard binary edges rather than noise.

So the composition is not mistuned; it is decoding the wrong thing. The blend
weights, the mask-weight fix earlier today, and the per-layer TexGens are all
downstream of a mask that never carried weights to begin with.

What this does NOT yet establish is the decode: which bit pattern selects which
of the six surfaces, and whether any blending between them is intended at all
(hard binary edges suggest little or none, with the satellite carrying the
low-frequency variation). That needs the pattern correlated against the layer
names the tile's own filename lists — `p_003-000_l02_l04_n_n_n` names its layers
in order, which is the key to the mapping.
