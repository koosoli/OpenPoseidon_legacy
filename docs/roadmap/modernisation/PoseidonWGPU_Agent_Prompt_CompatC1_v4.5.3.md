# PoseidonWGPU — Compatibility Preview C1 implementation prompt

Repository:
https://github.com/koosoli/PoseidonWGPU

Integration target:
`new-renderer-infrastructure`

Canonical roadmap:
`PoseidonWGPU_Modernisation_Roadmap_v4.5.3.md`

Execution overlay:
`PoseidonWGPU_Execution_Overlay_CompatC1_v0.3.yaml`

Ledger merge seed:
`PoseidonWGPU_Status_Ledger_CompatC1_Seed_v4.5.3.yaml`

This prompt is ONLY for the Arma/Real Virtuality asset-compatibility programme.

`CLD-015` cloud anti-repetition is deliberately out of scope and held by the compatibility overlay.


======================================================================
PRE-ACTIVATION PREVIEW-0 INVARIANT
======================================================================

Before activating COMPAT-C1:

1. The historical Preview-0 overlay MUST still exist at:
   `docs/roadmap/modernisation/PoseidonWGPU_Execution_Overlay_Preview0_v1.2.1.yaml`
2. An untracked copy under `old/` does NOT satisfy this requirement.
3. If the file is merely deleted/moved in the working tree, restore the canonical tracked file from the current baseline/HEAD.
4. Run:
   `python scripts/validate_preview0_ledger.py`
5. Require PASS before changing COMPAT-C1 activation flags.
6. If the project intentionally wants to migrate the historical overlay later, do that as a separate verified migration that updates every validator/ledger/reference atomically. Do not mix that migration into compatibility implementation.

======================================================================
OWNER-PROVIDED REFERENCE CORPORA
======================================================================

Arma 3 Samples:
`D:\SteamLibrary\steamapps\common\Arma 3 Samples`

Arma 3:
`D:\SteamLibrary\steamapps\common\Arma 3`

Additional official/licensed/sample reference packages:
`C:\Users\mail\OneDrive\Documents\GitHub\CWR-CE-paavohuhtalafork-\inspiration\officialarma`

Inventory these roots before requesting additional material.

If a needed revision/example is missing, report:

1. exact file/package/model/material/animation type needed;
2. target generation/revision if known;
3. exact ticket/test blocked;
4. property the fixture must demonstrate;
5. suggested place for the owner to add it under `inspiration\officialarma`.

Continue all independent work.

Do not hard-code these absolute Windows paths into runtime asset identity.

======================================================================
OPERATIONAL START
======================================================================

1. Read the COMPAT-C1 overlay FIRST.
2. Read the live status ledger.
3. Read the canonical v4.5.3 roadmap definitions relevant to the assigned ticket.
4. Do not implement while the overlay is still template/PREPARED.
5. Resolve the real checkout baseline and merge missing ledger seeds without overwriting live records.
6. Activate the overlay atomically according to its bootstrap policy.
7. Claim only an authorised ticket.
8. The C1 P0 set is authorised by the overlay, but at initial activation **only `AST-007` is OPEN/claimable**; the rest begin BLOCKED until their real dependencies/evidence are satisfied.

Do not rewrite the old Preview-0 overlay. It is historical execution evidence.

======================================================================
P0 PROGRESSION
======================================================================

Use this as a priority/dependency guide:

`AST-007`
→ `AST-011A`
→ `AST-011B`
→ `AST-011C`
→ `AST-015` when virtual dependency resolution is needed
→ `AST-014`
→ first `MAT-030` shader schema
→ `MAT-020` semantics needed by the real fixture
→ one Super-material fixture
→ one grass/tree fixture
→ `AST-020` deterministic derived cache
→ `AST-021` golden/capability manifests
→ `AST-012A`
→ `AST-012B` one real later ODOL revision.

Do NOT interpret that line as a requirement to make every item globally serial.

Examples of valid parallel work after `AST-007`:

- `AST-013` if the chosen PAA fixture already exercises `SWIZ`/`FLAG`;
- `AST-015` if the chosen model/material references require virtual path resolution;
- targeted `AST-018` geometry-basis work if a P3DM fixture exposes a winding/selection/special-LOD issue.

Do not make a BLOCKED P0 ticket claimable simply because it is authorised. Unblock it in the live ledger only when the dependency/evidence recorded by the overlay is satisfied.

======================================================================
P3DM FIRST
======================================================================

The first meaningful direct-source proof is P3DM.

`AST-011A`:
- detect SP3X/P3DM per LOD;
- route safely;
- preserve existing SP3X;
- structured reject of unknown signature.

`AST-011B`:
- P3DM faces;
- variable texture refs;
- material/RVMAT refs;
- source winding/basis metadata.

`AST-011C`:
- `#UVSet#`;
- multiple UV sets;
- no UV1→UV0 collapse;
- RVMAT/TexGen can refer to the correct UV source.

Use an asymmetric fixture.

======================================================================
VIRTUAL RESOURCE PATHS
======================================================================

When the real fixture needs dependency resolution, advance `AST-015`.

One canonical virtual-resource representation should cover:

- `ca\...`
- `a3\...`
- addon/PBO prefixes
- optional leading root slash
- slash normalisation
- case-insensitive identity + collision diagnostics
- original spelling for diagnostics.

Host filesystem roots remain separate.

======================================================================
PAA P0
======================================================================

`AST-013` P0 is not “implement every PAA tag”.

Immediate correctness priorities:

- `SWIZ`;
- `FLAG` / alpha policy;
- mip hierarchy;
- storage/channel transforms.

Capture/report `AVG`, `MAX`, `PROC`, `OFFS`, and other verified tags initially unless a real fixture demonstrates that ignoring one changes correctness.

Storage decode comes BEFORE semantic normal/material interpretation.

======================================================================
MATERIALS
======================================================================

Text RVMAT and embedded ODOL material data converge into one material-source IR.

Use a shader-schema registry/equivalent.

Do not globally assign meaning to Stage0/Stage1.

First target:
ONE real `Super` material.

Then a grass/tree fixture.

Preserve relevant real-fixture semantics:
- base colour/alpha;
- normal;
- detail;
- macro;
- AO/shadow information;
- legacy specular/gloss;
- height/parallax;
- TexGen;
- UV set;
- render state.

Do NOT map SMDI directly to roughness.
Do NOT discard NOPX height.
Do NOT silently guess unknown shader stages.

======================================================================
ODOL
======================================================================

`AST-012A` is architecture only:

- read exact version/revision;
- dispatch exact known parser;
- preserve existing OFP/CWA support;
- reject unknown safely;
- route to common IR.

It does NOT mean “fully support all Arma 1/2/OA/3 ODOL”.

`AST-012B` selects ONE exact later revision from a real fixture.

Record:
- exact revision;
- exact fixture;
- exact implemented subset;
- unsupported fields;
- capability result.

A second unknown revision must reject safely.

======================================================================
P1 — DO NOT BLOCK STATIC C1
======================================================================

Unless the chosen static C1 fixture genuinely requires them, keep these out of the first vertical slice:

- `AST-016` model.cfg / skeleton / AnimationSources;
- `AST-017` BMTR/RTM extensions;
- `AST-019` BISurface / broad CfgSurfaces / clutter gameplay semantics;
- `AST-022` WRP/OPRW expansion.

They remain roadmap work, not forgotten work.

======================================================================
CAPABILITY CLAIMS
======================================================================

Never report only:

“Arma 3 supported”
or
“Arma 2 compatible”.

Report a matrix item such as:

- `P3DM_STATIC_VISUAL: VALIDATED — fixture ...`
- `MULTI_UV: VALIDATED — fixture ...`
- `SUPERMATERIAL: PARTIAL — NOHQ/SMDI yes, parallax unsupported`
- `ODOL_REVISION_<N>: PARTIAL`
- `BMTR_V4: UNSUPPORTED`

Every claim names:
fixture/hash/revision/capability/limitations/verification commit.

======================================================================
FIRST VERTICAL SLICE
======================================================================

Prefer:

ONE P3DM source asset
+
its RVMAT
+
its PAA textures
↓
PoseidonFormats
↓
common ModelIR + RvMaterialSourceIR
↓
MaterialIR + RenderStateIR
↓
derived cache
↓
WGPU.

Show:
BASE COLOUR ONLY
versus
FULL TRANSLATED MATERIAL.

Then add ONE grass/tree fixture through the same path.

Then establish ODOL dispatch and ONE later ODOL revision through the same common IR.

No Arma-specific renderer fork.

======================================================================
TESTS
======================================================================

Use:
- asymmetric geometry;
- asymmetric normal maps;
- multiple UV sets;
- one Super material;
- one vegetation material;
- malformed/count/bounds tests;
- cache reload tests;
- golden semantic manifests.

Compare facts, not screenshots alone.

======================================================================
DELIVERABLES PER CLAIMED TICKET
======================================================================

Return:
1. baseline + implementation commits;
2. changed paths;
3. production call sites;
4. fixture(s) used;
5. source hashes/revisions;
6. semantic/golden manifests;
7. unsupported/lossy data;
8. tests;
9. before/after captures where visual;
10. CPU/GPU/memory evidence where applicable;
11. fallback;
12. live ledger update;
13. overlay promotion request if a next candidate is ready;
14. exact missing-fixture request if one is needed.

Guiding rule:

PARSE ACCURATELY
→ PRESERVE SOURCE INTENT
→ NORMALISE ONCE
→ TRANSLATE DETERMINISTICALLY
→ VALIDATE ONE CAPABILITY AT A TIME
→ NEVER TURN ONE SUCCESSFUL FIXTURE INTO A GENERATION-WIDE CLAIM.


======================================================================
C1 FOLLOW-ON — DO NOT BLOCK THE FIRST VERTICAL SLICE
======================================================================

`AST-023` defines the later automated compatibility conformance harness.

Do NOT front-load it ahead of the first real C1 model/material result.

During C1:
- produce deterministic fixture IDs/hashes;
- produce `AST-021` semantic/capability manifests;
- emit `TEST-007`-compatible failure/reproduction information where infrastructure already exists.

After those inputs are stable, `AST-023` can turn the curated OFP/A1/A2/OA/A3 corpus into automated conformance and regression testing.

Any compatibility percentage MUST name its exact capability/fixture denominator.
