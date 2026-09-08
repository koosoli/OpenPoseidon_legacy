# Nogova long-range view, surface lighting and LLDR display

Status: implemented; author-run installed-game smoke checks, not owner validation.
Scope: the owner's 2026-09-08 Nogova report, following the lighting/sky handover.
Initial implementation: `6fc3f9d2`; follow-up code is in the commit adding this record.

## Changes

- VD-002: 50,000 m maximum AND factory default. New configurations do not allow a
  mission to silently reduce the preference. Existing saved preferences are preserved
  by loading; both modern Game settings and classic Video settings use the same limit.
  The owner's installed `game.cfg` was explicitly changed from 900/mission-priority
  to 50,000/no-mission-priority under the game lock. Other settings were preserved.
- Object DRAW distance follows view distance; the 5,000 m simulation limit and 500 m
  shadow limit are deliberately separate. This is not a 50 km AI simulation budget.
- Weather fog zero disables the distance-fog shader, rather than deriving fog from
  draw distance. Godrays remain independently switchable: their physical scattering
  is not weather fog, so enabling shafts can still add atmospheric light.
- Coastline stripes: cancellation-safe reversed-Z projection and depth reconstruction
  alone only reduced them. The scene now requests Depth32FloatStencil8 when supported,
  across all matching attachments/pipelines. Depth24PlusStencil8 remains a capability
  fallback. The RTX 3070 installed smoke reports the float format and has no long
  horizontal coastline bands in the 8 km aerial capture.
- Blended surfaces integrate shaft light only over the air in front of their fragment.
  They cannot use the background depth behind glass/decals. This preserves the earlier
  ordering fix which stopped opaque-looking signs/windows showing the background.
- The black nearby sign is `o/road/davle.p3d`, object 117398 at approximately
  [7764.2, 4412.2, 23.6]. It remained black with godrays OFF, GTAO OFF and direct
  rendering. Its albedo-only view correctly showed DAVLE. Normal mapping had introduced
  normalize(0) BEFORE shade's zero-normal fallback, poisoning both shading and cavity.
  The shared surface_normal helper now runs before normal mapping and in both 1x/A2C
  normal prepasses, for direct and retained geometry. Valid authored normals are not
  flipped. This is NOT the rejected two-sided-interior experiment from the handover.
- The held LLDR screen was absent: WGR_SECTION_SKIP_TRACE=lldr proved SKIP-FILTER on
  lldr_monitor.paa. A held cockpit proxy now submits all its own sections once, regardless
  of its parent's blend census. The screen uses the completed world colour snapshot,
  monochrome green phosphor and antialiased scanlines through the authored aperture.
  It is a monitor treatment of the main view, not a separately rendered/zoomed sensor.
- The AA-off smoke exposed an existing UI depth size mismatch at reduced render scale.
  UI now gets output-sized depth whenever the scene size differs, even without MSAA.

## Checks and Reproduction

- Build: win-x64-clang-rwdi, PoseidonGame and PoseidonTests, DLSS enabled.
- PoseidonTests `[ViewDistance],[GameSettings]`: 15 cases, 77 assertions passed.
- Rust `shaders::tests`: shared entry shaders compose; guard against pre-normalizing
  zero normals in either colour/prepass path passes.
- Rust depth-format feature/fallback test passes. All 12 godrays tests pass, including
  real-device pipeline creation. These are focused checks, not the entire test suite.
- UI depth sample-count/size regression test passes. Installed monitor captures pass
  at 4x and AA off (1x), including the reduced-resolution/output-resolution split;
  the final AA-off log has no renderer validation errors. Wait 30 s for the held
  weapon animation, rather than treating a 12 s capture of the HK as an LLDR test.
- All deployments and launches use scripts/with-game-lock.sh; only the paired binaries
  from scripts/Deploy.ps1 are installed. Launch OpenPoseidon.exe, never ColdWarAssault.exe.
- Test mission: Nogova, SoldierWSaboteurLaser, 1985-06-21 noon, weather 0.25, fog 0.
  Ground camera: --test-world-freefly 7764.82 4408.71 23.83 20 4.8.
  Aerial camera: same X/Z, height 8000, azimuth 20, elevation -45.
  Use --no-dev so the panel cannot overwrite WGR_GODRAYS; no --vd override was used.
  A separate POSEIDON_USER_DIR contained saved 50000/false settings; the startup log
  confirms `Visibility set: view=50000 objects=5000 shadows=500`.
- MSAA test trap: saved graphics.cfg overrides WGR_MSAA, and AA off is msaaSamples=0,
  NOT 1. An invalid 1 is ignored at renderer seeding and normalizes too late to test 1x.
- Local harness and detailed logs are in .tmp-nogova. Selected screenshots are in
  docs/roadmap/evidence/nogova-20260908. Captures precede the final commit, stamped
  6fc3f9d2+dirty, and are qualitative evidence, NOT whole-frame numerical A/B metrics.

## Limits

- A thin open-ocean seam and the outer ocean coverage edge remain in the aerial view;
  do not confuse those with the now-removed horizontal coastline bands. The earlier
  parked planar-reflection boundary is not claimed fixed here.
- No claim of every vegetation asset being visually approved. The reproduced sign is
  readable with shafts enabled; foliage still receives legitimate canopy/shadow contrast.
- No new 50 km performance benchmark: the older VD-001 timings measured up to 10 km
  and must not be extrapolated to this setting. Large worlds can cost more to render.
- Foreign Cargo.lock is untouched (SHA256
  9A4E74D791B26A4303E064867DA67B862C1439D0635DF3E19ED5A97AADF31A2E).
