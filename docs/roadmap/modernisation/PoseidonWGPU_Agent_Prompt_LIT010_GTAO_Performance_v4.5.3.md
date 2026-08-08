# PoseidonWGPU — LIT-010 GTAO performance optimisation task brief

Canonical roadmap:
`PoseidonWGPU_Modernisation_Roadmap_v4.5.3.md`

Ticket:
`LIT-010 — GTAO`

IMPORTANT:
This task is deliberately separate from `COMPAT-C1-EXEC-001`.
Do not mix GTAO optimisation with P3D/PAA/RVMAT compatibility work.
Use a dedicated renderer-performance overlay/claim when the integration owner activates it.

======================================================================
OWNER / QUALITY INTENT
======================================================================

The current GTAO is visually useful and should remain the quality reference.

The problem is performance at the real Tier-1 configuration, not a request to redesign the AO look.

Recorded Tier-1 reference:
- 3441 x 1440
- WGPU / Vulkan
- RTX 3070
- GTAO prep: 0.299 ms
- GTAO march: 6.335 ms
- GTAO blur: 1.089 ms
- GTAO total: 7.723 ms
- GPU frame: 24.178 ms
- GTAO: ~31.9% of measured GPU frame

Do not quote the old 800x600 ~8% figure as representative production cost.

======================================================================
REFERENCE PROJECT
======================================================================

Owner-local Esoterica checkout:

`C:\Users\mail\OneDrive\Documents\GitHub\CWR-CE-paavohuhtalafork-\inspiration\Esoterica-main`

Inspect the relevant GTAO rendering code, including:

`Code/Engine/Render/RenderPasses/RenderPass_GTAO.cpp`

Esoterica is a REFERENCE, not a renderer architecture to transplant.

Its GTAO path provides useful evidence around:
- half-resolution evaluation;
- reduced-resolution depth preparation;
- bilateral reconstruction;
- XeGTAO-style quality/performance choices.

XeGTAO is Intel code vendored by Esoterica; if code is copied/adapted, follow `CORE-007` and preserve the correct upstream licence/notices. Merely learning from the technique does not require copying its code.

======================================================================
PRIMARY EXPERIMENT
======================================================================

Keep the full-resolution path as reference.

Implement/evaluate:

full-resolution depth + normals
        ↓
edge-aware half-resolution preparation
        ↓
half-resolution GTAO horizon march
        ↓
half-resolution spatial denoise/filter
        ↓
depth/normal-aware bilateral upscale
        ↓
full-resolution scalar AO + bent normal

The expensive horizon march should execute over the reduced-resolution target.

Suggested development modes:
- OFF
- HALF_RES
- FULL_RES_REFERENCE

Do not make HALF_RES the default until matched evidence and owner review support it.

======================================================================
RECONSTRUCTION REQUIREMENTS
======================================================================

Do not simply average four unrelated depths/normals.

Protect:
- geometry edges;
- foreground/background separation;
- thin posts/fences;
- vegetation blades/clumps;
- contact grounding;
- stable bent normals.

Bilateral reconstruction should consider at least:
- full-resolution depth difference;
- normal similarity;
- spatial distance.

Avoid:
- dark halos;
- AO bleeding across silhouettes;
- loss of thin geometry;
- crawling edges;
- vegetation becoming dirty/dark;
- unstable bent normals.

Evaluate thin-occluder compensation or equivalent only when it improves real fixtures without over-darkening grass/foliage.

======================================================================
NO TEMPORAL EXPANSION IN FIRST PASS
======================================================================

This pass is:
HALF-RESOLUTION GTAO
+
GOOD SPATIAL/BILATERAL RECONSTRUCTION.

Do not add temporal accumulation/reprojection unless the spatial result is first proven inadequate and a later evidence-backed ticket/decision authorises it.

======================================================================
EVIDENCE
======================================================================

Use matched camera/scene/settings.

At minimum compare:
- FULL_RES_REFERENCE
- HALF_RES
- OFF where useful

Scenes:
- soldier/vehicle contact grounding;
- building corners;
- thin fence/posts;
- dense grass;
- tree foliage;
- open terrain;
- geometry discontinuities;
- motion/camera pan.

Record:
- GtaoPrep
- GtaoCompute/march
- GtaoBlur/filter
- reconstruction/upscale
- GTAO total
- total GPU frame
- VRAM/resource changes
- quality settings/slices/steps

Acceptance:
1. Substantial Tier-1 GPU-time reduction.
2. Owner judges the accepted GTAO character/contact grounding preserved.
3. No unacceptable vegetation/thin-geometry artefacts.
4. Full-resolution reference remains available until the new path is validated.
