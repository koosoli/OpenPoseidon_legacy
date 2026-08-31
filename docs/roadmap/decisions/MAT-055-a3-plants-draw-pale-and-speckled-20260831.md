# MAT-055 — the Stratis plant that "does not draw fully" is the DEAD oleander, and it is a cutout-alpha defect, not a wrong texture

Status: **DIAGNOSED, NOT FIXED.** Parked at the owner's instruction ("not a priority").

## The report

Owner, on Arma 3 Stratis: *"flimmert das Gras … das kannst du auch in einem einzigen Bild
erkennen, weil diese Grashalme nicht voll zeichnen; andere Gräser funktionieren aber, es ist
also ein Problem mit dieser Grasart."* — visible in a single frame, one plant type only.

## Which binary

Two binaries were involved, because another agent redeployed the game folder at 13:20 while this
session was measuring. Both are named next to their captures below.

| stamp | commit | captures |
|---|---|---|
| `asset-obs-001 cedefdf3 2026-08-31_12:29` | `cedefdf3` | `base.png`, `base-view.png`, `close-nerium.png`, `nerium-blendtrace.log` |
| `asset-obs-001 1817ff3e 2026-08-31_13:26` | `1817ff3e` (14 commits later, shadow-pass perf work only) | `models2.png`, `m2-basecol.png`, `sectrace.log`, `gl33.png`, `m2-nomipalpha.png`, `nomat.png`, `alphashape.log` |

The defect is identical on both, and none of the 14 intervening commits touches material or alpha
routing. Exe and DLL in the game folder carried matching timestamps (13:20) for the second set.

## Repro

```
OpenPoseidon.exe --check --render wgpu --window --width=1600 --height=900 --mod @a3stratis \
  --test-world "<repo>/packages/a3-compat/world/stratis/stratis.wrp" \
  --test-world-freefly 2351.98 5055.03 89.98 16.0 -17.2 \
  --test-mission "dev-missions\devtest.abel" --test-type screenshot \
  --screenshot <png> --screenshot-delay 900 --log-file <log> --log-level info
```

A world-free repro also exists and is the better harness — no `.wrp`, no placement, a correct
control in the same frame:

```
OpenPoseidon.exe --check --render wgpu --window --width=1600 --height=900 --mod @a3stratis \
  --test-mission "dev-missions\devtest.abel" --test-type screenshot \
  --test-model "a3\plants_f\bush\b_neriumo2d_f.p3d" \
  --test-model "a3\plants_f\bush\b_ficusc1s_f.p3d" \
  --test-model "a3\plants_f\bush\b_neriumo2s_f.p3d" \
  --test-model-distance 8 --test-world-freefly 5058.75 3902.55 70.1 151.2 -3 \
  --screenshot <png> --screenshot-delay 900 --log-file <log>
```

(`--test-world-freefly` works without `--test-world`; without it the mission keeps its own camera
and the placed models are off-screen, which is how the first attempt produced a useless picture.)

## What the plant is

**`a3\plants_f\bush\b_neriumo2d_f.p3d`** — the **DEAD** nerium oleander bush. Named three
independent ways:

1. **Placement projection.** `PoseidonTools terrain objects stratis.wrp --near 2351.98 5055.03 700`
   projected through the logged camera (`camera at 2351.98, 89.98, 5055.03; azimuth 16 elevation
   -17.2`, `leftFOV=1.333 topFOV=0.750`) puts the nearest object — `b_neriumo2d_f` at 11.2 m —
   exactly on the big pale-white bush at the bottom left of the repro screenshot.
2. **Close-up at its own coordinates.** Freefly to `2357.2 5056.2 84.5 az 270 el -5`, 10 m from the
   `b_neriumo2d_f` at `(2347.20, 82.10, 5056.20)`: the defect fills the frame.
3. **Isolated `--test-model` scene, no world.** Placed beside the LIVING oleander
   `b_neriumo2s_f.p3d`, the dead one is pale and holed and the living one is correct — green
   leaves, saturated pink flowers, solid silhouette. That control is the single most useful
   artefact in this investigation.

Within 200 m of the repro camera: 87 `b_neriumo2d_f`, 419 `b_ficusc1s_f`, 198 `t_ficusb1s_f`,
36 `b_ficusc2s_f`. Within 700 m and on screen: 347 `b_neriumo2d_f`.

## The material and the texture

| | broken `b_neriumo2d_f` | working control `b_neriumo2s_f` |
|---|---|---|
| RVMAT | `a3\plants_f\bush\data\b_neriumo2d_f_lod1_polyplane.rvmat` | `a3\plants_f\bush\data\b_neriumo2s_f_lod1_polyplane.rvmat` |
| PixelShaderID | **`TreeAdvTrans`** | `TreeAdv` |
| VertexShaderID | `TreeAdvTrunk` | `TreeAdvTrunk` |
| Stage0 | absent (face texture is the colour) | absent |
| Stage1 | `_polyplane\data\polyplane_neriumoleander_l1_dead_nohq.paa` | `..._l1_nohq.paa` |
| Stage2 | `#(argb,8,8,3)color(0.5,0.5,0.5,1.0,MCA)` | same |
| ambient / diffuse / forcedDiffuse / emmisive / specular | byte-identical to the control | — |
| LOD0 section 0 texture | `polyplane_neriumoleander_l1_dead_ca.paa` | `polyplane_neriumoleander_l1_ca.paa` |
| LOD0 section 1 texture | **`polyplane_neriumoleander_l1_dead_ca.paa` (the same alpha atlas)** | **`a3\plants_f\_bark\bark_fagussylvatica_l3_co.paa` (an opaque `_co`)** |

The two materials differ in exactly two things: the shader family string, and the texture paths.
Every numeric constant is the same.

The atlas, `a3\plants_f\_polyplane\data\polyplane_neriumoleander_l1_dead_ca.paa`, is 2048×2048
DXT5, 10 mips, alpha **53.7% clear / 43.9% opaque / 2.3% partial** — a clean hard-edged 1-bit
cutout mask with a thin anti-aliased rim. Whole-atlas mean RGB is a dark warm brown, **(68, 60,
52)**; opaque texels (74, 64, 56), clear texels (63, 55, 48). There is no pale content in it
anywhere.

## The mechanism

The bush is **not** miscoloured by a wrong bind. It is **made entirely of cutout cards that the
alpha path does not resolve**, and there is no opaque geometry anywhere in the model to anchor it.

Two facts carry this:

* **`SECTRACE`** (`WGR_SECTION_SKIP_TRACE=neriumoleander`) shows the working control's LOD0 is
  *leaf atlas + opaque bark `_co`*, while the broken model's LOD0 is *the same alpha atlas on both
  sections*, both with `special=0x0` (neither `IsAlpha` nor `IsTransparent`). So every pixel of the
  dead bush depends on the Blend→cutout promotion in `DrawSectionTL`
  (`engine/WgpuRenderer/EngineWgpu.cpp:3386-3425`) being right; the living oleander has a solid
  dark core that reads correctly whatever the cards do.
* **`WGR_ALPHA_SHAPE_TRACE=1`** shows both atlases stay classified `BLEND` at runtime — the
  anti-aliased-mask demotion is measured (`clearNb=3.977 clustered=1.000 bandWidth=2.19` for the
  dead atlas) but is **not applied**, because `AlphaMaskDemoteEnabled()` gates it off:

  ```
  Wgpu alpha shape: ...neriumoleander_l1_dead_ca.paa 2048x2048 clear=53.7% partial=2.3%
      clearNb=3.977 clustered=1.000 bandWidth=2.19 BLEND -> BLEND
  Wgpu alpha shape: ...neriumoleander_l1_ca.paa      2048x2048 clear=53.7% partial=7.7%
      clearNb=3.970 clustered=1.000 bandWidth=5.40 BLEND -> BLEND
  ```

  Both therefore reach `DrawSectionTL` as `alphaClass == Blend` and depend on
  `GpuSectionMaterialUsesLeafCards` / `GpuSectionMaterialIsCutout` to raise `alphaRef` to 0.5. Both
  families (`TreeAdv` and `TreeAdvTrans`) are accepted there, so the *route* is the same for both
  models — the difference is what happens to a plant whose whole silhouette is sub-pixel twig
  drawn through that route.

The symptom set follows: dark brown twigs, one to two pixels wide at 8 m, whose kept texels are
mostly anti-aliased rim over bright sky ⇒ pale desaturated body; texels near the alpha threshold
flipping between kept and discarded ⇒ the black speckled holes and the frame-to-frame flicker;
and, because the model has no solid section, nothing that reads as a plant underneath any of it.

One contributor is **measured**: `WGR_CUTOUT_MIP_ALPHA` (default 0.5, the per-mip alpha gain added
for Arma 2 canopies). `WGR_CUTOUT_MIP_ALPHA=0` visibly restores twig definition and darkens the
bush. It does **not** remove the pallor or the speckle, so it is a contributor and not the cause.

## Ruled out (with the evidence)

| Candidate | Evidence against |
|---|---|
| The "untextured white" class (MAT-030 / MAT-054) | `grep -c "draws UNTEXTURED"` on the repro log = **1**, and it names `a3\structures_f\training\obstacle_climb_f.p3d`, not a plant. |
| The photo-card grass path | `use_photo_tuft=0 (clutterSource=0 mapAtlas=0 midPhotoTuft=0)`, `cards: drawn=0`, and `Wgpu grass placement: … NOTHING PLACED at this camera`. |
| DLSS / temporal | Frame-to-frame pixel change with a static camera is **worse** with `WGR_DLSS=0` (22.71% vs 19.74%). |
| Alpha-to-coverage | `WGR_FOLIAGE_A2C=0` produces a visually identical capture (`nerium-noa2c.png`). |
| The back-to-front blend pass / no-depth-write | `WGR_BLEND_PASS_TRACE=1` at the bush lists only `data3d\mc vojakw2.p3d` and `a3\structures_f\mil\offices\miloffices_v1_f.p3d`. The bush is not in the blend pass. |
| The retained / GPU-driven path | Both oleanders log `GpuModel foliage bail: … retained=0`. Both are on the CPU per-draw path; `WGR_TREEADV_RETAINED` is off by default. |
| A coarse LOD drawn up close | Settled LOD histogram at the capture is `lod0=1963 lod1=358 lod2..7=0`. |
| **Wrong map bound as base colour** (the incoming hypothesis) | **Killed.** GL33 — which applies no cutout at all and draws all three bushes as solid blobs — renders this bush the **correct dark brown of its atlas**. The right albedo is bound. `--test-model` also reports the face texture as `polyplane_neriumoleander_l1_dead_ca.paa` and the material as `no bindable Stage0 … faceTexture=1`, i.e. the face texture is the colour, as `TreeAdv`-family materials intend. |
| The RVMAT binding itself | Suppressing **every** production material binding (`WGR_MATERIAL_DEBUG_RVMAT=zz_no_such_material`, which makes `productionMaterialTarget` false for the whole CPU path) leaves the dead bush at mean RGB (132, 129, 125) against the live one's (91, 97, 87) — unchanged. The pallor survives with no material bound at all. |
| The texture histogram class being the discriminator | Both atlases classify `BLEND` with near-identical statistics (53.7% clear both; 43.9% vs 38.5% opaque). Classification does not separate the working case from the broken one. |
| The shader family being handled differently in code | `TreeAdvTrans` is aliased to `TreeAdv` at every branch site: `ShapeDraw.cpp:116` (`ShaderFamilyAlphaRoute`), `EngineWgpu.cpp:3732` (`GpuSectionMaterialIsCutout`), `GpuSectionMaterialUsesLeafCards`, and `ShaderSchema.hpp:237` (`kTreeAdvTrans` has the identical slot table to `kTreeAdv`). `EngineWgpu.cpp:5679` uses a `compare(0,7,"TreeAdv")` prefix, which also matches. So the family string is **not** the code-level discriminator, even though it is the only authored difference. |

## What is NOT proven

The chain from "sub-pixel cutout twigs through the Blend→cutout promotion" to the exact measured
pixel values. Specifically, the dead bush's dense interior renders around RGB (170, 165, 175)
against an atlas that is (68, 60, 52) everywhere, and I could not close that ~2.5× brightening
purely by argument. It is consistent with partial coverage over bright sky, but that is an
inference, not a measurement.

## The precise next step

Add a one-shot log at the end of the alpha-route ladder in `DrawSectionTL`
(`engine/WgpuRenderer/EngineWgpu.cpp:3386-3425`) that prints, per distinct section texture, the
**final** `blend`, `alphaRef`, `alphaClass`, `materialUsesLeafCards`, `materialCutout` and
`GpuTextureIsBarkOrTrunk`. Run the `--test-model` harness above and diff the two lines for
`polyplane_neriumoleander_l1_dead_ca.paa` and `polyplane_neriumoleander_l1_ca.paa`. That single
diff decides between the two remaining explanations:

* they differ ⇒ the promotion is not firing for the dead atlas, and the fix is in that ladder;
* they are identical ⇒ the route is right and the defect is the sub-pixel-coverage/mip-alpha
  interaction, in which case the lever is `AlphaMaskDemoteEnabled()` (turn the measured
  BLEND→CUTOUT demotion on for masks that pass the contiguity test — this atlas passes it:
  `clustered=1.000`, `bandWidth=2.19`) together with re-tuning `WGR_CUTOUT_MIP_ALPHA` for
  thin-twig cards.

That measurement needs a build; this session did not take one, because the owner had de-scoped the
item and a named model/material/texture plus the ruled-out list was the agreed deliverable.

## Harness notes worth keeping

* `--test-model … --test-world-freefly X Z Y az el` works **without** `--test-world` and is the
  cheapest way to put a suspect model beside a control with no terrain, no placement and no LOD
  governor in the way.
* `WGR_MATERIAL_DEBUG_RVMAT=<substring>` is not a pure observer: a non-empty selector sets
  `diagnosticSelector`, which turns `productionMaterialTarget` **off for every CPU-path section**
  and pulls matching shapes off the GPU-driven path (`EngineWgpu.cpp:6295`). A selector that
  matches nothing is therefore a useful "all materials off" control — but a selector that matches
  something is never a clean A/B of that one material.
* `WGR_ALPHA_SHAPE_TRACE=1` prints the per-texture alpha-shape verdict and whether the
  BLEND→CUTOUT demotion fired. It needs no build and should be the first thing anyone reaches for
  on a foliage transparency report.
* Every run in this session exited with a `Segmentation fault` inside `nvngx_dlss` during
  `LdrShutdownProcess`. It is at process teardown, after the screenshot is written, and is
  unrelated to this defect — but it means the shell exit code is worthless as a success check;
  check for the PNG instead.
