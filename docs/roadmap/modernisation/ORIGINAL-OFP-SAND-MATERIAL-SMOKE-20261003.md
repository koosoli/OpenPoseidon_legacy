# Original sand material smoke instructions, 2026-10-03

Extend the existing `Measure-DefaultMaterial.ps1` / `farfield-bench.ps1` path,
rather than adding another game launcher. Existing legacy Eden road, sand and
gravel commands retain their original default mission and camera.

## Scope and source sites

Use `-Surface Sand -SandSource AbelPi|NoePt|NoePs -SandView Near|Mid|Far`.
The three views aim at the same source-cell centre with azimuth 0 and elevation
-60 degrees. Their camera heights above the exported centre are 1.8, 15 and
50 m, respectively. This gives approximately 2.08, 17.32 and 57.74 m distance
to that centre, spanning the existing detail normal's 15–35 m fade.
The camera north-coordinate moves by height/tan(60 degrees) to keep the centre
in its aim. Original ground shape can change the exact centre-ray intersection;
the exported height is not a claim of an exact rendered ground pixel.

| SandSource | Stock mission | Actual source X / Z | Exported centre Y | Palette |
|---|---|---|---:|---:|
| AbelPi | perf_abel.abel | 7575 / 10675 | 27.765 | 11 |
| NoePt | perf_sand.noe | 2575 / 5125 | 14.985 | 7 |
| NoePs | perf_sand.noe | 2975 / 4125 | 39.015 | 4 |

AbelPi and NoePt use the checked-in actual stock source census. NoePs was
derived by the same bounded source rules from a fresh CPU export of actual
installed Noe: exact pure `o/ps.paa`, original 256x256/50 m layout, all four
corners above 3 m, height range below 2 m, and no source placement centre
within 12 m in the surrounding 3x3 cells. Only two NoePs cells meet those
rules; the listed candidate is the closest to the census reference 2675/5125.
Its corner heights are 39.465, 38.970, 39.060 and 38.790 m. The nearest source
placement centre is rock id15946 at 2976.0244 / 4094.1406, 30.8764 m away.
That owner-centre check is not collision, bounds, or unobstructed-camera proof.

Fresh Noe WRP SHA256:
`9b27b11661d3cb30f9f0749a9a4616bfc58d723c4750666d1e674dd4213598d2`.
Fresh exporter corpus SHA256:
`dbe1f29e300bb60c0b34c5f6467a842857147bed82495ef84f390a0871016a18`.
Both exactly match the checked-in census. The derivation receipt is in ignored
`build/soft-source-audit/noe-ps-candidate.json` in the candidate worktree.

## Run with the ordinary installed-game lock

For example, from the root worktree via the existing Bash lock wrapper:

```sh
bash scripts/with-game-lock.sh powershell.exe -NoProfile -ExecutionPolicy Bypass -File scripts/Measure-DefaultMaterial.ps1 -Surface Sand -SandSource AbelPi -SandView Near -Arm Full -DetailNormals 1
```

Run Full/detail=1 at Near, Mid and Far for each of the three sources. At the
same Near pose, also run Full/detail=0, Albedo/detail=1, and Original/detail=1.
Albedo means macro/detail normals OFF while the visual pack is mounted.
Original disables the pack. These three added entries preserve stock albedo,
so the original, Albedo and Full arms all use the actual original pi/pt/ps
colour images. The existing EdenPs entry retains its prior generated albedo.
Optional Mid/Far OFF captures help assess macro relief and fade stability.

Use the same installed pair/content, Hour, Upscaler and output dimensions for
each compared pose. These are appearance captures; do not infer performance
from a few readback frames. The script does not deploy, package content,
rewrite original archives, force texture flags, stamp footprints or alter
surface classification.

## What the runner proves

The small owned query job connects to the run's ephemeral local harness port,
waits for the selected original world/source and `triSceneReady`, and performs
read-only `dev_sand sample` at the view target. It validates the exact original
texture, actual authored class/files/sound/character, all-four-quadrant source
eligibility, and the target coordinates. Query time is recorded. This is a
pre-capture source observation in the same game process, not a per-pixel trace.

The log guards require the original-albedo mapping and source-specific macro
and detail normal loading where enabled, and forbid those bindings in their
OFF arms. The existing terrain traces occur only after nonzero normal layer
assignment in the CPU material table. An exact 2 repeats/metre detail receipt
is required. They establish loaded material-table descriptors, not selected
draw material, submitted GPU binding, or visible grain pixels. No current
helper reports bidirectional GPU descriptor/cache resolution for that exact
captured material; no such receipt is invented here.

The actual logged freefly camera initialization must match the plan within
0.02 m/degrees. The farfield driver supplies capture metrics, PNG/log artifacts,
watchdog protection, provenance stamp and normal exit checks. The wrapper also
checks normal-resolution or active-DLSS state and records unchanged SHA256 of
the installed stamp, EXE, DLL, visual PBO, base config and selected original
texture archive. `sand-source-bindings.json` records source/binding observations
before assertions, and records validated status only after all checks pass.

Root must inspect the near/mid/far captures for grain/ripple response, retained
map colours, tile seams, mip shimmer and distant glitter. Trees/rocks may
obstruct a view despite its native source qualification; adjust a pose only
after recording and verifying its actual source, then repeat every arm with
the same revised pose. Mixed ps/pt PAC palettes and Cain's missing authored
soft metadata remain outside this slice.

## Mod precedence remains an explicit separate arm

Current loader guards require the original selected archive to match the
manifest. The focused source-policy tests cover those guards. No actual
overriding mod was manufactured or tested by this candidate. A future runtime
arm must use a genuine existing override for the exact virtual pi/pt/ps source,
verify that its archive wins, require no default material mapping/normal
enhancement for that source, and inspect its actual texture. The present
runner expects the original archives and will reject that override; it does
not report mod precedence success from an ordinary stock run.

## Source-only validation

`Measure-DefaultMaterial.ps1 -SelfTest` passes actual reused source/binding
guard falsifiers for wrong map, source, class, files, sound, character,
coordinates, source refusal, missing normals, wrong detail scale and normals
present in an Original arm. `-PlanOnly` produces all twelve source/view plans
without a game lock, profile creation, jobs or game launch. PowerShell parser
and `git diff --check` pass. No device, full build, packaging or game was run
by the candidate agent. Actual pipeline and visual acceptance remain pending.
