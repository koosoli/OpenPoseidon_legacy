#pragma once

// PANEL SEARCH — a filter over the dev panel's controls, within the open tab.
//
// The design constraint that shapes everything here: a new setting must be
// covered by the filter WITHOUT anyone remembering to register it. A list of
// searchable settings would rot the first time someone added a slider and did
// not update it, and the failure is silent -- the filter would simply never
// find a control that exists.
//
// ImGui is immediate mode, so the label is already at the call site. Filtering
// AT the draw call therefore needs no registry and nothing to keep in sync: a
// control is searchable because it was drawn, and there is no other way to draw
// one. `panel_controls_go_through_the_filter` in the unit tests fails the build
// if a raw ImGui::SliderFloat/SliderInt/Checkbox/Combo/ColorEdit3 appears in
// DebugOverlay.cpp, so "the filter includes new features too" is enforced
// rather than merely documented.
//
// With no filter typed every one of these is a straight pass-through, so the
// panel behaves exactly as it did before.

struct ImGuiInputTextCallbackData;
struct ImVec2;
typedef int ImGuiSliderFlags;
typedef int ImGuiColorEditFlags;

namespace Poseidon::Dev
{
/// True when POSEIDON_PANEL_SEARCH seeded the filter, so the panel can open
/// itself for a scripted screenshot of a filtered tab.
bool PanelSearchPreseeded();

/// Restores the dev panel's settings to their compiled defaults.
///
/// Implemented as one EXPLICIT reset per tab, not by remembering pointers. The
/// first version did the latter -- capture a control's value the first time it is
/// drawn, write it back on demand -- and it was not merely broken, it was unsafe:
/// tabs commonly bind a slider to a member of a STACK-LOCAL copy of a settings
/// struct (`auto dof = GEngine->GetDepthOfFieldSettings(); Slider(&dof.focus)`).
/// Those addresses die with the frame, so the reset wrote into whatever had since
/// taken that stack space.
///
/// A tab is reset only if it says how. That is more code and it cannot silently
/// corrupt anything.
void ResetPanelSettings();

/// Restores fog appearance and zero authored fog amount; snow's automatic fog still applies.
void ResetFogSettings();

/// Draw the search box. Call ONCE per frame, above the tab bar.
///
/// This also resets the per-frame attribution state, which is why it must come
/// first. Resetting per frame is equivalent to resetting per tab: ImGui only
/// executes the selected tab's body, so exactly one tab runs per frame.
void DrawPanelSearch();

/// True while the user has something typed. Callers rarely need this; the
/// wrappers below already consult it.
bool PanelFilterActive();

/// Wrappers mirroring the ImGui signatures the panel uses. Each one draws
/// normally when it matches and is skipped (returning false) when it does not.
bool SliderFloat(const char* label, float* v, float min, float max, const char* format = "%.3f",
                 ImGuiSliderFlags flags = 0);
bool SliderInt(const char* label, int* v, int min, int max, const char* format = "%d", ImGuiSliderFlags flags = 0);
bool Checkbox(const char* label, bool* v);
bool Combo(const char* label, int* currentItem, const char* const items[], int itemsCount,
           int popupMaxHeightInItems = -1);
bool Combo(const char* label, int* currentItem, const char* itemsSeparatedByZeros, int popupMaxHeightInItems = -1);
bool ColorEdit3(const char* label, float col[3], ImGuiColorEditFlags flags = 0);

/// Action buttons. Filtered like anything else: while you are searching for
/// "heading", "Spawn" and "Copy current pose" are not what you asked for, and a
/// filtered tab that still lists every button is barely filtered at all. Clear
/// the search to use them.
bool Button(const char* label);
bool Button(const char* label, const ImVec2& size);
bool SmallButton(const char* label);
bool RadioButton(const char* label, bool active);

bool InputText(const char* label, char* buf, size_t bufSize, int flags = 0);
bool InputTextWithHint(const char* label, const char* hint, char* buf, size_t bufSize, int flags = 0);
bool InputInt(const char* label, int* v, int step = 1, int stepFast = 100, int flags = 0);

/// BeginTabItem, plus the one-shot selection POSEIDON_PANEL_TAB asks for.
///
/// Same reason as POSEIDON_PANEL_SEARCH: an --auto-screenshot run cannot click
/// a tab, so without this the search could only ever be photographed on
/// whichever tab happens to be first.
bool PanelTabItem(const char* label, bool* open = nullptr, int flags = 0);

/// Pairs with ImGui::EndCombo exactly as ImGui::BeginCombo does: a filtered-out
/// combo returns false, so the body -- and the EndCombo inside it -- is skipped.
bool BeginCombo(const char* label, const char* previewValue, int flags = 0);

/// Tooltip on the control above. Skipped when that control was filtered away
/// (otherwise it lands on whatever item happened to be drawn last), and its
/// text joins the searchable material for that control.
void PanelTooltip(const char* fmt, ...);

/// IsItemHovered() for the control above, false when it was filtered away.
bool PanelItemHovered(int flags = 0);

/// SameLine that stands down when the control before it was filtered away.
///
/// Without this, hiding the left half of a row silently promotes whatever comes
/// next onto that row -- a button ends up beside an unrelated slider, half off
/// the window edge. Rows are the one piece of layout filtering can genuinely
/// corrupt, so it is handled rather than tolerated.
void PanelSameLine();

/// A section heading. Hidden while filtering, so results are one flat list
/// rather than the empty skeleton of the sections they came from.
void PanelHeading(const char* text);

/// The explanation paragraph under a control (what ImGui::TextDisabled was).
///
/// Two jobs beyond drawing. It is HIDDEN when the control above it was filtered
/// out -- otherwise a filtered panel is a wall of orphaned paragraphs -- and its
/// text is REMEMBERED against that control's ImGui ID, so the next frame's
/// search matches the explanation as well as the label. That one frame of lag
/// is invisible: the filter is typed a character at a time.
///
/// The explanations are where the panel's knowledge lives ("above ~8x the old
/// flicker returns"), so searching only labels would miss most of what a person
/// is actually looking for.
void PanelHelp(const char* fmt, ...);

/// A separator that disappears while filtering, so results read as one list
/// rather than as the empty skeleton of the sections they came from.
void PanelSeparator();
} // namespace Poseidon::Dev
