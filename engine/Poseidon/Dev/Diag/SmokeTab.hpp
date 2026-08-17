#pragma once

// The dev-panel "Smoke" tab.  Body lives in its own translation unit for the
// same reason BallisticsTab and WeatherTab do.

namespace Poseidon::Dev
{
/// Draws the tab contents.  Call between ImGui::BeginTabItem / EndTabItem.
void DrawSmokeTab();
} // namespace Poseidon::Dev
