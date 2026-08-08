# PoseidonWGPU — CLD-015 distant cloud anti-repetition prompt

Ticket:
`CLD-015 — Distant cloud anti-repetition while preserving the approved cloud look`

Canonical roadmap:
`PoseidonWGPU_Modernisation_Roadmap_v4.5.3.md`

IMPORTANT:
This prompt is intentionally separate from COMPAT-C1.
Do not run it under `COMPAT-C1-EXEC-001`.
It is a validated task brief, not a current work order, until a dedicated cloud overlay/claim is activated.

======================================================================
OWNER VISUAL REQUIREMENT — NON-NEGOTIABLE
======================================================================

The owner LOVES the current clouds.

The current near- and medium-distance cloud appearance is considered VISUALLY APPROVED.

The defect is specifically:

WHEN VIEWED ACROSS LARGE DISTANCES / TOWARD THE HORIZON,
THE UNDERLYING CLOUD FIELD SHOWS NOTICEABLE TILING / REPEATED MOTIFS.

This task is NOT permission to redesign the clouds.

Target:

NEAR:
current appearance preserved.

MID:
current appearance preserved or extremely close.

FAR / HORIZON:
same cloud style and character, but repeated large-scale motifs are substantially less visible.

A solution that removes tiling but noticeably changes the approved near/mid clouds is a REGRESSION.

Do not broadly change:
- characteristic cloud shapes;
- softness;
- lighting response;
- self-shadowing character;
- weather preset character;
- wind motion;
- cloud altitude/deck behaviour;
- temporal stability.

======================================================================
CURRENT IMPLEMENTATION
======================================================================

The current cloud implementation ALREADY has:
- world anchoring;
- domain warping;
- different shape/detail scales;
- far-detail fading;
- a tileable 3D noise source.

Do NOT “solve” this by merely adding another generic domain warp.

======================================================================
DIAGNOSE FIRST
======================================================================

Determine the actual distant repeat source.

Investigate:
- finite 3D-noise tile period;
- same volume reused for weather/warp/shape/detail;
- channel correlation;
- axis-aligned sampling;
- effective combined repeat distance from current scale ratios;
- horizon compression;
- far march distance;
- wind wrapping;
- environment/reflection sampling;
- cloud-shadow sampling;
- quality/march undersampling.

Measure/estimate the actual repeat distance where possible.

======================================================================
PREFERRED INTERVENTION ORDER
======================================================================

Use the least visually invasive solution that works:

1. Decorrelate existing domains with stable world-space transforms:
   - rotations;
   - reflections;
   - axis permutations;
   - fixed offsets.
2. Add very-low-frequency, very-large-scale WORLD-ANCHORED macro variation
   if needed to break distant repetition.
3. Prefer the new anti-repeat influence to become relevant mainly at medium/far
   and far distances when that preserves the approved near field.
4. Use separate/larger noise resources only if source correlation/period is
   demonstrated to be the limiting factor.
5. Consider a bounded far-cloud representation only if simpler preservation-first
   fixes cannot remove the visible horizon repetition.

Potential tools:
- decorrelated coordinate transforms;
- non-axis-aligned sampling;
- huge-period macro coverage;
- stable procedural world-coordinate macro noise;
- independent noise channels/volumes;
- improved Worley/Perlin/fBm source generation.

======================================================================
DO NOT
======================================================================

Do NOT:
- redesign the cloud style;
- randomize clouds relative to the camera;
- make clouds swim/morph as the player moves;
- add per-frame random variation;
- hide repetition with blur or excessive fog;
- flatten the cloud field so much that the approved structure disappears;
- change lighting merely to mask the pattern;
- make water reflections or cloud shadows use a different cloud identity;
- accept a large GPU/memory regression without evidence.

======================================================================
MATCHED EVIDENCE
======================================================================

Use the same camera and weather seed/state for before/after captures.

Required:
1. CLOSE view proving the approved cloud look is preserved.
2. MEDIUM view proving the approved cloud look is preserved.
3. Ground 360° yaw.
4. Long horizon view.
5. Aircraft below cloud deck.
6. Aircraft above cloud deck.
7. Slow forward flight.
8. Sideways flight.
9. Deterministic accelerated wind/time.
10. Scattered/broken/overcast/storm.
11. Water reflection.
12. Cloud shadow.
13. GPU time and memory before/after.

Useful supporting evidence:
- coverage/warp/shape/detail/final-density debug views;
- repeat-distance measurement;
- autocorrelation / periodicity diagnostic.

======================================================================
ACCEPTANCE
======================================================================

Owner visual review must answer YES to BOTH:

1. “Do the clouds still look like the clouds I liked before at near and medium distances?”
2. “Is the distant/horizon tiling substantially reduced?”

If answer 1 is NO, the change does not pass even if tiling is gone.

The goal is NOT mathematically non-periodic clouds.

The goal is:
PRESERVE THE EXISTING CLOUDS
+
REMOVE THE DISTANT REPETITION THAT BECOMES VISIBLE ACROSS LARGE VIEW DISTANCES.
