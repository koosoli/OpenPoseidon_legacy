#pragma once

// The dev-panel "Audio" tab: which audio engine runs (Steam Audio or OpenAL), the one to start
// next time, and the live HRTF (headphones) switch.

namespace Poseidon::Dev
{
/// Draws the tab contents.  Call between ImGui::BeginTabItem / EndTabItem.
void DrawAudioTab();
} // namespace Poseidon::Dev
