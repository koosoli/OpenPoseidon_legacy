# TEST-005 — The dev panel search filters at the draw call, and a test keeps it that way

**Date:** 2026-08-18
**Branch:** `feat/wgpu-decal-billboards`
**Status:** Step one shipped: filter **within the open tab**. Cross-tab results are deliberately not done.
**Related:** [GRS-002](GRS-002-photo-card-density-works-the-mid-ring-reach-does-not-20260818.md)

## The problem

The panel had grown to **25 tabs and ~450 controls** across 6,560 lines. Finding a setting meant
knowing which tab it was on.

## Why not a registry

The owner's requirement was that new settings be covered **without anyone remembering to add them**.
That rules out any list of searchable settings: it rots the first time someone adds a slider and
forgets, and it rots *silently* — the filter simply never finds a control that exists, and nobody
learns why for months.

ImGui is immediate mode, so the label is already at the call site. **Filtering at the draw call
needs no registry and nothing to keep in sync**: a control is searchable because it was drawn, and
there is no other way to draw one.

## What was built

`DevPanelWidgets.hpp/.cpp` — wrappers mirroring the ImGui signatures the panel uses, each drawing
normally on a match and skipping on a miss. With no filter typed every one is a pass-through, so
the panel behaves exactly as before. Converted, mechanically:

| | count | | count |
|---|---:|---|---:|
| `SliderFloat` | 225 | `Button` | 76 |
| `Checkbox` | 82 | `TextUnformatted` → `PanelHeading` | 79 |
| `TextDisabled` → `PanelHelp` | 278 | `SetItemTooltip` → `PanelTooltip` | 129 |
| `SliderInt` | 23 | `SameLine` → `PanelSameLine` | 60 |
| `Combo` / `BeginCombo` | 20 | `BeginTabItem` → `PanelTabItem` | 25 |

Three things that make it useful rather than merely present:

* **The explanations are searchable, not just the labels.** `PanelHelp` and `PanelTooltip` record
  their text against the control's ImGui ID, so the next frame's search matches it. The one frame of
  lag is invisible — a filter is typed a character at a time. **Proof: searching `flicker`, a word in
  no label at all, returns 5 of 86 controls on the Grass tab**, including "Photo card density" whose
  help says "above ~8x the old flicker returns". That is most of the panel's knowledge, and a
  label-only search would have missed all of it.
* **Rows survive.** Hiding the left half of a `SameLine` row silently promotes the next control onto
  it — a button ended up beside an unrelated slider, half off the window edge. `PanelSameLine`
  stands down when the control before it was filtered away.
* **Headings and separators disappear while filtering**, so results read as one list rather than as
  the empty skeleton of the sections they came from.

## What keeps it true

`tests/unit/.../test_dev_panel_search.cpp` fails the build if a raw `ImGui::SliderFloat(`,
`Checkbox(`, `Combo(`, `Button(`, `TextUnformatted(`, `SameLine(`, `InputText(`, `BeginCombo(`,
`SetItemTooltip(`, `IsItemHovered(` or `BeginTabItem(` appears in `DebugOverlay.cpp`, and names the
line. **The guard was verified by breaking it**: reinstating one raw `ImGui::Checkbox` failed the
test with `DebugOverlay.cpp:3384`. A guard that has never been seen to fail is not a guard.

`ImGui::Text` and `TextColored` are deliberately NOT covered: those are live readouts and status
lines, and a frame-time number should not vanish because you searched for something else. Six
`TextDisabled` remain raw on purpose — they are table CELLS, whose content would blank out.

## Scriptable, so it can be checked without hands

`--auto-screenshot` cannot click a tab or type in a box, so the feature would otherwise only ever be
verifiable by a person sitting at the keyboard:

* `POSEIDON_PANEL_OPEN=1` — open the panel.
* `POSEIDON_PANEL_SEARCH=<terms>` — open it with that filter already typed.
* `POSEIDON_PANEL_TAB=<name>` — select that tab, once, on the first frame.

## What this does NOT do

- **Results do not cross tabs.** Searching `flicker` from the Zeus tab says "no match in this tab
  (35 controls)" even though the Grass tab has five. That is step two, and it is the fiddly half:
  the 25 tab bodies assume they are inside their own scope.
- Tooltips can still land oddly in the few places that hover-test something other than the control
  above them.
