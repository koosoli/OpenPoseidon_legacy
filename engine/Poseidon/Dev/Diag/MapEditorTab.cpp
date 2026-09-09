#include <Poseidon/Dev/Diag/MapEditorTab.hpp>
#include <Poseidon/Dev/Diag/TerrainBrush.hpp>
#include <Poseidon/Dev/Debug/DevPanelWidgets.hpp>
#include <Poseidon/World/Terrain/Landscape.hpp>
#include <algorithm>
#undef DebugLog
#include <imgui.h>

namespace Poseidon::Dev
{
bool DrawMapEditorTab()
{
    ImGui::SeparatorText("Terrain height painting");
    auto& brush = TerrainBrush();
    Checkbox("Enable terrain brush", &brush.enabled);
    if (RadioButton("Raise / build hills", brush.raise)) brush.raise = true;
    PanelSameLine();
    if (RadioButton("Lower / dig craters", !brush.raise)) brush.raise = false;
    SliderFloat("Brush radius", &brush.radius, 8.0f, 250.0f, "%.1f m");
    SliderFloat("Height change per second", &brush.metresPerSecond, 0.1f, 32.0f, "%.1f m/s");
    if (GLandscape)
    {
        const float grid = GLandscape->GetTerrainGrid();
        ImGui::Text("Terrain grid: %.1f m | Effective radius: %.1f m", grid, std::max(brush.radius, grid*1.5f));
    }
    const bool start = Button("Start painting (free-fly)");
    ImGui::SeparatorText("How to paint");
    ImGui::TextWrapped("1. Choose Raise or Lower, radius and speed.\n"
        "2. Click Start painting to enter free-fly and close this panel.\n"
        "3. Point the cursor at land and HOLD LEFT MOUSE. Longer holds change more height.\n"
        "4. Release LEFT MOUSE to stop. Hold RIGHT MOUSE to look; WASD moves, Q/Z moves up/down.\n"
        "5. Ctrl+` / Ctrl+; reopens Dev Tools to change the brush or disable it.");
    ImGui::TextColored(ImVec4(1.0f,0.8f,0.3f,1.0f), "Mission-local only. No save/export yet.");
    if (ImGui::CollapsingHeader("Limitations / future map creation"))
    {
    ImGui::TextWrapped("Single-player only. The first stroke may briefly pause while collision is prepared. "
        "Coarse terrain uses a minimum radius of 1.5 grid cells. Water cannot be painted. "
        "Reload the mission to restore its original terrain. Buildings and AI paths are not updated.");
    ImGui::TextWrapped("New maps from scratch, save/export and undo history are not implemented yet. "
        "This tab is the home for those future editor tools, not a finished map authoring pipeline.");
    }
    if (Button("Reset brush settings (not terrain)")) ResetTerrainBrush();
    ImGui::SeparatorText("Rocket impact craters");
    auto& craters = RocketCraters();
    Checkbox("Rockets create terrain craters", &craters.enabled);
    SliderFloat("Rocket crater radius", &craters.radius, 8.0f, 100.0f, "%.1f m");
    SliderFloat("Rocket crater depth", &craters.depth, 0.1f, 8.0f, "%.1f m");
    ImGui::TextWrapped("Experimental, single-player, mission-local. Land impacts only; "
        "minimum radius is 1.5 terrain grid cells. Buildings and AI paths are not updated. "
        "The first impact may pause briefly to prepare terrain collision. Turning off does not undo craters.");
    if (Button("Reset rocket crater settings")) ResetRocketCraters();
    return start;
}
}
