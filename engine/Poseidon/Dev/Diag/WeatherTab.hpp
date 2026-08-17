#pragma once

// The dev-panel "Weather" tab.  Declared in its own header for the same reason
// BallisticsTab is: DebugOverlay.cpp is 6000 lines and several workstreams edit
// it, so a tab body lives in its own translation unit and costs that file five
// lines.

namespace Poseidon::Dev
{
/// Draws the tab contents.  Call between ImGui::BeginTabItem / EndTabItem.
void DrawWeatherTab();
} // namespace Poseidon::Dev
