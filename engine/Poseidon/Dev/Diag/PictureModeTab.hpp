#pragma once

// The dev-panel "Picture Mode" tab: controls that exist to compose a screenshot rather than to
// play. Declared in its own header for the reason BallisticsTab.hpp gives -- so hosting it in
// Dev/Debug/DebugOverlay.cpp is a five-line change and concurrent workstreams do not have to
// merge a several-hundred-line tab body.

namespace Poseidon::Dev
{
/// Draws the tab contents. Call between ImGui::BeginTabItem / EndTabItem.
void DrawPictureModeTab();

/// Restores depth of field to its compiled defaults.
void ResetPictureModeSettings();
void UpdatePictureModeFocus();
bool PictureModeClickFocusEnabled();
bool FocusPictureModeAtPixel(float x, float y);
} // namespace Poseidon::Dev
