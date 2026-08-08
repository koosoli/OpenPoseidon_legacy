# COMPAT-C1 owner smoke test — 2026-08-08

**Verifier**: Oliver Kay (integration owner)
**Build**: `dist/x64-win-rwdi/PoseidonGame.exe`, launched via `scripts/Start.ps1`
**Covers commits**: `a72fbae`, `8bb6afd`, `08f3ac7`, `8a990e0`
**Result**: PASS — no visible change, no crash.

## What this verifies

AST-011A/B/C and AST-015 modified three code paths that existing OFP/CWA content
runs through, none of which are P3DM-specific:

1. `MLODLoader::loadLOD` — every LOD now routes on its own signature before the
   SP3X reader sees it.
2. `convertMaterials` — materials are keyed on the texture/RVMAT pair rather than
   the texture alone.
3. `convertGeometry` — `Vertex::uv1` participates in vertex identity, so the merge
   could in principle split vertices that previously combined.
4. `ModelCache::normalizePath` — cache keys are now the canonical virtual path.

Unit tests assert each is inert for SP3X: `uv1` stays zero, SP3X faces carry no
material reference so the pair degenerates to the texture, and the signature peek
rewinds. Tests can show the values are unchanged; they cannot show the resulting
model looks right. This smoke test is the part only the owner can do.

Mission loaded, soldiers, a vehicle and a building inspected: textures present,
no holes, no inverted or exploded geometry, no crash.

## What this does NOT verify

- **No P3DM model was rendered.** Nothing here confirms any Arma-generation
  content is correct — the renderer still cannot reach a P3DM model, which needs
  AST-020's derived cache first.
- **Winding remains unconfirmed.** `sourceWinding` is still
  `CW_REVERSED_ON_LOAD_ASSUMED` on P3DM LODs. This session rendered only SP3X
  content, whose winding was never in question.
- **No UV channel was seen mapped onto a surface.** That the second `#UVSet#` is
  the channel a material's `uvSource` selects is inferred from the format.
- Not a performance measurement. The vertex-merge change can only increase vertex
  counts on P3DM models, which were not drawn.

---

## Second run — after AST-013 (LZO / Pactext), same day

**Build**: `dist/x64-win-rwdi/PoseidonGame.exe` 12:14, deployed over
`ColdWarAssault.exe` in the retail install and launched directly.
**Covers commit**: `a800dc4`
**Result**: PASS — no crash, nothing visibly wrong.

This run mattered more than the first. AST-013 changed `Pactext.cpp`, which is the
live texture path *and* the font path, and removed a refusal in
`PacLevelMem::Init` so that a mip with bit 15 set is masked and accepted instead
of rejected. The claim that no OFP/CWA texture sets that bit is what the run
tested; menus, UI text and in-mission textures all rendered.

Still NOT verified by this run: nothing exercised the LZO path, because no
OFP/CWA texture is compressed. Compressed-mip decode is evidenced only by the
offline corpus check (124/124 Arma 3 sample textures through the shipped DLL), and
Pactext's own decode routines remain raw-only.
