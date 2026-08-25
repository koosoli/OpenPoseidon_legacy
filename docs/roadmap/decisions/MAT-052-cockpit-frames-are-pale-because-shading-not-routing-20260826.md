# MAT-052 — cockpit frames render pale on WGPU, and it is shading parity, not routing

- **Ticket:** MAT-052 (open, WGPU cockpit shading)
- **Date:** 2026-08-26
- **Status:** OPEN — mechanism narrowed to the shading fold, with the classifier
  and camera halves of the owner's report FIXED separately (see the commit that
  lands this record).

## The report, dissected into three defects

Owner: "cockpit of OFP helis is still transparent; cockpit view of Arma 1/2/3
helis is still too far from the pilot position and transparent." That is three
mechanisms, two of which are fixed:

1. **FIXED — solid panels classified Blend.** `cobra_palubka_bl/br.paa` (AI88,
   9.9%/13.5% partial, mean 235/229, zero clear) classified Blend on the plain
   partial>=2% rule, were dropped from the opaque pass, and nothing revisits
   blend sections in the camera vehicle's cockpit queue — the panels simply
   did not draw. ClassifyAlpha now keeps a no-clear high-mean surface
   (pctClear < clearThreshold && aMean >= 200) Opaque: the partial texels are
   anti-aliased seams on a solid, not translucency. True glass is nothing like
   this shape (uh60_skla mean 29, 99% partial; MH-9 glass mean 75, 100%).
   This same rule is expected to fix the see-through M60/M16 detail parts
   against the sky (the standing task) for every high-mean case — re-measure
   there before closing that one.
2. **FIXED — bridged-vehicle interior camera at model origin.** Bridged crew
   proxies (\ca\temp\proxies\..., \a3\data_f\proxies\...) exist neither as
   files nor CfgNonAIVehicles classes, so no ManProxy is Present() and
   Transport::InsideCamera returned MIdentity. New degraded-path fallback:
   memory `driverview` (A2/A3 authored) -> memory `pilot` -> VIEW_PILOT
   NamedPosition("pilot"). Measured on the A1 UH-60: camera moved from metres
   aft to the cockpit bulkhead. Stock never reaches this code (its proxies
   resolve).
3. **OPEN — the frames themselves render PALE.** GL33 vs WGPU on the stock
   Cobra pilot view (`.tmp-a1-tank/cobragl.png` vs `cobra2.png`): GL33's canopy
   members are solid dark camo; WGPU's are washed-out translucent ghosts. The
   frame texture `ah-1_kabina_predo.paa` is **AI88 = luminance+alpha** — it has
   no colour of its own — and the per-draw trace on the same material family
   shows `emi2.000` (emissive 2.0) in the folded material. Leads, in order:
   (a) how the wgpu fold applies emissive/ambient to a LUMINANCE texture vs
   GL33's fixed-function modulate; (b) whether the AI88 decode expands L into
   RGB the same way GL33's does; (c) whether cockpit (NoDropdown /
   PassKindHint::Cockpit) draws take a lighting path that skips the sun fold.
   NOTE the engine's Shape carries NO per-vertex colour array and `SVertex`
   has no colour field — if the fix turns out to need authored vertex colour,
   that is an ABI-touching mesh-format change, not a shader tweak.

## Witnesses (all in `.tmp-a1-tank/`)

`cobra.png` / `cobra2.png` (WGPU before/after classifier — panels unchanged
because the Cobra sides are genuinely glass; the classifier's panel case shows
on vehicles with solid palubka panelling), `cobragl.png` (GL33 reference),
`pilotglass.png` (UH-60 inside, glass floor working), `a1cam.png` (bridged
camera after fallback).
