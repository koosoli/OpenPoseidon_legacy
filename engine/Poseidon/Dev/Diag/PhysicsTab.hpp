#pragma once

// The dev-panel "Physics" tab (PHY-020). Own header for the same reason as the
// other tabs: hosting it in DebugOverlay.cpp stays a five-line change.

namespace Poseidon::Dev
{
/// Draws the tab contents. Call between ImGui::BeginTabItem / EndTabItem.
void DrawPhysicsTab();
} // namespace Poseidon::Dev
