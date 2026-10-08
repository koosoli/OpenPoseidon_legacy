# Local Worldgen source preparation

`scripts/Prepare-EarthWorldgen.py` invokes an external GPL-3.0-or-later
[CWR-Worldgen checkout](https://github.com/svamp3717/CWR-Worldgen). It combines
existing Earth feature/building source cells, samples existing cached Terrarium
elevation, and runs the actual upstream M4 generator with installed game assets
as read-only references. No public map service is queried and no stock asset is
copied into this repository. The generator's Python dependencies must already
be available. Preparation refuses an existing output directory or output under
the installed game.

Example (local checkout and dependencies required):

```powershell
python scripts/Prepare-EarthWorldgen.py `
  --worldgen-source D:/tools/CWR-Worldgen `
  --output D:/Worldgen-Lab/luxembourg `
  --asset-root 'D:/SteamLibrary/steamapps/common/ARMA Cold War Assault'
```

The source receipt records exact OSM cell hashes and the geographic extent.
Missing cached cells are not invented or claimed populated. Preserve upstream
OSM attribution and generated dependency/licence receipts. M4 is the baseline
pipeline; this does not imply all M9 terrain fitting/procedural assets were ported.

`Run-EarthWorldgen.py` narrowly handles the upstream M4 bridge-spec compatibility
regression: old dataclasses without the M9 bridge fields use upstream's existing
stock-bridge read-through proxy. New specifications and unrelated features are
unchanged. The Windows entry point is guarded for multiprocessing workers.

Actual local preparation `lux-region-03` completed in 11.55 seconds and actual
RVW4 parsing passed: 7369 placements and bounded selection of 2048 models.
The generator itself reports failed junction/road-overlap/grading/town-count
validation. This fixture is suitable for testing model rendering only; its
road geometry and gameplay terrain are not accepted.

Launch its real model references separately from simplified OSM previews:

```powershell
./scripts/Launch-WholeWorld.ps1 -Latitude 49.61 -Longitude 6.13 -Altitude 500 `
  -LocalSurface -WorldgenManifest '<output>/generated/manifest.json' `
  -WorldgenAddonRoot '<output>/generated/@CWR-Milestone4'
```

The installed-game test must establish model registration, actual images and
near-ground camera behavior before this is called a working visual demo.
One generated region is not worldwide generated-content streaming.
