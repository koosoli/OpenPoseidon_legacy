# HANDOVER — photo-card grass density is still not visibly changing

Written 2026-08-18. One open problem, everything else around it is proven.

---

## 0. BRANCH AND WORKTREE — READ THIS FIRST

**Branch: `feat/wgpu-decal-billboards`. Remote: `safety`. Tip when written: `f7101250`.**

```bash
git fetch safety
git worktree add .claude/worktrees/<yourname> -b <yourbranch> safety/feat/wgpu-decal-billboards
```

* **Do NOT work in the repo-root checkout.** It is another session's working tree.
  `.agents/README.md` Rule 0 is mandatory reading — one game folder, many sessions.
* **Do NOT branch from `new-renderer-infrastructure`, `codex/wgpu-performance-modernisation`
  or `feat/texture-streaming`.** All the grass work described here is ONLY on
  `feat/wgpu-decal-billboards`, which already contains `feat/texture-streaming` as an
  ancestor. Branching from anywhere else silently loses it and you will "reproduce" a
  bug that was fixed.
* Before deploying: read `D:\SteamLibrary\...\ARMA Cold War Assault\DEPLOYED-FROM.txt`.
  If it names a branch that is not yours, STOP and report. Write it after deploying.
  Copy **both** `ColdWarAssault.exe` (and `PoseidonGame.exe`) **and** `wgpu_renderer.dll`.
* Build from the worktree (Build.ps1 dies under the agent shell; use bash):
  ```bash
  export PATH="$PATH:/c/Program Files (x86)/Windows Kits/10/bin/10.0.26100.0/x64:/d/Program Files/LLVM/bin"
  cmake --preset win-x64-clang-rwdi
  cmake --build build/win-x64-clang-rwdi --target PoseidonGame
  ```

---

## 1. The problem

The owner reports photo-clump ("card") grass is far sparser than procedural grass, and
that **no setting changes it** — including the density control added for exactly this.

Their screenshot: scattered individual tufts with metres of bare ground between them.
What they want: a continuous meadow, which they say the same photo grass can produce.

---

## 2. What is PROVEN (do not re-investigate)

Density = `coverage / grid²`. Defaults are coverage 0.24 on a 1.11 m grid = **0.195 /m²**;
the procedural near ring is `spacing 0.10 × 3.20 = 0.32 m` = **9.8 /m²**. A real 50x gap.

The whole renderer chain works. Measured, same pose, card path forced on:

| density | log line | near ring |
|---|---|---|
| 1x | `grass card density: 1.0x -> grid 1.110 m` | cards at 1.11 m, **80x80** grid |
| 25x | `grass card density: 25.0x -> grid 0.222 m` | cards at 0.22 m, **376x376** grid |

and the same-pose screenshots differ enormously (1.11 m = bare ground with a few tufts;
0.22 m = grass filling the frame). So: the conversion, the FFI
(`wgr_grass_set_card_spacing`), the per-frame grid dims and the shader all work.

Settings round-trip is also fine: `SetGrassSettings` does `_grass = settings` and
`GetGrassSettings` returns `_grass`; `SubmitGrass()` re-pushes every frame (it does not
stomp the panel, it re-applies the stored value). The slider (DebugOverlay.cpp ~3400) and
the push (~3735) are in the same `DrawGrassTab()` scope on the same `grass` variable.

---

## 3. What is NOT proven — the actual open question

**Nobody has verified the owner's hand on the slider produces the log line.** Every test
above drove the value via env var, never through ImGui.

`f7101250` added the diagnostic for exactly this. Ask the owner (or drive it yourself) to
move "Photo card density" and then:

```bash
grep "grass card density" cwr.log
```

Three outcomes, three different bugs:

1. **No line** → the panel never reaches the engine. Look at `if (changed)` around
   DebugOverlay.cpp:3735 and whether the Grass tab is even the one being drawn.
2. **Line says `setting 1.0x`** → the slider is not writing into the struct that gets
   pushed (check for a shadowing local, or a preset/reset overwriting it that frame).
3. **Line says `setting 25.0x -> grid 0.222 m`** → the renderer HAS it and is placing
   376x376 candidates. Then the density genuinely changed and what the owner is looking
   at is not near/mid-ring cards. Most likely the **far proxy** — the flat green beyond
   the rings, which cards never touch and no card control affects. Check
   `WGR_GRASS_DISTANCE` (near radius, default 50 m, max 60) and the mid radius: at high
   density the mid grid hits `MID_GRID_MAX` 2440 and the mid ring SHRINKS in radius
   while getting denser, so the dense zone can be much smaller than the visible field.

Bypass the panel to confirm the effect exists on their machine at their location:

```bash
POSEIDON_GRASS_CARD_DENSITY=25 ./ColdWarAssault.exe --render=wgpu --window --dev \
  --log-file cwr.log --log-level info
```

---

## 4. Traps that cost this session hours — do not repeat them

* **The FIRST `Wgpu grass cards:` log line always says `drawn=0 ... blades`.** It is
  emitted before the atlas finishes uploading. `announce_cards` logs on every state
  change, so **read the LAST line**. Reading the first one led me to tell the owner the
  card path was off when it was fine.
* **Two clamps had to agree.** `CARD_SPACING_RANGE` in `grass/mod.rs` AND a separate
  hardcoded `clamp(grass.lod_density.w, 0.16, 4.0)` in `grass.wgsl`. Lowering only the
  CPU one was silently floored. Both are 0.06 now. **If a grass value "does nothing",
  grep the WGSL for its own clamp before anything else.**
* **The control used to be spacing in metres, where more is less.** Dragging it to max
  made the grass sparser. It is now a density multiplier (1x..300x). If you add another
  knob here, make it go up for more.
* **Cards need TWO gates**: `use_photo_tuft` (`clutterSource == 2 || (== 0 && map atlas)`)
  AND the tuft atlas resident. The panel now states this in green/orange above the
  coverage slider. `WGR_GRASS_CLUTTER_SOURCE=2` forces it.

---

## 5. Levers

| lever | what |
|---|---|
| Panel → Grass → **Photo card density** | 1x..300x, log scale. 1x = the map's clutterGrid. |
| Panel → Grass → **Photo card coverage** | fraction of grid cells that grow a card. |
| `POSEIDON_GRASS_CARD_DENSITY=<x>` | forces the multiplier through the panel's code path |
| `WGR_GRASS_CARD_SPACING=<m>` | forces the raw grid in metres; wins over everything |
| `WGR_GRASS_CLUTTER_SOURCE=2` | forces the card path on |
| `WGR_GRASS_DISTANCE=<m>` | near ring radius, 8..60 |

The panel prints both densities in /m² live and, while cards are sparser, the exact
multiplier that would match the procedural ring.
