# Local curling-breaker prototype

The requested effect is a travelling swell whose lip curls into a hollow barrel and collapses. The old ocean remains the default. The new implementation is a local, opt-in prototype, not a completed replacement for coastal water.

## Implementation

`WGR_WATER_CURLING_BREAKER=1` adds a 36m folded sheet at the authored Nogova beach: 6144 triangles plus 512 small spray billboards. It advances shoreward over ten seconds, grows an approximately 2.7m crest, curls beyond vertical and progressively breaks along the crest. The lip falls before the rear face, with foam concentrated near impact and the wash following the underlying ocean displacement. An eight-step terrain search plus interpolated crossing follows the current tidal contour; the camera derives its framing from that contour too.

The sheet uses normals from its folded geometry and ripple detail in its tangent frame. The opaque scene snapshot cannot represent refraction through multiple water boundaries: the candidate uses approximate rear-water radiance, allowing a small amount of scene transmission only through the thin lip. This is a rendering approximation, not a fluid or layered-refraction solver. The spray is authored ballistic animation.

The optional depth-writing pass has a minimal bind group excluding scene depth. It invalidates subsequent depth resolves, which can add a resolve at MSAA4. Disabled mode creates no curling pipeline or pass. The original ocean shader body is unchanged from installed baseline `4de6ea57`; only an import and new entry points are appended. Physics, AI, boat queries, saved settings and the separate heightfield-surf opt-in are unaffected.

## Verification and limits

- Committed engine code `c9006e58` built using `Build.ps1` (`build/curl-open-build.log`). The production-WGSL GPU geometry test passed (`build/curl-open-geometry-test.log`), confirming actual overhang, shoreward travel, collapse, finite sampled values and a stable cycle. Shader composition was checked at `f3739d52`; final installed runtime exercised and validated the revised pipeline.
- Installed runtime completed cleanly with four stage captures in `build/ocean-runs/installed-curl-open-20260919-235705`. The barrel opening is visible. **The surface still looks too smooth/tubular to call natural finished ocean water.** No owner acceptance or completed coastal rollout is claimed.
- Final twelve-second screenshot-free comparison: WaterDraw median **3.173ms enabled / 3.0445ms disabled**; whole GPU frame **13.548ms / 13.4725ms**. Six logged timing samples per arm; no logged slow frames in either measured interval. Full-frame timing includes work outside the water timer, including a possible extra depth resolve. This short sample is within the provisional +0.5ms budget, not proof against regressions on other hardware, settings or coastlines. See `docs/roadmap/evidence/curling-breaker-20260919.json`.
- This remains one guarded fixture. It does not participate as folded geometry in planar reflections or boat collision/water queries. Material quality, owner acceptance, broader coastal placement and lifecycle coverage remain open. The current visual approximation must not become the default merely because geometry tests pass.

## Installation and review

Installed code: `c9006e58`, pushed to `safety/codex/ocean-release`. The owner session that blocked deployment was closed by the owner. Deployment and tests used the game lock. The stamp contains `+dirty` because report/ledger edits and the new review launcher were pending; engine sources match the committed tree, and SHA-256 checks confirm both installed binaries equal the built dist pair. The evidence manifest records these hashes.

`scripts/Start-CurlingBreakerReview.ps1` launches the installed prototype in a private profile, with no automatic timeout or screenshot capture. Close it before using `-Legacy` for the original ocean at the same calibrated location. The launcher refuses an installed build older than the current prototype. Normal game startup retains the old ocean.

The next decision is owner visual review of the local barrel shape; the next engineering work is improving its visibly procedural surface and integration before considering coast-wide placement. The old default remains until the owner explicitly accepts the new behavior.
