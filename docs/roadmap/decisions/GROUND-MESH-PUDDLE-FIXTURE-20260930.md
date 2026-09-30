# Grounded rigid puddle fixture: stock model, private class

Status: source and fixture-helper checks pass. Installed receiver admission,
physical cover and pixels remain **pending**. This does not establish puddles on
every placed ground mesh or every map.

The implementation being tested is 5c78233b, integrated by root as 9d3f73ae.
Root reports its canonical CPU/source policy checks and both actual-device
`ground_mesh` shader tests passed. The runner below has not been launched by
this agent; root owns the installed game and serial GPU work.

## Audited candidates

| Candidate | Actual stock evidence | Decision |
| --- | --- | --- |
| `HeliH`, `data3d/Heli_H.p3d` | Stock `HeliH : NonStrategic : Building : Static`, simulation house. Engine `Building` sets Static/Primary. Visual vertices have ClipLandOn, all at Y=0. | Genuine pinned ground decal; cannot validate the new independent rigid receiver. |
| `o/hous/Nam_dlazba.p3d` | Actual Nogova WRP paving model; resolution-2 visual LOD has 361 vertices and mixed On/Keep land flags. No named stock class found in decoded main or O config. | Whole-model pinned/mixed source is correctly excluded from the new receiver. Do not change production admission to make a fixture pass. |
| `data3d/Molo_beton.p3d` | Actual retail concrete quay, three visual LODs, no land-pin flags or animation. Solid horizontal concrete top in each LOD. No named stock class found. | Bounded stock **geometry** candidate using an explicitly private class alias; actual installed source tag remains mandatory. |

The new `Inspect-GroundMeshStockFixture.py` reads the bounded original member
from retail `Dta/Data3D.pbo`, using the repository ODOL7/PBO reader. It writes
JSON and a **config-only** PBO beneath the requested output directory. It never
copies or alters stock geometry, launches the game, or writes into installation
or normal user profiles. Python/NumPy are required, as for the existing stock rig
auditor.

Exact concrete payload facts:

- PBO stored member: 10,680 bytes, decoded 30,532 bytes.
- Stored SHA256: `b01c3bd018d39ecf9a96dd61ba0ec46987df2314606dbef1895fc96e437eca00`.
- Decoded ODOL7 SHA256: `ad8192875966fec2e0660b2c80087af21fde029b541aefdb811615b0ffd61e51`.
- Visual resolutions 1, 2, 6; vertex counts 296, 106, 72. No vertex land flags,
  animation frames or proxies; global AllowAnimation=false.
- All three tops have Y=`2.4916980266571045`. X bounds ±9.96665478 and Z bounds
  ±8.87326622. Each has seven flat upward top faces; ODOL normals are negated by
  the engine model loader. Top texture is `data/betonpoprask_top.pac`.
- Top source flags are `0x2020` at LODs 1/6 and `0x2000` at LOD2: NoClamp and
  optional NoShadow, without alpha, transparency, OnSurface, depth bypass,
  water or emissive bits. NoShadow is not proof of weather-map participation.

The actual `PoseidonTools model convert INPUT OUTPUT -v` load/save path was run
on an extracted read-only copy. It rebuilt legacy sections and preserved the
three visual bounds, top heights and bounding centre. Original input SHA stayed
unchanged. This supports placement, but runtime model identity/pose and visible
top still require inspection. The converted model is **not** used by the runner.

The private addon declares `GroundPuddleConcreteFixture : NonStrategic` with
`model="\data3d\Molo_beton.p3d"`. This is **not a stock CfgVehicles class** and
does not establish how normal world-owned quays are classified. Text config
parsing with the actual PoseidonTools config compiler and config-only PBO
roundtrip were checked offline. The runner requires actual mounted class,
debug model, Static/source receiver trace and exact first seated world pose. A
missing or rejected alias yields `fixture-unavailable`, not acceptance.

## Installed falsifier, owned by root

After installing the combined source build, from the worktree root in Git Bash:

```sh
LOCK_OWNER="ground rigid puddle falsifier" scripts/with-game-lock.sh \
  pwsh -NoProfile -File scripts/Test-GroundMeshPuddleRuntime.ps1
```

Default centre X=9486, Z=3006 is the already validated native terrain-puddle
positive region, including the roof test's strong stable world mask. This is a
candidate **pavement placement**, not a claim that this point naturally contains
a stock quay. Runtime ground is queried; the model top is seated at ground+.05.
Nine independent samples over a 3m square must lie within .12m of that top,
leaving margin under the actual .15m fragment gate. This is a bounded fit
diagnostic. The shader continues checking exact bare terrain at every fragment;
the fixture does not license the entire 20m model through one AABB probe.

The camera is X, Z-1.5, top+1, azimuth0/elevation-55. All control heights derive
from runtime terrain plus the audited loaded-model top, not an assumed absolute
camera altitude. The captures still need inspection to verify a common visible
concrete ROI with no hidden terrain substituting for the intended mesh.

The runner uses three independent private-profile arms: dry override0, wet
override1, and wet override1 with puddle master off. Actual weather rain/fog stay
zero; these isolate accumulated wetness response and do **not** test liquid
rain-hit ripples or natural accumulation/drying. Existing lifecycle tests own
that separate evidence. Fixed simulation time/date, wind/cloud, UV jitter,
exposure, grass-off, temporal-off and LOD range keep controls comparable.

Each arm records:

1. Actual class/model and initial seated source trace/pose; exposed top positive.
2. Paused static pair and move/return to the same top pose.
3. Real stock CampEastC floor-free cover, lifted 3m to avoid its Geometry LOD
   hull intercepting the solid pavement. Actual retained roof/probe and depth
   readback are required, then the same camera is captured again.
4. Removed roof return to exposed pavement.
5. Same pavement raised +1m with camera raised equally: within identical local
   surface framing, exact ground relation must reject it.
6. Same pavement raised +8m; ground+1 camera faces upward at elevation85,
   below the audited bottom near ground+3. This is an underside negative, not a
   downward-normal simulation or altered material.
7. Object restored to original seating and captured, then deleted normally.

Source telemetry is deduplicated per model/path/admission. The runner requires
its first trace to match the seated pose within .02m, and separately queries
later poses within .01m. If registration emitted an earlier unseated trace, it
fails honestly; it does not substitute a source tag from another instance.

Existing central-60% image diagnostics use threshold8. Exposed wet/dry change
must be nonempty (fraction >=.001), move/return mask IoU >=.97. Static, cover,
raised, underside, master-off, removed/restored returns require mean absolute
channel difference <=.5 and changed fraction <=.005. They do not segment a
material or prove every pixel is a puddle. Failure retains evidence without
relaxing thresholds. A passing result is `numeric-controls-pass-appearance-pending`.
Root must inspect concrete identity/ROI, shelter and negative geometry before
claiming the bounded installed case accepted. No cutout positive or normals
prepass attachment has been captured; those gaps stay open.

The runner requires the shared lock and rejects a foreign game. Provenance is
the first installed read. Exe/DLL hashes/stamp are checked before and after,
private addon/source/helper/script hashes are recorded, screenshots require PNG
signature/path/hash, and owned processes exit via the harness normally. Only
the exact owned PID can be stopped after a failed normal exit. Environment is
restored; there is no deployment or permanent profile mutation.

## Completed offline verification

- Actual retail-source auditor and exact SHA/source-face/LOD contract: PASS.
- Config-only private PBO readback, plus actual config text compiler: PASS.
- Actual model load/save top/bounds preservation: PASS (legacy section rebuild
  warnings retained in the source audit notes above).
- PowerShell AST syntax for all three new scripts: PASS.
- `Test-GroundMeshConcreteFixture.ps1`: PASS for actual helper queries/seating,
  raised and underside cameras, exact model/pose checks, refusal of poor fit,
  pose mismatch, wrong model and missing lock. No game/GPU was started.

Installed source admission, material/normal prepass continuity, actual physical
cover, pixel response, cutout silhouettes, ordinary map-owned paving, all-map
coverage, and rain ripples are not accepted by these offline checks.
