#pragma once

// The dev-panel "Fixed Step" tab: what the simulation accumulator is doing.
// Declared in its own header for the reason BallisticsTab.hpp gives -- hosting it
// in Dev/Debug/DebugOverlay.cpp stays a five-line change, so concurrent
// workstreams never have to merge a tab body.

namespace Poseidon::Dev
{
/// Draws the tab contents. Call between ImGui::BeginTabItem / EndTabItem.
void DrawFixedStepTab();
} // namespace Poseidon::Dev
