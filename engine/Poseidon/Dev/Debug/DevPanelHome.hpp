#pragma once

// The dev panel's first tab: an index of every other tab, grouped by what it is for, with
// one-click jumps, plus the panel's keyboard shortcuts.

namespace Poseidon::Dev
{
/// Draws the tab contents.  Call between ImGui::BeginTabItem / EndTabItem.
void DrawHomeTab();
} // namespace Poseidon::Dev
