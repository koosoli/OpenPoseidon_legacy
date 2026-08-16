# Handover — paste this whole file into a fresh session

> **WHERE THE WORK IS.** Branch **`feat/texture-streaming`**, in the worktree:
>
> ```
> C:\Users\mail\OneDrive\Documents\GitHub\CWR-CE-paavohuhtalafork-\.claude\worktrees\texstream
> ```
>
> **Work there, not in the main checkout.**

## Where to push

**Push after every milestone. Never let work sit uncommitted — an unattended crash with hours of
uncommitted work is the one genuinely unrecoverable outcome here.**

```
git push safety feat/texture-streaming
```

| remote | URL | use |
|---|---|---|
| **`safety`** | `https://github.com/koosoli/PoseidonWGPU-private-safety-mirror.git` | **Default. Push here, freely and often.** Private mirror. |
| `origin` / `poseidon-wgpu` | `https://github.com/koosoli/PoseidonWGPU.git` | The real repo. **Only push here if the owner asks.** |

Commit messages on this branch carry the measurement and the reasoning, not just the change. Match
that. If a number is in the message it must come from a run, not an estimate.

## How to talk to the owner

**Be concise and plain. He has said so directly, twice.**

- Lead with **what is fixed** and **what is not**, as a short list or small table.
- When something is not done, say **why** in one line.
- Prove fixes with a **picture or a number**, not a description.
- No walls of text, no narrating your process, no repeating the diagnosis he already has.
- If you were wrong about something earlier, say so in one sentence and move on.

He does not want root-cause essays. He wants working software and a straight answer about its state.

---

# THE ONE INSTRUCTION THAT OVERRIDES EVERYTHING ELSE

**The owner's words: "i do not care about finding root causes, i want it fixed."**

The previous session was good at diagnosis and bad at shipping. It produced eleven root causes and
**four** user-visible fixes. Almost everything below is already diagnosed to a file and a line —
your job is to **apply the fix, build it, verify it with a capture, and confirm it to the owner
with a picture or a number**. A root cause with no shipped fix is worth nothing to him.

Rules that follow from that:

1. **Do not re-diagnose anything in the "READY TO SHIP" list.** The work is done. Ship it.
2. **Every item ends in a deployed binary and a verification capture**, not a decision record.
3. **Use subagents aggressively and in parallel** — the owner asked for this explicitly. Rules that
   made them work last time: disjoint file ownership stated in the brief, who else is editing right
   now, **DO NOT BUILD / DO NOT RUN THE GAME** (you own the machine, they hand you env + poses),
   and **ask for null results explicitly**. The most valuable outputs last session were agents
   refusing the premise they were handed.
4. **Do not stop.** If an item is blocked, move to the next and come back.

---

# READY TO SHIP — diagnosed, fix known, NOT yet delivered

## 1. Reforger rocks show only their backside — HIGHEST CONFIDENCE FIX IN THIS FILE

**Every Reforger model has its vertex normals AND its triangle winding inverted, together.**
Measured absolutely (does a stored normal point away from the centroid on a closed solid):
`graniteboulder_01` 5156/5156 faces outward, `graniteboulder_02` 3732/3732,
`granitebeachstone_01/02/03` 3874/3874, 3526/3526, 2298/2298. `.xob` normals are **outward**; this
engine's are **inward** (`MeshBuild.cpp:43-44` negates on the way to the GPU while
`shader3d.wgsl:226` declares the result "outward"; `Odol73.hpp:1004-1009` decodes ODOL with a
deliberate negative scale whose comment says dropping it "would flip every normal in the model",
while `XobModel.hpp:115-117` uses a positive one).

**The fix already exists and is default-off:** `XobCommand.cpp` `--fix-orientation` /
`XOB_FIX_ORIENTATION=1`. It negates the written normal **and** swaps corners 0/1 — deliberately one
flag, because either alone trades one wrong for the other.

**What you must do:** rebuild `PoseidonTools`, back up the rock `.p3d` tree, re-export the rocks
with the flag, copy over the game tree, capture, confirm. The tool now prints `nrm <f> out` per
model — **without the flag it prints 1.00, with it 0.00.**

```
PoseidonTools xob convert "<Reforger>\addons" -f "rocks/granite" -o <scratch> `
  --virtual-prefix "reforger\everon\models" -n 200 --fix-orientation --textures
```
Capture at a rock field: `--test-world-freefly 3800 7960 4 40 -5`
(8,981 `granite_cluster_01` around 3810.6/7974.0; 10,853 `granitebeachstone_01` around 4420.9/9307.3).
**Expected: rocks become solid, correctly silhouetted, and lit bright on the sunlit side.** If they
become solid but *dark* on the sunlit side, only the winding half took.

**Free pre-check, no run needed:** the shadow pipeline is already `cull_mode: None`
(`gfx3d/mod.rs:4429-4431`). In any existing Everon capture, if the rocks' *shadows* are solid and
correctly shaped while the rocks are see-through, this is confirmed before you launch anything.

## 2. Reforger buildings partly in the ground — SAME SHAPE, SAME CONFIDENCE

A converted `.p3d` carries no named properties, and `autocenter` defaults **ON** when absent, in
both implementations (`Model.cpp:64-108` and `ShapeLOD.cpp:413-441`). It subtracts the bbox centre
from every vertex and records the offset nowhere; `ShapeAdapter.cpp:723-745` restores
`_boundingCenter` only for `sourceFormat == "ODOL"`. Nothing puts it back, because
`LandSave.cpp:2467` passes `preserveAuthoredElevation` for OPRW25, so the one place that re-grounds
the authored origin never runs.

Measured sink, LOD0 point table: `shophouse_e_2i01t` **4.83 m**, `house_prefab_2i02t` 3.71,
`farmhouse_e_1l01` 3.38, `garage_e_01` 1.07, `birdhouse_01` 0.01. The negative minima are foundation
skirts, and across 304,035 placements the median of (object Y − terrain Y) is **−0.004 m** — the
authored Y already puts the *origin* on the ground. That is exactly "lots of buildings are partly in
the ground", including which ones.

**Fix exists, default-off:** `--fix-origin` / `XOB_FIX_ORIGIN=1` writes `autocenter=0` on every LOD.
Re-export `-f "structures"`, capture at `--test-world-freefly 4535 10760 20 50 -3`.
**Expected: each building rises by exactly its printed `originY`; flat props must not move at all.**

## 3. Cockpit body is translucent — root cause found, POLICY DECISION NEEDED THEN SHIP

The owner corrected an earlier misreading: **it is not the glass**, it is the solid cockpit body.

`TextureWgpu::Init()` (`TextureWgpu.cpp:616-620`) calls `_src->ForceAlpha()` for **every**
ARGB4444/AI88/ARGB8888 texture, overriding the FLAG tagg that MAT-047 established is the only real
alpha signal. So `ShapeSetup.cpp:727-732` stamps `IsAlpha|IsAlphaOrdered`, the descriptor becomes
`AlphaBlend`, and `ClassifyAlpha`'s ≥2%-partial rule returns **Blend**. Measured on `oh58.pbo`:
**28 of 44 textures classify Blend — the solid body** — with mean alpha `kiowa_insidtga` 10/255,
`kiowa_inspanl` 11, `kiowa_in_doors` 12, `kiowa_inspanr` 23. The Kiowa interior is being submitted
alpha-blended at 4–20% opacity.

**The blend-depth change shipped last session made the symptom worse** (honest finding from the
agent that wrote it): with depth-write on, the first blend section occluded the rest so you saw one
translucent layer; with it off, all 28 composite. `WGR_COCKPIT_BLEND_LEGACY=1` is the A/B.

**Why it was not fixed:** the authored data offers no clean discriminator — the Kiowa's *real* glass
(`oh58_sklotest*`) has no FLAG tagg either, so simply gating on FLAG would turn the side windows
opaque. **That is a judgement call, and it is yours to make and ship.** A defensible approach:
gate on FLAG *and* keep a texture-name or alpha-histogram escape for genuine glass.

**Repro exists:** `scripts/cockpit-capture.ps1` (already written and verified against the SQS
commands). A free camera inside a helicopter does **not** show a cockpit —
`World.cpp:1568-1570` selects the cockpit LOD only when `obj == camInsideVehicle`, so the script
seats the player. **Run it; it has never been run.**
```powershell
.\scripts\cockpit-capture.ps1 -Label kiowa-new    -Vehicle OH58
.\scripts\cockpit-capture.ps1 -Label kiowa-legacy -Vehicle OH58 -Env @{ WGR_COCKPIT_BLEND_LEGACY='1' }
.\scripts\cockpit-capture.ps1 -Label kiowa-gl33   -Vehicle OH58 -Backend gl33
```
GL33 has the identical `ForceAlpha`, so it should show the **same** translucent cockpit. If GL33 is
clean, the whole diagnosis is wrong.

## 4. Arma 2 roads — root cause proven, current fix is not shippable, better fix named

**Roads never run the 3D shader.** `ShapeDraw.cpp:503-506` denies a vertex buffer to any
`OnSurface` shape, `RoadType` sets `OnSurface` on every road of every generation, so `_buffer` is
always null, the T&L gate fails, and the road goes to `EngineWgpu::DrawSection(FaceArray&)` which
converts to **screen space** and submits to the **2D pipeline** — `textureSample * color` mixed
toward fog. No lighting, no sRGB decode, no conform. One fact explains white-by-day (gamma-space
colour into an HDR target, ~2.3× at mid-tones), glow-at-night (legacy CPU lighting double-exposed),
see-through (nothing conforms the road), and why `ROAD_TRACE` never matched a road.

`WGR_ROAD_CPU_SPLIT=1` **works and is verified**: `ROAD_CPUPATH` 42→0, `ROAD_TRACE` 68→320, road at
night (41.9,42.1,45.7)→**(0.1,0.1,0.1)** against terrain's (0.2,0.2,0.1). But it renders the near
road as a **dark scalloped, segmented ribbon** — the CPU `SurfaceSplit` emits one piece per terrain
triangle and they shade inconsistently — and it allocates a unique `Shape` + vertex buffer per
visible placement. So it stays default-off.

**The fix to ship instead:** let roads **keep** a vertex buffer and be GPU-conformed like everything
else — remove `OnSurface` from the `optimizeHW` deny at `ShapeDraw.cpp:503` and let the existing GPU
conform handle it. `WGR_CONFORM_RASTER=1` (already implemented) makes that conform agree with the
rasterised terrain lattice. No CPU split needed. **This is the highest-value unshipped road work.**

## 5. Auto-exposure is default-ON — verify it does not pump, then it is done

Shipped this session at the owner's request. Measured: Takistan hour 8 worst-clipping pose,
mean luminance 155.1→123.9 and **23.17%→0.59%** of pixels clipped; Stratis 10.62%→0.17%. Controls
are in the dev panel **Tonemap** tab, including a new framerate-independent "Adapt time (s)"
(`ExposureSettings::rateTau`, default 0.4 s; `rate` was per-frame and adapted twice as fast on a
45 fps world as a 20 fps one).

**The one untested risk is pumping on camera turns — the exact reason the original author disabled
it.** A still capture cannot show it. Run `scripts/farfield-movecam.ps1` (30 m/s and 5 m/s arms
already exist) and score frame-to-frame luminance delta. If it pumps, **narrow Min scale first** —
0.55..1.10 measured within 0.2% of the full 0.25..4.0 range, so almost all the benefit survives a
much narrower band. Turning it off is the last resort, not the first.

## 6. Streaming stall — implemented, never measured

The owner's 4 fps is **vehicle-speed only**: 30 m/s gives a median second of **4 frames**, 5 m/s
gives 46.6 fps (*faster* than standing still). Median frame time barely moves (25.0→24.0 ms); all
damage is in the tail, 74% of wall time inside frames >100 ms. Cause: residency recentres on a
4-cell quantised boundary, the set is already at cap, there is no cache ("what the camera left
behind must be rebuilt"), and a mandatory floor of 128 admits makes the 4 ms budget unreachable.

A shape-level eviction cache, velocity prefetch and an admission ceiling are **implemented and
default-on** (`WGR_OBJECT_STREAM_EVICT_CACHE=256`, `WGR_OBJECT_STREAM_MAX_MS=16`,
`WGR_OBJECT_STREAM_PREFETCH_SECONDS=2.0`, `WGR_OBJECT_STREAM_PREFETCH_MIN_SPEED=8`). **Nobody has
ever measured them.** Re-run the 30 m/s arm and compare against the numbers above. This is the
owner's single biggest performance complaint — measure it.

---

# OWNER REQUESTS AND REGRESSIONS FROM THE LAST HOUR — newest, least investigated

## R1. Sea level appears to have RISEN (Arma 3) — NEW REGRESSION, NOT INVESTIGATED
Owner, testing Arma 3: "for some reason the sea level rose." Nothing in this session knowingly
touched sea level. Candidates, in the order I would check them:
- the water surface work (`rust/src/water/**`) — foam band and patchiness changed, but those are
  shading terms and should not move the plane;
- `ShapeAdapter`'s `autocenter` / `mapType` changes (shipped this session) — these move OBJECT
  origins, so if the *shore* objects moved rather than the water, it would read as a higher sea;
- auto-exposure making shallow water read brighter and therefore *look* wider.
**Bisect it.** The branch has ~30 commits this session and each is self-contained; a capture at a
coastline on two binaries settles it far faster than reading. Stratis coastline pose:
`--test-world-freefly 2200 5400 220 45 -8 --mod a3stratis`, hour 10.

## R2. Sunlit walls blow out — owner explicitly dislikes it
"the sun when it hits a surface like a wall from a house, it can totally overexpose it... this i do
not like." This is *local* overexposure on a lit surface, not the global sky white-out that
auto-exposure already fixes. Auto-exposure meters the whole frame, so a single bright wall does not
move it much. Look at the material/lighting side: specular or diffuse response on a sunlit wall,
and whether the tonemap shoulder is doing any roll-off at all at the top end. Related and already
known: Arma 2 vegetation ships an authored `emissive` of 2.6, and materials generally may be
carrying values the shading model treats as absolute radiance.

## R3. FEATURE — moonlight. The owner wants this.
His words: **"i want that the moon is creating light for the game world... not much but a little, as
long as the moon is out the game world should not be total black at night, a bluish light that casts
shadows and water reflection would be nice."**

Three parts, and they are separable — ship them in this order:
1. **A dim bluish ambient/directional term while the moon is up**, so night is not pure black. The
   engine already tracks the moon fully: `EngineWgpu.cpp` logs
   `moon: dir=(...) az=... el=... | lit=... radius=... discScale=... draw=YES/NO earthshine=...`
   every frame, and `moon: ephemeris=on sun=off lighting=on manual=off ... intensity=0.0600`
   shows a moon *lighting* path already exists with an intensity knob. **Start by finding out what
   that path currently contributes and why it reads as zero.** Night captures currently measure
   terrain at (0.2, 0.2, 0.1) — essentially pure black — so either the term is off, or it is far
   too small, or it is being multiplied out.
2. **Moon shadows.** The cascade system is sun-driven; `SceneDraw.cpp:2318` computes
   `sunFactor = 1 - NightEffect()` and **skips the shadow depth pass entirely once
   `sunFactor <= 0.01`**. So at full night there are no cascades at all, by construction. Driving
   the cascades from the moon direction when the sun is down is the change.
3. **Water reflection of the moon.** The water already has a sun specular path
   (`godot_water_fresnel`, `GODOT_LIGHT_ROUGHNESS`) — feed it the moon as a second, much dimmer
   light rather than writing a new one.

Do NOT make night bright. He said "not much but a little". Measure it: terrain should come off 0.2
to something like 3-8, not to 40. Verify with a night capture and quote the number.

---

# OPEN — no root cause yet, do not assume the previous guesses

## Black triangular wedges inside buildings (Arma 2, "seen in many buildings")
Hard black wedges hanging down interior walls, one per roof beam. Inside them the value is exactly
`(0,0,0)` — the scene target's clear colour — while genuinely shadowed wall 5 px away reads 27–55.

**Two hypotheses tested and BOTH REFUTED.** Culling: `WGR_GPU_OCCLUSION=0` changed nothing (28.22%
vs 27.40% base). Ambient occlusion: `WGR_GTAO=0` gave 27.90% against a predicted <3%, and
`WGR_SKY_BAKE_VOLUMES=0` 28.04%. Note the two arms that *did* clear it (`WGR_PREPASS=0` → 0.07%,
`WGR_GPU_DRIVEN=0` → 0.21%) are **confounded** — each removes several AO factors as well.
Repro: `--test-world-freefly 6907.68 6017.04 313.98 187.2 -1.7 --test-world-hour 8`, hour 8,
score exact-black inside x 200-1400 / y 105-790.

## Orbs / "light posts look like white sticks" (Arma 3 AND Reforger)
A ~16 m mast rendering at pure white, bloom turning the bar into a disc. **The slot-0 untextured
fallback is REFUTED**: with `WGR_UNTEXTURED_ALBEDO=255,0,255` and even `...,all`, zero magenta
pixels appeared while all 34 pure-white pixels survived — and the log proved the lever fired
(`untextured-albedo correction is ON`). So these pixels are not sections resolving to bindless
slot 0. Something else makes them white.

Real and worth keeping: `EngineWgpu.cpp:2807-2817` logs "no bindable Stage0" and **never assigns
`d.texture_id`**, so those sections do keep 0 and go to the white texel — it just is not what the
owner is seeing. A per-draw UNTEXTURED warning now exists (the retained-path one could never name a
lamp: the emitter bail at `:4450` rejects `\lamps\`, `lamphalogen`, `lamp_searchlight`, `\lights\`,
`\runwaylights\`, `warninglight` before registration).

**On Reforger, white may be correct-by-construction:** 100% of Everon materials are `.emat`, none
opens, and `TranslateEmat` reads DayZ keys while only **2.2% of 10,312** real Reforger `.emat` files
name any key it reads (the real ones are `BCRMap`, `NMOMap`, `MaskMap`). Fixing the `.emat` reader
is the Reforger path and it is untouched.

## Reforger near trees stuck on a coarse LOD — UNTESTED, my pose was invalid
The registration hypothesis is **refuted** (618 models log `lod=0`, so fine LODs are registered).
The governor test was **invalid**: the pose `7156 2450 alt 3` landed the camera *inside a structure*,
where near geometry is naturally fine (`lod0=18158 lod1=22262 lod2=15983`, lod3+ = 0). **Re-run
outdoors, in trees.** The governor remains the leading suspect: `Scene::AdjustComplexity` drives
`_lodInvWidth` over a 16× range from **wall-clock** frame time, and on Everon the GPU is only 8.9%
of the frame, so it strips detail chasing time it cannot recover. Levers with no rebuild:
`WGR_LOD_GOVERNOR_TARGET=1`, `WGR_LOD_GOVERNOR_RANGE`. The proper fix is to feed the loop **GPU**
frame time (`Engine::GetWaterGpuTimings` already exposes it) — that lives in `SceneDraw.cpp`.

## Reforger roads do not render — NOT A BUG, it is a missing feature
Everon's roads are generated by the World Editor from `RoadGeneratorEntity` splines and **have no
mesh in the pak corpus**: of 51,452 GUIDs, 406 match "road" and only 6 are `.xob` (all `RoadBlock_01`
props/markers); `ev_DALL.wrp`'s 1,123-model table contains **zero** road models. Every road fix on
this branch is a no-op there. **Trap: `WGR_ROAD_TRACE` cannot help on Everon** — `ROAD_CPUPATH` is
gated on `OnSurface|IsOnSurface`, which no Everon shape carries. Zero `ROAD_*` lines there is
evidence of nothing.

## Reforger "buildings look wrong from the inside"
Not independently diagnosed. Fully accounted for by inverted normals + the 3–5 m sink. Re-check
after shipping items 1 and 2 before opening a new investigation.

## Reforger performance
Unmeasured since the streaming work landed. See item 6.

---

# TRAPS THAT COST REAL TIME — read before running anything

1. **`--test-world-freefly` takes `X Z Y` — ALTITUDE LAST — while every log and the in-game readout
   prints `(x, altitude, z)`.** Copying a pose from a log put a camera at 6099 m photographing
   clouds, with no error. The owner's in-game readout, however, is *already* in CLI order.
2. **`cargo test` from the REPO ROOT tests the main checkout, not the worktree.** It reported
   "33 passed" for a shader change whose tests had not been compiled, and the deployed renderer then
   failed to start. Always `cd .claude/worktrees/texstream && cargo test -p wgpu_renderer`.
3. **`patch` is a WGSL reserved keyword.** Naming a local that made `wgr_create` panic and the wgpu
   backend unavailable — no window, no error the owner would recognise.
4. **A diagnostic whose inactivity is indistinguishable from a clean result is worse than none.**
   The magenta fallback lever announced via `eprintln!`, and the launcher deliberately never
   redirects stderr (it kills the renderer, os error 232). One arm was run and had to be discarded.
   Both sides now announce through the game log. **Never accept a null from a lever that cannot
   prove it fired.**
5. **The last gate-trace line in every run is teardown, not steady state.** Screenshots land 8–32 ms
   before it. Reading it as a result produces a confident, wrong confirmation.
6. **`WGR_ALPHA_BLOCK_SCAN=0` is inert on CWA** — of 6,976 textures there are zero BC2 and exactly
   one BC3 (a startup logo). A null from that A/B means nothing.
7. **Ablation flags that silently do nothing.** `env_f32` filtered `*v > 0.0`, so
   `WGR_DECAL_SCALE=0` returned the default — the effect at *full* strength. Every prior "ablated
   it, no change" conclusion was void. Fixed, but check the pattern before trusting an ablation.
8. **Score the region, not the frame.** A whole-frame pixel diff called the conform arms
   "inconclusive"; masking the road ribbon showed `WGR_CONFORM_RASTER` moved **exactly 0** road
   pixels, which was decisive.
9. **Do not build or run two things at once.** A game process resident during a build corrupts any
   frame number. One machine, one job.

---

# HOW TO BUILD, DEPLOY, CAPTURE

`scripts\Build.ps1` does not work from an agent shell. Use:
```bash
export PATH="$PATH:/c/Program Files (x86)/Windows Kits/10/bin/10.0.26100.0/x64:/d/Program Files/LLVM/bin"
cmake --build build/win-x64-clang-rwdi --target PoseidonGame -- -j 8
```
Deploy **both** binaries or the game dies with `Entry Point Not Found`:
```bash
G="/d/SteamLibrary/steamapps/common/ARMA Cold War Assault"
cp build/win-x64-clang-rwdi/apps/cwr/Game/PoseidonGame.exe  "$G/ColdWarAssault.exe"
cp build/win-x64-clang-rwdi/apps/cwr/Game/wgpu_renderer.dll "$G/wgpu_renderer.dll"
```

**Capture harness (written this session, use it):** `.tmp-shadow/capture-arms.ps1` takes `-Out`,
`-World`, `-Addons`, `-Mods`, `-Freefly "X Z Y az el"`, `-Hour`, `-ShotFrame` or `-ShotSpec "150s"`,
and `-Arms ([ordered]@{ name = @{ ENV='value' } })`. It encodes every launch trap above. Everon and
Chernarus settle slowly — use the **time** form, never a frame number.

Worlds:
| world | `-World` | extra |
|---|---|---|
| Reforger Everon | `%TEMP%\rf\world\ev_DALL.wrp` | no addon root |
| Arma 3 Stratis | `%TEMP%\ff-worlds\stratis\stratis.wrp` | `-Mods a3stratis` |
| DayZ Chernarus | `%TEMP%\ff-worlds\chernarus\chernarusplus.wrp` | `-Addons "D:\...\DayZ\Addons"` |
| Arma 1 Sahrani | `%TEMP%\ff-worlds\sahrani\sara.wrp` | `-Addons "<repo>\packages\a1-compat\addons"` |
| Arma 2 Takistan | `%TEMP%\poseidon-a2oa-takistan-inspect\takistan.wrp` | `-Addons "<repo>\.tmp-perf-modernisation\a2-addons"` |

**The Arma 2 addon corpus is the only copy on the machine — read it, never delete or move it.**
All worlds live under `%TEMP%`, which Windows Storage Sense deletes. Check the file exists before
blaming a failed load.

**The owner will give you coordinates and will fly the game himself if you leave a session open.**
That produced the interior pose nothing else could construct. Use him — it is far cheaper than
searching for a pose.

---

# WHAT WAS ACTUALLY DELIVERED THIS SESSION

Fixed and verified: auto-exposure default-on with framerate-independent adaptation and dev controls;
the wall wobble (`mapType` defaulted to 0 and `MapTree` **is** 0, so every modern-format model loaded
as a tree and got the grass-wind shader on its walls — CWA was immune only because its older loader
reads the byte); the decal depth bias (the previous "fix" was numerically nulled to one micron); the
`env_f32` ablation bug; white roads proven to be overexposure rather than a texture bug.

Implemented but unmeasured: streaming cache/prefetch/ceiling, prepass-colour parity,
`WGR_CONFORM_RASTER`, cockpit blend depth-write.

Diagnosed but unshipped: everything in READY TO SHIP above.

**The gap between those three lists is the whole job.** Close it.
