#pragma once

// The dev-panel "Conquest" tab: a read-only view of the referee (Game/Conquest), the war's
// objectives, sides, groups and log. Its own translation unit, like WeatherTab.

namespace Poseidon::Dev
{
/// Draws the tab contents.  Call between ImGui::BeginTabItem / EndTabItem.
void DrawConquestTab();
} // namespace Poseidon::Dev
