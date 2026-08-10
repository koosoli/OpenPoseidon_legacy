# MAT-032 — generating the textures Real Virtuality writes as names

- **Ticket:** MAT-032
- **Date:** 2026-08-10
- **Follows:** MAT-031, which measured this as the last albedo gap on Stratis

## What was missing

A Real Virtuality texture reference can be a *generator* rather than a path:

```
#(argb,8,8,3)color(0.5,0.5,1,1,NOHQ)
#(ai,64,64,1)fresnel(2.0,0.1)
 ^type ^w ^h ^mips  ^generator ^args
```

Nothing in this build produced one. Every such reference went down the file-source
chain, found no file, and failed to upload — which for a surface whose entire
appearance is an authored flat colour means nothing to draw. Arma 3's landing and
warning lights are exactly that: their **face** texture is
`#(argb,8,8,3)color(1.0,0.95,0.85,1.0,co)`.

## What is generated, and what deliberately is not

Only `color`. It is a constant, so building it is not a guess. The other
generators — `fresnel`, `irradiance` — are lookup tables computed from their
parameters, and producing a plausible-looking table would be exactly the kind of
invention the material work has refused elsewhere. They parse, report which
generator they are, and report that they are not generated, so a caller can say
which one it was rather than reporting a generic failure.

## The arguments are RGBA, whatever the type token says

The header says `argb`; the arguments are R, G, B, A. The corpus settles it without
needing documentation:

- `color(0.5,0.5,1,1,NOHQ)` read as RGBA is a flat tangent-space normal at full
  alpha, which is what a NOHQ stand-in must be. Read as ARGB it is a
  half-transparent cyan.
- `color(0.2,0.2,0.2,1.0,AS)` is an opaque grey one way and nearly invisible the
  other.

The trailing token (`NOHQ`, `DT`, `SMDI`, `co`) is a role tag, not a component. It
is parsed so it cannot be mistaken for one, and then ignored: role comes from the
shader family's slot, never from the texture (MAT-030).

Components outside 0..1 saturate rather than wrap — a `specular` of 1.4 is in the
corpus, and wrapping would turn an over-bright value into a dark one. The declared
size is honoured rather than collapsed to 1×1, but clamped to 256, so a malformed
header cannot become an allocation.

## Result on Stratis

Whole-world object census, same harness as MAT-031:

| | before | after |
|---|---:|---:|
| materials with no albedo | 301 | 293 |
| ...because the stage is procedural | 8 | **0** |
| fallback face texture failed to upload | 2 | **0** |
| **cannot resolve a colour at all** | **5** | **2** |

The two that remain are `a3\data_f\penetration\{metal_plate,concrete}.rvmat`, which
have no texture by design. That they reach a visual draw is a LOD-selection
question, not a material one, and is the next thing to look at rather than
something to paper over with a colour.

Of the 293 that bind no RVMAT albedo, 291 fall back to a face texture that uploads
— the correct outcome for families that carry no Stage0, which is nearly all of
them.

## Why the shared texture layer

`ProceduralTexture.hpp` lives under `Graphics/Textures/` rather than in the wgpu
backend, because the reference appears in two unrelated places — a material stage
and a model's face texture — and a second, subtly different parser in the other
backend is how the two would drift apart.

`ResolveMaterialBaseColour` no longer rejects every procedural Stage0 outright; it
rejects the ones that are lookup tables and binds the ones that are colours.
