#pragma once

// AIR-010: the dev-panel "Flight" tab -- the aircraft altitude falloff.
// Own header for the same reason as the other tabs: hosting it in
// DebugOverlay.cpp stays a five-line change.

namespace Poseidon::Dev
{
/// Draws the tab contents. Call between ImGui::BeginTabItem / EndTabItem.
void DrawFlightTab();
} // namespace Poseidon::Dev
