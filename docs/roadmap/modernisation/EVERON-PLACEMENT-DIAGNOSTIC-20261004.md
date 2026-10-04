# Original Everon house placement diagnostic

`scripts/Test-EveronPlacement.ps1` prepares source-bound, installed-game evidence at the owner's exact camera, `5805.63 3597.97 46.35 147.8 6.3` (X,Z,Y,azimuth,elevation). It does not alter authored objects or implement a placement correction. Root owns execution and visual inspection.

The complete original `Worlds/eden.wrp` is checked against SHA256 `5a2550b27612fc4c8ccdec55ce2555c1ed23114d46f8b687ce5fb8b273515a79`, then decoded in memory with the existing checked SSCompress reader. OPRW2 contains 56,740 placements and 264 model names, but only 54,466 distinct authored IDs. Legacy load resolves duplicate IDs; the twelve selected candidates are individually unique in the original file. Exporter ordinals are not used as runtime IDs.

Candidates 6034–6037, 6042–6045 and 6047 are the farmhouse/house/chapel group around X7170–7196, Z2268–2393, 1.83–1.92km from this camera. ID7710 is a house at X7127.521,Z2059.055 (2.03km); IDs31997 and25191 are distant controls at 4.41/5.57km. The bounded source selection uses model-name families and a ±24° azimuth window, not an inferred screen FOV or exhaustive visible-building census. It does not identify the reported pixels.

Source-only inspection found negative foundation gaps across all examined visual LODs of these models against the original 50m grid. This is not installed rendered terrain evidence. Existing `Landscape::ObjectCreateFromShape` applies the classic negative-BoundingCenter anchor correction; ODOL7 points are already recentered. Subdivision preserves the terrain-relative anchor and updates retained instances after final positioning. Retained instances use the actual Object transform. The existing startup census only reports gaps greater than5m at an unrotated bounding-box bottom/centre, so it cannot exclude smaller footprint gaps or raster differences.

The runner checks the installed stamp and EXE/DLL hashes before and after a normal zero exit; a lock is required, and foreign sessions are preserved. It records exact returned camera vectors, paused simulation clock, unedited native height revision, physical soil/snow history, live owner frames and source identities around each PNG. Actual Y remains authoritative: a suspicious elevation is recorded rather than rejected against a predicted source Y.

The first tracked installed arm stopped on an invalid whole-native-response comparison: only `seaLevel` changed from 2.307034 to 2.982092 while simulation time8.636, native revision6, spacing6.25, range2048 and all soil histories remained equal. `GetSeaLevel()` returns `_seaLevelWave`, and `Landscape::Simulate` recomputes it from current sun/moon and waves, including after the fixture's date change. Sea level remains a finite recorded observation; native identity comparison checks the explicit readonly/baseline/source-ready/grid/revision fields. Self-tests allow that isolated observation change and still reject clock, revision, grid, edit-baseline, mud-history and nonfinite sea-level changes. This does not attribute any reported house gap to tides.

It captures the requested pose at the requested initial range (default8000m; minimum4000m), explicit8000m control, twelve near foundation views, four targeted far views, and return to the exact pose. `diag_geometry` uses the registered read-only `object <id>` expression with `draw=false` before and after views. Source visual-bottom extrema are transformed through each live owner frame and compared with actual native `dev_mud sample` SurfaceY. Four candidates additionally have 24 native terrain samples along the camera-to-predicted-roof line; these are discrete support observations, not continuous ray intersections or object occlusion results.

The geometry response describes CPU shape bounds/components, not selected pixel LOD. The screenshot API has no exact submitted-frame/owner association. There is no rendered-terrain-height readback here. These limitations remain explicit in `result.json`; successful capture is not a finding that a house floats or that its placement is correct.

Prepare without launching:

```powershell
pwsh -NoProfile -File scripts/Test-EveronPlacement.ps1 -SelfTest
pwsh -NoProfile -File scripts/Test-EveronPlacement.ps1 -SourceOnly -ExpectedCommit f6e5c1d5
```

Root runs the ordinary installed arm under `scripts/with-game-lock.sh`, passing the actual installed commit. It uses an isolated profile, clear16h conditions and the stock `parity_field.eden` mission. Results go under `build/everon-placement/`. No production engine change, arbitrary sinking, authored elevation change or bridge snapping is included.

## Installed findings, 4 October

Final diagnostic receipt: `build/everon-placement/everon-foundations-aimed-20261004-024010-087ded/result.json`, against installed `f6e5c1d5` / engine source `fd1e236c`. The normal exit and unchanged matched pair passed. All twelve candidates' sampled visual foundation gaps were negative: maxima range from -0.82 m to -3.41 m against the unchanged native surface. Preferred/tactical/renderer distances were checked, including the explicit 8 km control.

Root inspected the exact reported view, correctly aimed nearby village foundations, chapel and targeted far views. The reported view shows a beach and hills across water. The near village buildings sit on the ground; trees/terrain obscure parts of their far sightline. **The reported floating houses have not been identified or reproduced.** These candidates are not a substitute for the owner's actual target, and their support does not certify every map building. The owner was asked whether the houses were nearby/across water and whether terrain had been edited. Pending that detail, no placement change is justified.

Earlier tracked camera targets contained two fixture bugs: PowerShell member enumeration flattened world-point arrays before selecting Y; roof-target arithmetic also needed parentheses to keep Y and Z distinct. The final runner fixes both, adds falsifiers, and disables grass for foundation inspection. Earlier mis-aimed near/far captures are not visual evidence; their independently captured owner/native bottom samples remain diagnostic observations. The separate temporary pinned-camera close inspection agreed with the final village view.
