# Targeted original Eden church snow acceptance

Source baseline for this fixture is `6cf9973e` plus the bounded fixed-church
receiver implementation `e2c60a9c`; root integrated that implementation in the
installed `335` pair. This change adds a runner and focused tests, without
changing production admission, powder, clock animation or physical exposure.

Root's existing `fixed-church-original-everon-20261001-041540-bf85d0` campaign
completed both tree arms with normal exits and stable installed hashes. Its
actual `kostel3` retained trace admitted the owner, direct clock textures were
vetoed, and root inspected the 500 m tree-enabled picture: the previously brown
central roof was white. This is a bounded distant positive. Near views, an
identifiable underside and the actual GPU material-bit veto still need their
own evidence; this document does not infer those results from a distant image.

## Source and installed fixture

`Test-ChurchSnowRuntime.ps1` uses the original `perf_field.eden` mission and
original installed `Worlds/eden.wrp`, whose SHA256 is pinned. It binds existing
WRP owner **4867**, `data3d/kostel3.p3d`, rounded source position
`[5056.6,32.9,3920]`, through the read-only `stream_identity_probe`. Actual
position, twelve-float frame, intactness, residency and collision/ViewGeometry
are recorded and required. The loaded model's decoded SHA256 is
`14fb625d820383b5e32b1e45d563c4eeace8caaed44c2d699bd0a3f64af9484c`.

The stock auditor records roof and clock local centres from real source
vertices. The runner transforms those centres using the live owner frame,
then queries terrain support for the cameras. Those coordinates are candidate
image aims, not proof that a roof face, clock hand or underside is visible.
In particular `interiorCandidate` may show a wall/floor, or a culled underside;
its negative cannot be accepted without root inspecting what is actually in
the picture. No synthetic owner, render loader, forced section, edited clock,
forced exposure or widened admission is used.

The two serial arms set `WGR_OBJECT_SNOW=1` and `0`, with the same stored 0.18 m
deposit, no falling flakes, dry weather, frozen simulation time 100, wind zero,
grass off, fixed cloud/LOD/jitter controls and normal engine exposure. Each
captures roof views at 30/200/500 m, top, clock, candidate interior and original
forest framing. A repeated near view follows the camera excursion. Snow is
then disabled without deleting the stored field, and re-enabled for restored
roof captures. The second arm requires actual zero object-snow uploads even
while the stored deposit remains present.

Strict functional gates include actual retained `eligible=true` and
`poseAllowed=true`, actual `hodiny_ruc` direct `sourceEligible=false` and
`churchFixedSection=false`, ready submitted physical-map telemetry, actual
deposit/master-off upload values, unchanged owner frame/field chunks/paused
time/weather, complete PNGs, installed pair hashes/stamp, ownership and normal
exit. Missing clock draw or owner evidence fails the run; it does not become
a successful negative. No pixel equality or appearance acceptance is claimed
by the runner. Captures are marked `captured-not-visually-accepted`.

Root execution, after confirming the installed matched pair:

```powershell
pwsh -NoProfile -File scripts/Test-ChurchSnowRuntime.ps1 -SelfTest
```

```bash
LOCK_OWNER="targeted original Eden church snow" scripts/with-game-lock.sh pwsh -NoProfile -File scripts/Test-ChurchSnowRuntime.ps1 -ExpectedCommit <installed-commit> -Label actual-original-eden
```

`build/church-snow/<label>-<timestamp>-<unique>/result.json` retains each pose,
PNG hash, actual owner/weather/snow state, source corpus, harness transcript,
source/head/script/helper hashes, profile and installed provenance. Root must
inspect roof positives at all three distances, ON/OFF/restored and return
controls, and visible clock/underside negatives before upgrading acceptance.

## Actual production material receiver falsifier

The new `gfx3d/church_snow_tests.rs` module extracts the real production
`SectionMaterial`, `VsOut`, constants and `retained_snow_receiver` declarations
from `gpu_driven.wgsl`. The CPU case validates that composition and requires the
same receiver in colour and normal prepass. The GPU case requires a real device
and uses Naga's actual member offsets rather than a copied byte layout.

Ten cases exercise solid and cutout fixed positives, bit128 clock veto alone,
bit128 combined with another material bit, reflective/emitter negatives,
rain-only receiver denial, emissive denial and zero-normal denial. Expected
readback is `[1,0,0,0,0,1,0,0,0,0]`. Original alpha/discard/A2C remain production
authorities; this receiver test does not claim a rasterized silhouette proof.

Root must register `#[cfg(test)] mod church_snow_tests;` in `gfx3d/mod.rs` and
run the filtered tests under its normal exclusive GPU ownership. No GPU or
crate build was run in the agent worktree. The local PowerShell fixture
self-test and exact stock C++ source auditor are the source-only checks.
