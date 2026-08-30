#pragma once

// The dev-panel "Streaming" tab.  Declared in its own header so
// Dev/Debug/DebugOverlay.cpp only needs a five-line change to host it (same
// arrangement as BallisticsTab / WeatherTab).

namespace Poseidon::Dev
{
/// Draws the tab contents.  Call between ImGui::BeginTabItem / EndTabItem.
void DrawStreamingTab();
} // namespace Poseidon::Dev
