# Dense physical rotor-canopy fixture

Installed `6cf9973e`, campaign
`build/rotor-land/clear-canopy-neutral-hover-20261001-040004-68426b`, remains
**FAILED**, with normal exit 0 and unchanged binary hashes. Its dry and snow
positive stages each observed four real emission rows. The covered stage first
recorded sheltered=8/emitted=0 twice, then sheltered=7/emitted=1 twice. Position
stayed near 2699.79–2699.82/5149.41, RPM=1, up approximately 1, neutral real pilot
and active hover. Aircraft clearance stayed above the existing conservative
envelope gate. This is a real cover miss, not flight/input or collision drift.

The covered rows' inferred phase-after values are 136/144/152/160, counting the
complete same-object trace from its initial phase. Phase itself is not logged,
so this inference is bounded evidence rather than a new runtime measurement.

`Object::Intersect(ObjIntersectView)` uses the physical ViewGeometry and convex
component face planes. Stock `stan_eastC.p3d` has ViewGeometry LOD **5**, 80
vertices, seven components. Its roof components 05/06 span X
**[-2.3900282383,+2.3971486092]**, width **4.7871768475m**. The old 6m placement
pitch leaves about 1.21m gaps between those roof components; even the complete
ViewGeometry width is only 5.3688m. Visual first-LOD bounds therefore cannot
establish a closed physical canopy.

The runner changes only its 25 real stock tents' X/Z offsets from
`-12,-6,0,6,12` to **`-8,-4,0,4,8`**. The 4m pitch overlaps the physical roof
width by about 0.79m. Common centre-ground datum, RoofLift0, exact actual class,
model and ASL proof, continuous aircraft clearance, neutral input checks, strict
all-eight hit requirement, zero covered emission, and 25-object deletion plus
renewed open emission remain unchanged. The canopy is test geometry; some outer
lower walls can be buried. No helicopter flight physics, rotor RPM, emission or
collision/shelter policy is altered.

CPU source inference uses the actual stock ViewGeometry component-selected face
corners, reconstructs finite plane clipping, and applies the production ring's
deterministic jitter at conservative radius12. Stock Noe source is exported with
PoseidonTools into ignored output; its 50m quantized heights are interpolated on
the actual anti-diagonal terrain convention. At the recorded centre2700/5150,
source ground is14.985m, matching the recorded runtime centre. Over phases128–159
and eight spokes, source supports range approximately14.985–17.876m: the slope
explains why an all-flat-bed approximation incorrectly hides the cover gaps.

For 256 finite probes at held centre2699.82/5149.41, helicopter Y24.8m and shared
canopy origin17.8150525284m, old pitch6 misses33; pitch4 misses0. These are source
inferences with reconstructed planes and quantized ground, not the actual
runtime transformed geometry, ray normals or emission acceptance. They justify
the candidate; only installed strict controls establish it.

Reproduce CPU evidence with `scripts/Inspect-RotorStockCanopy.py --game-dir
"D:/SteamLibrary/steamapps/common/ARMA Cold War Assault" --noe-rvw4 <ignored-Noe-export.rvw4>
--out <ignored-report.json>`. Produce the RVW4 via the existing CPU-only
`PoseidonTools pbo extract .../AddOns/Noe.pbo <ignored-dir> --filter noe.wrp`, then
`PoseidonTools terrain export-rvw4 <ignored-dir>/noe.wrp <ignored-export.rvw4>`.
The audit runs the existing strict ODOL7 parser before reading source faces.
`Test-RotorLandRuntime.ps1 -SelfTest` passes the existing input/clearance
falsifiers and new physical width/pitch contract. No agent game/GPU/build run.

Root must repeat the complete installed campaign, preserving failed status if
any actual probe remains exposed. The removed, high-altitude, wet-ground and sea
controls were not reached in the failed campaign and remain pending. Particle
appearance and existing accepted water downwash stay separate from this fix.
