# PHY-040 — Stock mesh admission before corpse articulation

**Date:** 2026-09-30. **Source baseline:** `codex/streaming-residency` `a001b55f`.
**Status:** reusable read-only admission diagnostics implemented; no runtime rig enabled.
**Previous:** [corpse articulation assessment](PHY-040-stock-corpse-articulation-assessment-20260930.md).

## Delivered prerequisite

`scripts/physics/inspect_stock_corpse_rig.py` now measures actual stock model
selection coverage and derives finite, enclosing capsule geometry. It decodes
bounded PBO/ODOL7 members in memory and reports every candidate's refusal or
success. It does not extract models, change game data, launch the game or create
physics bodies. NumPy supplies the symmetric covariance eigensolver.

The diagnostic provides geometry a future calibrated ragdoll can consume, while
explicitly retaining `runtimeRigAdmitted: false`. A finite capsule is an envelope,
not proof of anatomical proportions, a joint frame or visual quality.

The corpus is all 22 `mc ` P3D members in retail `DTA/Data3D.pbo`, covering
western/eastern/resistance soldiers, crews, pilots, snipers, saboteurs and four
civilian models. It is a complete bounded census of that explicit archive-name
selection, not a claim that every person model in expansion/addon PBOs was tested.
All 22 members decoded and parsed, with their stored and decoded SHA-256 hashes
recorded in [the evidence JSON](../evidence/phy-040-stock-rig-admission-20260930.json).
Every SSCompress checksum and full member consumption was checked.

## Why raw selection capsules need proxy exclusion

Stock graphical LODs contain attachment helper triangles. Some are weighted to
an anatomical bone: the flag's `proxy:flag_vojak.01` construction triangle is
strongly weighted to `hrudnik`. Including it in the capsule makes the chest
envelope extend far beyond the body. The initial measurement of
`mc vojakwo2.p3d` produced a chest radius of 1.63639366 m; after excluding the
attachment helper vertices it is 0.21001797 m.

Capsule fitting now excludes positive vertices in explicit `proxy:*` selections.
The complete selection/pose census still includes these vertices because weapon,
flag and other attachments must consume corpse transforms. This exclusion is
tested with a synthetic flag triangle 100 m from a small body. It is a generic
authored-data rule, not a special adjustment for one soldier model.

The tool fits a weighted PCA axis to positive raw bone-membership vertices and
uses the **maximum** perpendicular distance for radius. Axial endpoints span all
those vertices. Even a vertex with a tiny positive weight must be enclosed;
unselected zero-weight vertices do not affect the fit. Single-point, collinear,
nonfinite and zero-extent inputs refuse instead of manufacturing a collider.
The returned endpoints are raw ODOL7 model bind coordinates, before engine
recentering. `boundingCenter`, `autoCenter` and `lockAutoCenter` are reported;
no world transform or joint pivot is inferred from them.

The grouping covers pelvis, chest, head, four arm parts and four leg parts. Hands
and feet ride their forearm/shin envelopes for this proposed 11-body grouping.
Per-bone envelopes for all 23 anatomical selections are also reported, allowing
later hand/foot grouping to be evaluated without re-reading assets.

The corrected run produced 242 finite grouped envelopes and 506 finite per-bone
envelopes (all requested fits). Grouped radii range from 0.07252699 to
0.74340456 m. The largest is the western sniper's pelvis envelope: finiteness
and enclosure alone do not make that a useful anatomical collider. Clothing,
grouping and physical proportions still require admission checks before a solver
uses these envelopes. No fit is silently marked as a runtime-ready body.

## Coverage and remaining admission gates

The 22 models contain 241 LODs. Their finest graphical LODs all expose all 23
anatomical names. Source selection coverage accounts for every vertex using at
least one of the 25 stock person/equipment bones; no finest-LOD vertex is
unweighted. There are six vertices per model whose influences are equipment
only. The source census found no vertices with more than four positive bone
influences, or with only weights that quantize to zero, across all 241 LODs.
These are source diagnostics, not a replacement for runtime weight preparation:
`AnimationRTWeights::AddSelection`/`Normalize`, top-four ordering and synthetic
face rebinding remain the engine's authority.

Special LODs expose different selections and vertex sets, making a render-only
seam insufficient:

| Source special LOD | Vertices | Anatomically influenced vertices |
|---|---:|---:|
| Memory, 21 models | 19 | 11 |
| Memory, Guba | 18 | 10 |
| Geometry, 18 models | 216 | 192 |
| Geometry, 4 civilians | 192 | 192 |
| Fire Geometry, all 22 models | 332 | 288 |

Source View Geometry indices reuse Geometry in this corpus. Engine fallback and
LOD resolution must still be checked when wiring the runtime; the diagnostic
does not alter `LODShape::ScanShapes` semantics.

Nine of ten candidate anatomical boundaries have shared positive-weight
vertices in every model's finest graphical LOD. The tool records their weighted
overlap centroid as **anchor evidence only**. `bricho`/`zebra` has no overlapping
vertices in all 22 models, leaving the torso pivot unresolved. Even where an
overlap exists, it does not determine the anatomical hinge/cone axes. Every
joint candidate therefore remains `jointFrameAdmitted: false`.

## Smallest actual pose slice to build next

A useful next runtime change is a development action that snapshots and holds
the pose of **one settled local single-player corpse**, using one per-instance
affine base palette. This is a real consumer for the pose seam, and a foundation
for later rigid body deltas; it must not be advertised as an articulated corpse.
Authored deaths remain the default and unsupported/attached/water/network
corpses refuse activation. A bounded snapshot should carry skeleton/model
identity and a lifetime token, own its matrices, and clear on hide/delete,
revival, serialization and world replacement. Clearing before serialization
retains the existing authored save/network schema.

For actual articulation, preserve the full initial affine deformation and apply
a rigid body **delta**. Do not turn the initial authored matrix into a quaternion.
The earlier assessment's mapping remains applicable:

```text
D_i(t) = inverse(O(t)) * B_i(t) * inverse(B_i(0)) * O(0) * D_i(0)
```

The seam must replace the **base animation evaluation**, before existing
consumer-specific head/aiming/leg/face adjustments. A single fully modified
graphical palette cannot be reused blindly by every consumer:

- `Man::Animate` applies head/aiming transforms, graphical-only leg handling and
  level-specific face-parent matrices before `ApplyMatrices`, then
  `BasicAnimation` handles the head, wounds and bounds.
- `Man::AnimatePoint` evaluates original positions and applies aiming/leg
  adjustments separately.
- The weighted `Man::AnimateMatrix` preserves its existing final
  `Orthogonalize`; the selection/proxy overload then adds aiming/head blends.
  Orthogonalizing the stored base palette would destroy the nonrigid residual
  used by vertex and point evaluation.

Implement one immutable base source with these consumers retaining their
existing adjustment order. This requires substantive tests of identity delta
equivalence on nonrigid matrices, multi-weight vertices, special LODs, proxy
transforms and face-parent variants. Test refused/expired snapshots and repeated
clear/reload, then run the installed-game no-motion comparison before attaching
solver ownership. The present change deliberately implements the measured
admission prerequisite rather than inserting an unconsumed palette API.

## Reproduction and validation

From the checkout with Python and NumPy available:

```powershell
python -m unittest discover -s scripts/physics -p 'test_stock_corpse_rig.py' -v
python scripts/physics/inspect_stock_corpse_rig.py --archive 'D:/SteamLibrary/steamapps/common/ARMA Cold War Assault/DTA/Data3D.pbo' --out docs/roadmap/evidence/phy-040-stock-rig-admission-20260930.json
```

`--member` can be repeated to inspect a bounded subset. A candidate decode or
admission failure is recorded with authored-animation fallback, and the CLI exits
nonzero when not all requested candidates parsed. Unsupported ODOL generations,
bad indices/weights, duplicate archive members, checksum failures, truncated
payloads and trailing bytes fail closed. Repeated runs do not retain cached
geometry or backend handles.

Twelve synthetic tests cover complete ODOL/PBO traversal, full-weight fallback
for absent weight arrays, zero/partial/duplicate weights, top-four and quantization
hazards, weighted and rotated capsule enclosure, helper-triangle exclusion,
degenerate/nonfinite refusals, unresolved joint frames, corrupt wire/checksums,
bounded corpus selection and repeated-run stale-data fallback.

No game source or art changed, and no runtime smoke or visible quality claim is
made. The installed stamp read before the corpus measurement was
`codex/streaming-residency a001b55f 2026-09-30_10:47 deploy-ps1`.
