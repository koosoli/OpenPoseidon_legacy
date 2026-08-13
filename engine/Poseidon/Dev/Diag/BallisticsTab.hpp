#pragma once

// The dev-panel "Ballistics" tab.  Declared in its own header so
// Dev/Debug/DebugOverlay.cpp only needs a five-line change to host it, and so
// three concurrent workstreams editing that file do not have to merge a
// several-hundred-line tab body.

namespace Poseidon::Dev
{
/// Draws the tab contents.  Call between ImGui::BeginTabItem / EndTabItem.
void DrawBallisticsTab();
} // namespace Poseidon::Dev
