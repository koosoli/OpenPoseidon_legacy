# REN-TEMP-002 — The compute skin bake erases skinned characters on the DLSS route

**Date:** 2026-08-31
**Status:** FIXED (identity-world pack restored, bake default back ON; see Resolution)
**Affects:** every skinned character (soldiers, crew) whenever DLSS is active
**Fixed builds:** `21da5262` and later (deploy stamp 2026-08-31_00:17)

## Symptom

With DLSS active, skinned characters do not render **at all** — no body, no shadow —
while their rigid weapon proxies still draw. On screen that reads as rifles floating
in mid-air (vertical, at the proxy bind position) over invisible or half-buried
soldiers. First reported by the owner in campaign mission 1 (`00Training.Abel`,
the shooting-range scene); reproducible there deterministically.

Without DLSS the same build renders the same scene correctly, which is what made
this look like several different bugs before the common trigger was found.

## Root cause

`skin_bake_enabled` in `engine/WgpuRenderer/rust/src/lib.rs` defaulted to
`temporal_will_run`: the moment the temporal/DLSS path comes up, skinned meshes are
routed through the compute skin bake (`gfx3d/mod.rs`, `skin_bake.wgsl`) instead of
the vertex-shader skinning path — the bake exists to hand the temporal pass per-bone
motion vectors. On the OFP-generation skinned models (`data3d\mc vojakw*.p3d` and
kin) the baked output produces no visible draws at all. The 2026-08-30 17-word
vertex-stride fix in the bake shader evidently does not cover this corpus; the
suspicion is a bone-weight/vertex-layout mismatch in the ODOL-v7 chain versus the
shader's assumptions.

The weapon stays visible because a proxy is a separate **rigid** shape that never
goes through the bake — which is exactly why the symptom is "floating rifles" and
not "empty scene".

## Why the first diagnosis was wrong (worth remembering)

1. The bug was initially taken for a same-day regression and bisected across the
   day's commits — with runs launched from the build's `dist/` staging directory.
   `dist/` did not contain `nvngx_dlss.dll`, so DLSS silently fell back to native
   on every "reference" run: **all dist-side results were DLSS-off and green**, and
   the bisect implicated two innocent commits in sequence (the proxy-creation
   reorder, then the fast-allocator lock — the latter was even reverted on that
   false evidence in `c34a76ad`).
2. The reliable witness that DLSS actually ran is the known **NGX teardown crash**
   (`EXIT=139` after "Shutdown complete", stack in `nvngx_dlss`/`nvoglv64`). A run
   that exits 0 never initialised the DLSS feature. Check this before trusting any
   DLSS A/B.
3. The bug is **not** a regression of the `asset-obs-001` branch: yesterday's build
   (`ba26ba05`) reproduces it identically once DLSS genuinely runs. It has been
   latent since the skin bake was tied to the temporal path; no play-testing had
   combined DLSS with nearby AI soldiers until now. The owner's "strange ground
   texture artifacts at night with DLSS" report from 2026-08-30 may share this
   root and should be re-tested (chip `task_c060eaf7`).

## Deterministic repro

```
# one-time: extract the campaign (packing-aware PBO reader)
python <scratchpad>/unpbo.py "…\ARMA Cold War Assault\Campaigns\1985.pbo" <outdir>

# the lever: dist exe + the NGX snippet beside it + forced DLSS
copy "…\ARMA Cold War Assault\nvngx_dlss.dll" dist\x64-win-rwdi\
WGR_DLSS=1 dist\x64-win-rwdi\OpenPoseidon.exe --render wgpu --window ^
  --test-mission <outdir>\Missions\00Training.Abel ^
  --auto-screenshot "35s:out.png" --log-file out.log --log-level info
```

Read `out.png`: instructor soldier visible with his rifle = good; a lone vertical
M16 floating mid-air = broken. `WGR_SKIN_BAKE=0` with everything else identical
flips broken → good; `WGR_TEMPORAL=0` does **not** (it is the bake, not the
temporal accumulation). The log line
`wgpu compute skin bake enabled (WGR_SKIN_BAKE); VS skinning path bypassed`
says which path the run took.

## Mitigation shipped

`21da5262`: `skin_bake_enabled` defaults to **false**; `WGR_SKIN_BAKE=1` opts back
in for debugging. Cost while off: characters fall back to VS skinning and provide
no per-bone motion vectors to the temporal pass — slightly softer/ghostier moving
characters under DLSS, which is a fair trade against invisible ones.

## Resolution (2026-08-31, same night)

A background agent audited the full baked-draw chain and found the true defect —
not a shader/layout problem at all: the identity-world pack in `prepare()` was
left disabled (`false && bake_on`). The C++ side premultiplies the camera-relative
world into every palette matrix, so baked vertices leave `skin_bake.wgsl` already
world-transformed; the rigid pipeline the baked draw routes through then applies
`objects[instance].world` ON TOP. Commit `728f4035` had disconnected draw routing
from the bake and (consistently) disabled the pack; `60c9d63c` reconnected the
routing after the 17-word stride fix but never re-enabled the pack — so the world
was applied twice and every skinned character landed off-screen. First-person
hands stayed visible because their world is ≈ the camera (world² ≈ world).

Fix: the decision now lives in `object_gpu_for_draw()` (gfx3d/mod.rs), a
device-free seam mirroring `baked_draw_base`'s predicate exactly, pinned by unit
tests so routing and packing cannot disagree silently again. Verified on the
00Training repro with DLSS genuinely active (NGX teardown witness): instructor
visible, rifle in hand, shadow present, with the bake ON. The `WGR_SKIN_BAKE`
default is back to `temporal_will_run`.

Owner verified 2026-08-31: soldiers are correct again in the campaign.

The night lit-ground artifacts under DLSS are a **separate, still-open** defect —
owner re-tested on the fixed build and they persist; parked again by owner
decision (chip `task_c060eaf7`). So REN-TEMP-002 is closed and the night
artifacts keep their own track.
