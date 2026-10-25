# Water audit: open-water, minamo, napali vs Tidewater (2026-10-25)

This audit compares three new open-source water renderers with Open Poseidon's Tidewater ocean. It is read-only:
nothing was ported and the renderer was not changed.

**What was read.** Each repo was shallow-cloned at its default-branch HEAD on 2026-10-25, outside this repository.
The baseline was our Tidewater code on `perf/reforger-render-hitches` @ `d29d139c4`, the newest branch that carries
`engine/WgpuRenderer/rust/src/water_tw/`. This audit branch (`new-renderer-infrastructure` lineage) has no
`water_tw/`, and `docs/roadmap/modernisation/Tidewater/` does not exist on either branch. The files compared were
`water_tw/fft.rs`, `tw_water.wgsl`, `tw_underwater.wgsl`, `caustics.rs`, `tw_caustics_splat.wgsl`,
`tw_surf_foam.wgsl` and `fft_cover_tests.rs`.

**Licences.** All three repos are **MIT**, which is compatible with our GPL-3.0-or-later. A port must keep their
copyright notice, as we already do for dgreenheck/tidewater. Two exceptions:
- minamo's CC0 Poly Haven assets are not needed.
- minamo's brand art (`docs/brand/`) is excluded from its MIT grant.

No game data is involved.

## Where we already stand

These items need no work:
- **Waves.** Four FFT cascades of 256² (733 / 157 / 33.3 / 7.1 m) with a Horvath JONSWAP + TMA spectrum, a second
  swell system, a shared-memory FFT and a Jacobian foam field that persists and decays.
- **Foam calibration.** Whitecap cover is measured on the GPU: `WHITECAP_COVER_BIAS` and the `fft_cover_tests.rs`
  guard tests.
- **Surface shading.** GGX sun with Cox–Munk unresolved roughness, chosen by pixel footprint. SSR, plus capillary
  ripples that re-sample the finest cascade near the eye.
- **Shore.** Breakers, swash, lip, shore sim and lace.
- **Underwater.** Photon-splat caustics in two layers, a froxel medium with HG phase and shafts, a per-pixel lens
  split from the probe grid, a meniscus, wakes, spray and impacts.

**Gaps.** On features, Tidewater is broader than any of the three repos. Where they beat us, the gaps are in
**specular and foam fidelity**:
- how sun glitter is distributed;
- how slope variance flows from the mips into the BRDF;
- how foam ages.

---

## 1. Leonxlnx/open-water

**Licence:** MIT (© 2026 Leonxlnx). **Stack:** WebGL2 (GLSL ES 3), single-scene open ocean with weather, no assets.

**What it does.** A Tessendorf FFT in four cascades of 256² (1800 / 117 / 21 / 3.7 m).
- **Spectrum.** Elfouhaily 1997 unified spectrum with capillary-gravity dispersion. Spreading is Donelan–Banner near
  the peak, ECKV for short waves, blended to cos²ˢ in a gale. A narrow swell sits on top.
- **Geometry.** A camera-centred polar grid. Each vertex samples the cascades at a mip chosen from its ring spacing.
  Earth curvature is applied.
- **BRDF.** Bruneton 2010 LEAN Beckmann BRDF. Glints come from Poisson-sampled capillary facets.
- **Foam.** Two stages: active breaking and residual. They are thresholded against a procedural bubble-raft pattern,
  and the coverage is calibrated to Monahan's whitecap law.
- **Rest of the scene.** A Hillaire atmosphere and volumetric clouds, plus rain, lightning and night.

**What it does better than us:**

| Technique | Where | Why it is better |
|---|---|---|
| **Poisson sun glints** | `src/ocean/water.js` FS, "Glints" block (~l.266-295) | The expected sun reflection is computed as a facet count per pixel footprint: λ = E[L]·Ω☉/(F·L☉)·N. The shader then draws a Poisson count (by inversion when λ < 12, Gaussian above), re-rolled at 12 Hz. Near the eye that gives sparse full-brightness sparkles on dark water; far away it converges to the smooth glitter path, and **the mean is preserved**. Our GGX lobe is one smooth highlight, capped at 400, at every distance. This is the most visible "photo vs CG" difference in sunny scenes. |
| **True LEAN slope moments** | `src/ocean/ocean.js` ASSEMBLE_FS (`oSlope = vec4(s, s*s)`), `water.js` `SLOPE()` macro | Each cascade stores slope **and** slope² and mips both with hardware filtering. Per pixel, `var = E[s²] − E[s]²` is the variance filtered out at that mip, added to the BRDF width, per axis (anisotropic). Geometry, normals and BRDF hand over exactly. We estimate unresolved roughness from a global Cox–Munk mss and `log2(footprint)`, which is isotropic and not tied to the local sea. |
| **Specular AA plus a variance cap** | `water.js` FS l.201-208 | Kaplanyan–Hoffman normal-variance AA, and slope variance capped at the whole sea's mss, so the glitter path keeps narrowing toward the horizon. We have neither term explicitly. |
| **Two-stage foam (active + residual)** | `ocean.js` ASSEMBLE_FS (`oActive`, `exp(-2.5·dt)`), `foam.js` `foamCoverage` | Active froth lives about 0.4 s as a dense core. Residual foam lives for seconds and opens into lace along a Voronoi raft network instead of shrinking as a blob. Our Jacobian foam is one field with one decay rate, so a fresh cap and its old trail differ only in density. |
| **Hydrodynamic modulation** | `water.js` FS l.149-163 (`crestM`, `face`, `sm`) | Short-wave slope variance is scaled 0.8–1.25× by crest height and by the downwind face of the dominant waves. Short waves bunch on crests and smooth troughs stay glassy. We modulate by gust noise only. |
| **Downwind froth smear** | `water.js` FS l.180-183 | The active field is re-sampled 1.5 / 3 / 5 m downwind, so a cap trails its froth. Cheap, and it removes the "painted on the crest" look. |
| Elfouhaily spectrum + capillary dispersion | `ocean.js` SPECTRUM_FS | Short waves are calibrated to Cox–Munk, and Hs is checked against the Beaufort scale (`tools/hscheck.mjs`). It is not obviously better than Horvath for our scale. Note it; do not switch. |

**GPU cost (estimate, our renderer, 1440p, about half the screen water):**
- **Glints:** about 15 ALU plus a loop of up to 24 iterations in a rarely taken branch. About 0.05–0.15 ms.
- **LEAN:** one rgba16f 256²×4 array (2 MB) plus mips in the FFT column pass, about 0.02 ms. Four extra filtered
  samples per water pixel, about 0.1 ms.
- **Two-stage foam:** one r16f channel in the existing column pass, negligible. Two or three extra samples, about
  0.05 ms.
- **The whole set:** about 0.2–0.3 ms against today's ~3 ms water.

**Verdict: port** the glints, the LEAN slope moments with the specular-AA cap, and the two-stage foam. These are small,
self-contained GLSL blocks that map directly onto our existing cascade layout. Borrow the idea of hydrodynamic
modulation and the downwind froth smear. **Skip** the sky, clouds, rain and lightning: we have our own sky stack, and
those parts are scene-specific. Skip the polar grid: our CDLOD is already uniform in screen space.

## 2. kanaworksai-hue/minamo

**Licence:** MIT (© 2026 東京AI映像合同会社). The brand art is excluded; the assets are CC0 Poly Haven. **Stack:**
three.js WebGPURenderer plus hand-written WGSL compute and post passes. Stated cost is about 7 ms on a laptop
RTX 4070 at 1080p, for the whole scene.

**What it does.** A tropical bay rendered above, below and exactly at the waterline.
- **Waves.** Three FFT cascades of 256² (250 / 37 / 7.3 m) with JONSWAP and swell. Inverse FFT in shared memory.
- **Waterline.** An exact per-pixel near-plane waterline test.
- **Surface.** Snell's window and TIR from below.
- **Underwater.** A Jerlov-IA single-scattering medium in closed form, chromatic area-ratio caustics and half-res
  shafts.
- **Lens.** A lens water film plus drops after surfacing.

**What it does better than us:**

| Technique | Where | Why it is better |
|---|---|---|
| **Lens water film and drops after surfacing** | `src/post/post.js` (lens film, beads), `src/post/lensDrops.js` | A half-res semi-Lagrangian film drains down the lens. CPU-simulated drops slide, leave trails, merge and evaporate, and clear when the lens goes under again. We have a meniscus at the lens split but nothing after the camera leaves the water, so surfacing reads as an instant cut. |
| **Chromatic caustics** | `src/ocean/caustics.js` (three IORs 1.3310 / 1.3335 / 1.3370, one per channel via `writeMask`) | Faint dispersion fringes on the bed. Our splat is monochrome. Their own notes list a cheaper variant as "not done yet": dispersion approximated at lookup time. |
| **Exact waterline filtering** | `post.js` `bilinearExact` / `bsplineDisp` / `heightAt` | Hardware bilinear keeps about 8 bits of sub-texel position, which is about 4 mm steps on a 1 m texel. They use f32 bilinear from four `textureLoad`s, plus a 4-tap cubic B-spline on the finest cascade, plus 6 damped fixed-point inversions of the horizontal displacement. The **same** function runs in the mesh and in the test. We answer from a probe grid within 0.12 m. Worth it only if stair-steps along our waterline are ever reported. |
| Closed-form single scattering per channel | `docs/HOW_IT_WORKS.md` §Underwater | A clean analytic segment integral with `Kd` for downwelling and `σt` along the view. Our froxel medium already covers this and does more (shadows, shafts). Nothing to take. |

**GPU cost (from their profile, scaled to us):**
- **Lens film:** about 0.4 ms, up to 1 ms right after surfacing, and 0 when the lens is dry, so it is only paid
  transiently.
- **Chromatic caustics:** three splats cost them about 0.7 ms. That is 3× our splat cost, so use the lookup-time
  approximation instead (three offset taps, about 0.05 ms).
- **Exact waterline:** about 0.5 ms at full res. Our probe grid is cheaper.

**Verdict:**
- **Borrow** the lens film and drops: the idea, and the film advection, which is small enough to port with credit.
  This is the most noticeable gap for swimming, diving and boats.
- **Borrow** chromatic caustics, as a lookup-time offset rather than three splats.
- **Skip** the exact waterline for now, unless we see waterline stair-steps.
- **Skip** the three.js integration, the rock LODs and the post chain.

## 3. Leonxlnx/napali

**Licence:** MIT (© 2026 Leon Lin). **Stack:** WebGL2. A single generated `index.html` that ray-marches an SDF coast
(terrain, vegetation and ocean). The README states about 1–2 s per frame offline on an integrated GPU; the real-time
mode drops resolution. **Not a real-time renderer at our budget.**

**What it does.** A drone flight over a procedural Na Pali coast.
- **Waves.** A three-cascade FFT ocean (`oceanH`) intersected by a 24-step ray march.
- **Breakers.** An analytic shoaling breaker train along a blurred coast-distance field (`shoal()`), with
  per-section crests and a dense bore followed by a residual-foam trail.
- **Swash.** A swash sheet on beaches (`swash()`).
- **Foam and glints.** Footprint-filtered whitecap and surf foam, and glints, in `tools/v4_water.glsl`.

**What it does better than us (ideas only, the code is too costly and too scene-specific):**

| Technique | Where | Why it is better |
|---|---|---|
| **Footprint-averaged surf profiles** | `index.html` `shoal()` (the `gShFW / lam` block, r10w) | When a pixel spans more than about λ/6 of the wave spacing along the shore distance, each breaker profile is replaced by its **exact period mean**. The slope goes to 0 and the phase to a fixed value. That removes the grey-white dashes where far rows alternately hit and miss a crest. If our far breaker bands ever shimmer, this is the analytic fix. |
| **Cascade anti-tiling** | `index.html` `oBlendW` / `OB_OFF` / `OB_SC` | Cascades 1 and 2 are also sampled as a shifted copy rescaled ×1.1736. The two are blended by a slow noise mask with **variance-preserving** weights (w/‖w‖), so the 137 m and 20 m tiles never repeat and the wave statistics stay unchanged. We rely on non-integer cascade ratios. This costs about 4 extra taps on two cascades. Only worth doing if tiling is seen from altitude (helicopters, Earth mode). |
| **Footprint-area glints and whitecaps** | `tools/v4_water.glsl` header comments (r8w) | Each glint and cap is a disc convolved with the pixel footprint, so a sub-pixel one lights the pixel by its area share instead of as a crawling 1-px dash. The idea is the same as open-water's Poisson glints, from the other direction. open-water's version is the better fit for us. |
| Crest sectioning / lobed swash | `shoal()` `sect`, `swash()` `lobe` | Each wave breaks in sections, and run-up comes in scalloped lobes. We already have breakers and swash. Compare visually before taking anything. |

**GPU cost:** as written, unusable in real time. Each idea alone costs about 0.05–0.1 ms.

**Verdict: skip the code, borrow two ideas.** Take footprint-averaged breaker profiles if far surf aliases, and
variance-preserving anti-tiling if tiling shows from the air.

---

## Top 3 worth doing (ranked)

1. **Poisson sun glints plus a slope-variance cap** (from open-water `water.js`, MIT).
   - *Gain:* discrete sparkles near, a correct glitter path far, mean-preserving, so no look regression on average.
   - *Cost:* about 0.1 ms.
   - *Risk:* temporal shimmer. It needs a still A/B and a motion capture, and must stay stable under DLSS (re-roll
     rate against the history).
2. **LEAN slope moments in the FFT output** (`(s, s²)` per cascade, mipped; per-pixel anisotropic variance into the
   GGX alpha; Kaplanyan normal-variance AA).
   - *Gain:* glitter and roughness that follow the actual local sea, not a global Cox–Munk guess. Also the right
     input for item 1.
   - *Cost:* about 0.1 ms and 2 MB.
   - *Risk:* it changes the far-sea brightness, so re-check `fft_cover_tests` and the water-look profiles.
3. **Two-stage whitecap foam (active + residual)** plus downwind froth smear (open-water `ocean.js` / `foam.js`).
   - *Gain:* fresh caps with solid cores, and old foam opening into lace rather than fading as blobs.
   - *Cost:* about 0.05 ms.
   - *Risk:* foam cover is the most sensitive control we have. Re-fit `WHITECAP_COVER_BIAS` with the cover harness,
     never by eye.

Runner-up: **minamo's lens water film and drops after surfacing.** It is the biggest gap for diving and boats, but more
work than the top 3, and it touches the post chain and the DLSS ordering.

All four are additive and fidelity-raising, and none trades away visual quality, which fits the owner's "no visual
regressions for perf" rule. Each port must keep the MIT copyright line of its source in the file header, as our
Tidewater port does.
