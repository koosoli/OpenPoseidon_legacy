# RND-034 — The capture "sub-rect" was the cinema border, not a window-sizing bug

**Date:** 2026-08-15
**Branch:** `codex/wgpu-performance-modernisation`
**Status:** PARTIAL — cause identified and fixed, verified across four resolutions; affected prior measurements not yet re-taken.

## What was claimed, and why it was wrong

Two independent observers reported that captures rendered into only part of the requested
framebuffer — 1442x828 inside a 1920x1080 PNG, "with stale content in the border" — and it was
written up as a DPI / window-sizing regression introduced between `aa24c32` and `4b33b5e`, and
escalated as a release blocker. **All of that was wrong.** Each premise was disproved directly:

- *"The engine believes it has a surface size it does not have."* It does not. `EngineWgpu.cpp:665-672`
  sets `_w/_h` from `SDL_GetWindowSizeInPixels` and passes exactly those to `wgr_create`. At a
  1920x1080 request the window really is 1920x1080 px.
- *"The screenshot path writes a PNG at the requested size."* It does not. `lib.rs:2457-2462` sizes the
  readback from the live surface config with correct row padding, and `EngineWgpu.cpp:5236` calls
  `wgr_screenshot_take(nullptr, 0, &w, &h)` first to learn the real size. Proof: `--width 2560
  --height 1440` produces a **1920x1080** PNG, not a 2560x1440 one.
- *"Build 9d846ce writes the full frame at the same request."* Both the "broken" 1920x1080 captures and
  the "clean" 800x600 one are stamped `9d846cecd85c-release`. That string is a git commit and the
  worktree is dirty, so **it does not identify a binary** — the DLLs in `rf/oldbin1` and `dist` differ
  in size while reporting the same id. Any A/B keyed on that stamp is unsound.
- **Control:** with `--window --width/--height` and no free-fly, both the frozen `9d846ce` pair and the
  current `dist` pair fill **100%** of the framebuffer at 800x600, 1280x720, 1920x1080 and 2560x1440.

The discriminator was never the commit. It was **`--test-world-freefly`**.

## The actual cause

The "sub-rect" is the **cutscene cinema border**, painted as 2D quads over an already-finished frame:

- `apps/cwr/Game/GameApplication.cpp:1331` — `--test-world-freefly` calls `GWorld->SetCameraEffect(...)`.
- `engine/Poseidon/World/WorldImpl.cpp:2233` — `CameraEffect::Draw()` draws the letterbox whenever
  `showCinemaBorder` is set.
- `engine/Poseidon/World/WorldSetup.cpp:1461` — `bool showCinemaBorder = true;` — a global, defaulting on.
- `engine/Poseidon/World/Scene/Object.cpp:1421` — `DrawWidescreenPillarbox()`, width
  `AspectRatio::LateralPillarboxWidth = (w - 4h/3)/2` (`UI/Settings/AspectRatio.cpp:277`).
- Top/bottom bars come from the 4:3-authored CinemaBorder **model**, whose quads scale by `(4/3)*h`
  (`Object.cpp:1407`) — a constant fraction of height.

The arithmetic matches the measured pixels exactly: `(800-800)/2 = 0`, `(1280-960)/2 = 160`,
`(1920-1440)/2 = 240`, against measured 0, 160, 240.

The bars are lit by `sunlight x eye-accommodation x texture` (`Object.cpp:1283-1378`), so they **dim
rather than replace**. That is why the border read as "stale content with scene structure in it", and
why the top bar is warm grey while the bottom is near-black — the detail that made it look like a
framebuffer defect.

`DebugOverlay.cpp:151-157` already documents this exact trap for the Zeus camera and clears the flag
there. `AdoptFreeFlyCameraImpl` (`:268`) only *saves* the flag and never clears it, and is skipped
under `--no-dev` and absent from release builds — so the command-line free-fly camera kept the
letterbox.

## Fix

1. `apps/cwr/Game/GameApplication.cpp` — suppress the cinema border for the `--test-world-freefly`
   developer camera, following the Zeus precedent, ordered after `AdoptFreeFlyCamera` so the dev
   panel's restore snapshot keeps the mission's value. Logs
   `--test-world-freefly: cinema border suppressed`.
2. `engine/Poseidon/Graphics/Shared/SdlWindow.cpp` — the one *real* sizing defect found:
   `ResolveWindowPlacement` clamped a windowed request to the desktop **silently**. It now warns with
   requested vs granted vs desktop, plus a second warning if the SDL pixel readback differs from the
   intended size (borderless-aware, so its deliberate +1 column does not warn on every boot).

**Clamp-and-report was chosen over render-off-screen.** Honouring an over-desktop request needs a new
offscreen path in renderer files other agents are editing, and would have fixed nothing here:
1920x1080 *was* granted exactly, and the lost 42% was overlay quads, not missing pixels. Offscreen
capture above desktop size is a scoped follow-up, not a bug fix.

## Verification — Everon + free-fly, the configuration that showed it

| requested | PNG | drawn world before | before | drawn world after | after |
|---|---|---|---|---|---|
| 800 x 600 | 800x600 | 800 x 459 at (0,71) | 76% | **800 x 600 at (0,0)** | **100%** |
| 1280 x 720 | 1280x720 | 960 x 551 at (160,85) | 57% | **1280 x 720 at (0,0)** | **100%** |
| 1920 x 1080 | 1920x1080 | 1440 x 826 at (240,127) | 57% | **1920 x 1080 at (0,0)** | **100%** |
| 2560 x 1440 | 1920x1080 | clamps to the 1920 row | 57% | **1920 x 1080 at (0,0)** | **100%** |

Before and after ran the **identical `wgpu_renderer.dll`** (md5 verified); the exe was the only
variable. The 2560 run now logs `CreateGameWindow: requested windowed 2560x1440 does not fit the
1920x1080 desktop; clamped to 1920x1080`.

## Consequence for every earlier measurement in this session

**Every per-pixel measurement taken through `--test-world-freefly` at 16:9 was against 57% of the
frame; at 4:3, 76%.** That includes the LOD and performance captures under
`C:\Users\mail\AppData\Local\Temp\rf\cap\`. What this does and does not invalidate:

- **GPU timings are unaffected.** The 3D scene rendered at full surface size; the bars are overlay
  quads composited afterwards. `GPU frame total` and the region breakdowns stand.
- **Pixel-ratio statistics need re-taking** if their crop overlapped a bar. Crops taken well inside the
  picture remain valid, but the safe move is to re-measure.
- A specific correction: the reading published as "800x462 at (0,69) — the normal 4:3 letterbox" was
  **not** an aspect letterbox. It was 71 px of cinema bar, i.e. the same bug.

## Method note

The row/column mean-gradient detector used to find the "sub-rect" **mislabels flat sky as
not-rendered and a lake shore as a 240 px pillarbox**. The corrected detector adds one test — a bar's
outer band is much *flatter* than the picture inside it (`scratchpad/bars.py`). Any future framing
measurement should use that, not raw gradient extent.
