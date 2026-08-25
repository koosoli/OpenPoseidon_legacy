# HANDOVER 2026-08-26 — cockpits, rotors, and the cross-generation vehicle bridge

Written at the owner's request at end of a two-day session. The owner's LAST
playtest (on the deployed `5deb8f65` pair — verify with `DEPLOYED-FROM.txt`
before trusting anything) reports, verbatim intent:

- **A1/A2/A3 heli cockpit view: STILL too far behind, STILL transparent.**
- **A3 heli rotor: not spinning. A2 heli rotor: spinning wrong.**
- **OFP heli cockpit view: position FIXED, but cockpit still transparent.**

Treat that as the ground truth over this session's own captures — the static
captures verified less than play does.

## What shipped this session (all on `safety/feat/consolidated`, deployed)

| Commit | What |
|---|---|
| `2f8898bc` | Proxy conversion for ODOL 40–73; boardable A1 T-72; mission error-latch logging; `pbo pack --prefix` |
| `4a09d1f4` | vertexBoneRefs → named-selection synthesis (A2/A3/DayZ selections filled; A2 rotor turns at all because of this) |
| `1e90487c` | Five helicopters, one per generation (AST-016C) |
| `1dee99c7` | Reforger vehicles un-blackened (decal shells dropped at conversion) |
| `4bdc1704` | MAT-051 glass sheen (sky reflection on glass signature) + T-100 |
| `05d62e74` | THE cockpit-camera root cause: my absent-proxy probe was skipping 102 STOCK crew proxies (config-class proxies have no files); fixed — this is what fixed the OFP camera |
| `7236fa2f` | Cloud layer-2 defaults 1/1/match-1 (measured perf-neutral) |
| `5deb8f65` | Solid-panel classifier fix (no-clear high-mean AI88 ≠ Blend) + proxy-less cockpit camera fallback |

Suite 4320/4320 at every landing. Mods in the game folder: `@cwr_t72_a1`
(A1/A2 T-72s, T-100, V3S, four helis), `@cwr_uh1h_rf`, `@cwr_btr70_rf`.
Line-up mission `dev-missions\t72drive.abel` (3 mods semicolon-joined in one
`--mod`).

## OPEN — in the order the owner cares

### 1. Bridged (A1/A2/A3) cockpit camera still wrong in play
`Transport::InsideCamera` (TransportCore.cpp) now has a proxy-less fallback:
memory `driverview` → memory `pilot` → VIEW_PILOT `NamedPosition("pilot")`.
A static capture on the A1 UH-60 showed the camera move from metres-aft to the
cockpit bulkhead (`.tmp-a1-tank/a1cam.png`) — but the owner still reports
too-far-behind in PLAY, and the A2 Mi-35 / A3 MH-9 were never individually
verified. First moves: (a) log which chain step fired per vehicle (the A2/A3
helis may miss ALL three points and still land at origin — census their memory
LODs: `mi24_v.p3d`, `heli_light_01_f.p3d`); (b) check whether PLAY goes
through a different camera path than the parked capture (head tracking,
`_head.Init(Type()->_pilotPos …)` in Helicopter.cpp uses `_pilotPos`, which is
ALSO zero on these models — the head anchor may be the real offender in
flight); (c) the A3 convention is `pilotview` / memory `pos_driver` — extend
the fallback name list from a census, not a guess.

### 2. Cockpit "transparent" on every generation = MAT-052 (pale frames)
Fully dissected in
`docs/roadmap/decisions/MAT-052-cockpit-frames-are-pale-because-shading-not-routing-20260826.md`.
GL33 draws the Cobra canopy members solid dark camo; WGPU draws them as
washed-out ghosts. Frame texture is AI88 **luminance** (no colour of its own),
material fold shows `emi2.000`. Three ranked leads live in the record; the
landmine: `SVertex` has NO vertex-colour field, so if authored vertex colour
turns out to be the answer it is an ABI-touching mesh change. The two fixed
sub-mechanisms (solid panels mis-classed Blend; glass invisible at 11% alpha)
are already in `5deb8f65` / `4bdc1704` — do not re-fix those.

### 3. A3 rotors not spinning
The MH-9's rotors are PROXY SUB-MODELS (AST-016C census), not selections —
selection synthesis cannot reach them. Needs bone-driven proxy animation: the
proxy records carry `boneIndex` (already in the IR from `2f8898bc`); the
engine's ProxyObject transform is static. Design: animate `ProxyObject`
transforms from the same turret/rotor rotation the selections get, keyed by
the proxy's bone. This is a NEW engine capability, medium-sized.

### 4. A2 rotor spins WRONG
The Mi-35 rotor turns (synthesis works) but around a wrong pivot/axis.
`AnimationWithCenter` derives the centre from a CENTER SELECTION
(`"velka osa"` etc. — see `HelicopterType::InitShape`, `_rotorH.Init(_shape,
"velka vrtule", nullptr, "velka osa")`). On rev-48 the `velka osa` memory
selection is either empty (synthesis fills bone-named ones only — axis
selections are NOT bone names!) or mis-resolved. Probable one-line-family fix:
synthesis should also fill memory-LOD axis selections, OR
`AnimationWithCenter::Init` should fall back to the memory point of the same
name. MEASURE `Center(i)` per LOD first (log it) — this is the AST-016D
sibling and may share its root.

### 5. AST-016D — crewed-tank turret vanishes (both renderers)
The nine-theories-die record:
`docs/roadmap/decisions/AST-016D-the-simulated-turret-vanishes-and-nine-theories-die-20260825.md`.
Next probe is written there: per-section face submission inside the ANIMATED
draw path. Note item 4's wrong-pivot finding may reopen theory #4 (axes) via
the CENTER-selection route — `Turret::Animate`'s matrix came from
`obj->AnimateMatrix` and the probe showed identity, but the BODY animation's
centre selection (`OtocVez` centre?) was never checked. Cross-reference before
starting fresh.

## Do-not-re-walk (measured dead this session)

- The nine AST-016D theories (see the record).
- MAT-052: it is NOT alpha routing (classifier now correct for panels), NOT
  the pass-2 blend revisit (traced firing for both helis, WGR_BLEND_PASS_TRACE),
  NOT the sheen (that fixed a different sub-defect).
- The Reforger albedo suspects: BCR alpha AND colour transfer are measured
  clean; the black was decal shells (fixed).
- `WGR_CIRRUS_*` defaults: measured perf-neutral, do not revert for perf.

## Recipes that work (hard-won this session)

- **One game instance at a time. Always `Get-Process OpenPoseidon` first.**
  Parallel launches crashed the owner's claude.exe twice (memory:
  one-game-instance-at-a-time).
- Captures: detached `Start-Process` with redirected stdio, args with spaced
  paths quoted as `('"'+$p+'"')`, poll for the PNG inside ONE bash loop.
- Missions for this work, all in `dev-missions\`: `pilottest` (stock UH60
  moveInDriver — NOT moveInPilot, which does not exist), `pilotcobra`,
  `pilota1` (CWR_UH60_A1), `turrettest`/`turretnowatch`/`turreta1`/
  `turretstock` (crewed doWatch tanks), `helicanopy` (nose-on UH60+MH9),
  `t72drive` (the 11-vehicle line-up).
- Env doors: `WGR_GLASS_SHEEN=0`, `WGR_CIRRUS_PUFF/LOOK`, `WGR_GTAO=0`,
  `WGR_SKIP_TURRET_ANIM=1` (temp, in tree), `WGR_COVERAGE_TRACE=1`,
  `WGR_BLEND_PASS_TRACE=1`, `WGR_LEGACY_TEXTURE_TRACE=1`.
- Bridge recipes: AST-016B (+ its two addenda) and DZ-009 are complete and
  current. `addOns[]` in mission.sqm must name the CfgPatches class.
- The mission-boot error latch now LOGS its reason — read it before blind
  reruns.

## State at handover

- Branch `feat/consolidated` == `safety/feat/consolidated` at `5deb8f65`
  (plus this handover commit). Working tree clean apart from `.tmp-*`.
- Game folder deployed+stamped `5deb8f65`; suite 4320/4320.
- Witness captures under `.tmp-a1-tank/` and `.tmp-clouds/` (deletable, but
  MAT-052/AST-016D reference them by name — regenerate via the missions above).
