# Reforger parity on native Everon -- 2026-10-15 (branch MrDudeX-reforger)

Owner's brief: compare Open Poseidon's native Arma Reforger Everon with the original and bring it as
close as possible -- grass (looked flat), trees (near and far), doors that open, window panes that
look like Reforger's and break, rivers that look like Reforger's. Keep our sky and our sea, and
everything we already do better. Clean room: only Reforger's own data (text prefabs, .emat, .conf
read from the .pak archives, as the engine already does) and its rendered output were used; no
Bohemia executable was disassembled or decompiled.

Base: Oli's `perf/optimization-20261014` (all its commits), CI build per push
(`.github/workflows/reforger-windows.yml`, rolling pre-release `reforger-ci`).

## How the comparison is made

`scripts/reforger-pc/` on the owner's PC, folder `<CWA>\_OpenPoseidon-Reforger` (never the shared
`Remastered` binaries; that folder is only the working directory):

- `poses.txt` -- named Everon poses (x z altitude azimuth elevation).
- `MODE=poses` -- the build in `bin\` flies through the poses on native Everon and saves one
  screenshot per pose (`triFreeFlyPose`, `triScreenshotNext`). The first pose waits 3 s: the
  `--test-world-freefly` camera is installed after `init.sqs` starts and overwrote it.
- `MODE=workbench` -- the same poses in the original: Arma Reforger Tools' World Editor with the
  `MDXParity` plugin (`workbench/MDXParity`, public script API only: `WorldEditorAPI.SetCamera`,
  `System.MakeScreenshot`, `TimeAndWeatherManagerEntity.SetTimeOfTheDay`).
- `watch.cmd` (once), then runs are started by writing `request.txt` / `queue\*.txt`.
- `compare.py` builds side-by-side sheets and luma statistics; `poses_probe.txt` opens the doors
  and breaks the panes of one house and photographs it before and after.

The Workbench viewport has a wider field of view and another aspect than our 16:9 captures, so the
sheets are compared by eye, not per pixel.

## What changed

| Item | What it was | What it is now | Switch |
|---|---|---|---|
| RFG-157 doors | placed closed, no action | leaves swing about their hinge over 1 s; "Open door"/"Close door" in the action menu; collision and GPU instance follow; an evicted open door comes back open. 8,654 doors on Everon | `POSEIDON_ENFUSION_DOORS=0` |
| RFG-158 panes | could not break | a round's path or an explosion breaks a pane: its `_dst` phase model (shards in the frame) replaces it and stays broken; sound; the round flies on. 29,052 panes | `POSEIDON_ENFUSION_GLASS_BREAK=0` |
| RFG-164/168 glass look | panes read as black holes | MatPBRBasicGlass panes mirror with window_glass.emat's curve (FresnelBias 0.05, FresnelPower 1.333), no ripple, reflection near the horizon dimmed to 25 % (it would meet houses and trees, not sky) | `WGR_ENFUSION_GLASS_SHEEN=0` |
| RFG-159/160 grass | one normal per card, no occlusion, no shadow, ring to 41 m shrinking past 20 m | NTO per-texel normals, root occlusion, cascade shadows, backlight; ring x2 (82 m) thinned with distance | `WGR_NATIVE_TUFT_SHADE=0`, `WGR_NATIVE_RING_*` |
| RFG-167/168 lawns | knee-high in yards | Grass_01/02 PlantMats (GrassLawn surfaces) at 0.35 m; Grass_03 meadows and crops unchanged | `WGR_NATIVE_LAWN_HEIGHT` (0 = off) |
| RFG-161 far forest | far proxies lit by sky alone | take the real sun | `WGR_NATIVE_FAR_SUN=0` |
| RFG-162/163 rivers, ponds | opaque olive ribbons / nothing | drawn as water (refraction, absorption, Fresnel sky + screen-space reflection, flow ripples) | `WGR_RIVERS=0` |
| RFG-165 lakes | Lake_Center (0.9 x 0.45 km) 7 m too high: dry bed at its shore | the generator's own offset (`coords 0 -7 0`) applied | -- |
| RFG-166/168/169 water look | a pale sky sheen over pale gravel; streaks over Lake_Center | MatRiver Color covers a shallow bed (`WGR_RIVER_BODY`) at 0.5 x `WGR_RIVER_SCATTER` of the sky+sun light (dark olive, was pale sand), grazing reflections meet the banks (`WGR_RIVER_BANK`), wet bed >= 0.38 darker, >= 0.3 m look column inside a river ribbon, round ripples on still water, the bed lookup at most 0.25 m deep (the streaks) | `WGR_RIVER_*` |

Trees: Oli's FAR-002 (forests to the horizon) is in the base. A/B of the foliage lighting knobs
(`WGR_FOLIAGE_AMBIENT_BOOST=0.75`, `WGR_FOLIAGE_WRAP=0`, `WGR_FOLIAGE_TRANS_SCALE=0.5`) at the forest
poses changed the median luma by under 1 %: native crowns no longer depend on them, and the
remaining look difference to Reforger at these poses is framing (field of view), not lighting.

## Verified on the owner's PC (DESKTOP-QLF5JPR)

- `b5e490e` probe: doors 8654 / parts 11352, panes 29052; around the test house 6 doors opened and
  18 panes broke (`triEnfusionDoorsAll`, `triEnfusionGlassBreakAll`); the after-shot shows open
  doors and shard-edged frames. Same counts with `aa88042`.
- `aa88042`: Lake_Center is water at meadowNear/meadowWide (was a dry bed); yards read as lawn;
  windows reflect (too brightly -- fixed in RFG-168, `181a1f8`).
- Not covered yet: the multiplayer synchronisation of doors and panes (local per client; panes
  break on every client from the same shot), Reforger's door sounds (its audio is not decoded).

A/B on the house poses with `WGR_GI_RSM_GAIN=3` and `WGR_SKY_AMBIENT=2.2` left the shaded
facades almost unchanged: their darkness is not those two knobs.

Harness fixes found on the way: each pose is set again 2 s before its shot (a late freefly camera or
stray input moved it), and a start that dies within 30 s (stderr access denied at renderer
creation) is retried.

## Still different from Reforger (seen in the sheets, not addressed)

- Facades in shade are darker and bluer than in Reforger (global sky/indirect lighting).
- Wheat fields read denser and taller than Reforger's at fieldEdge.
- Meadow grass colour leans to straw where Reforger's is greener at the same pose.
