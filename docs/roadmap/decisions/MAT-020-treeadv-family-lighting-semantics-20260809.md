# MAT-020 — TreeAdv render-state semantics are family-specific

**Date:** 2026-08-09
**Related:** MAT-020, MAT-030, `RND-post-load-shapes-have-no-vertex-buffer-20260809.md`
**Status:** Semantics established; no production lighting change made.

## The question

After the post-load shape fix moved the shipped Arma 3 pine onto the real WGPU
material path, the tree became materially correct but too bright in the 01:00
fixture. The old fallback had been untextured software geometry; it cannot be
used as a lighting reference.

The initial RVMAT census made a generic `emmisive` mapping unsafe: it is
non-black in almost every TreeAdv material, although real trees do not emit
light at night. The correct meaning had to be established before wiring any
authored state.

## Evidence

The controlled installed-game run used the real direct WGPU path
(`WGR_GPU_DRIVEN=0`) and placed
`a3\\plants_f\\tree\\t_pinusp3s_f.p3d` after the world had entered gameplay.
It exited successfully after a 01:00 screenshot. The trace reports 13 LODs /
5,931 faces and resolves both authored RVMATs with `schema=true`:

- trunk: `TreeAdvTrunk`, `t_pinusp3s_f_lod1_noatlas.rvmat`;
- crown: `TreeAdv`, `t_pinusp3s_f_lod1_polyplane.rvmat`.

The captured scene has nearly black ground while the pine remains visibly lit.
This is a renderer-lighting discrepancy, not a missing material or texture.

BI's official [Arma 3 tree material guide](https://community.bistudio.com/wiki/Arma_3:_Trees)
defines the otherwise misleading state vectors:

| family | source field | documented meaning |
|---|---|---|
| TreeAdv crown | `ambient` | crown self-shadow/bleed and per-pixel main-light softening controls |
| TreeAdv crown | `diffuse.rgb` | diffuse plus ambient colour |
| TreeAdv crown | `forcedDiffuse` | per-pixel translucency and MCA ambient-occlusion tuning |
| TreeAdv crown | `emmisive` | sky translucency modulation RGB and main-light translucency multiplier; **not emission** |
| TreeAdv crown | `specular` | main-light specular RGB and ambient-specular multiplier in alpha |
| TreeAdvTrunk | `ambient`, `forcedDiffuse`, `emmisive` | explicitly ignored |
| TreeAdvTrunk | `diffuse.rgb` | diffuse plus ambient colour |

The same guide defines MCA RGB as macro colour and MCA alpha as ambient occlusion
(optional when the texture has no alpha). MAT-030 already established that Stage2
is MCA for all observed TreeAdv-family materials, but the present WGPU shader
does not sample that slot.

## Current renderer behaviour

On the HDR sky-lit path, shared `shade()` deliberately ignores every
per-material sun/ambient/diffuse term and uses frame-global atmospheric sky
irradiance. It also does not sample MCA. This means the observed night result
cannot be repaired by globally forwarding `TranslatedMaterial` values: those
values are bypassed on the active path, and a literal emissive implementation
would be semantically wrong for TreeAdv.

The existing generic foliage SSS/ambient-boost pass applies only to the cutout
crown. It cannot account for the bright solid TreeAdvTrunk section, so reducing
those knobs would not be a complete fix.

An attempted `WGR_HDR=0` A/B did not produce a valid comparison: the renderer
then reported a reproducible wgpu validation error because the `wgr_sky` pipeline
expects `Bgra8Unorm` while the active render pass uses `Rgba16Float`. The run
exited after the screenshot request but no PNG was produced. This is recorded as
a separate pipeline-configuration defect, not lighting evidence.

## Decision

Do not globally route RVMAT render-state fields through the generic material
fold. The next implementation must introduce a TreeAdv-family lighting path
that consumes only documented family semantics, starting with:

1. TreeAdvTrunk: `diffuse`, documented specular handling, MCA macro RGB and
   optional MCA-alpha ambient occlusion.
2. TreeAdv crown: MCA macro/AO plus the documented state-vector controls,
   explicitly treating `emmisive` as translucency input rather than emission.
3. Controlled per-term numerical captures on the 01:00 pine fixture before
   changing defaults or claiming the owner-visible night result is fixed.

This is a new family shader semantic implementation, not a generic MAT-020
render-state propagation patch.
