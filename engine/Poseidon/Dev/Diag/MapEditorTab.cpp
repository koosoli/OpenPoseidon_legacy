#include <Poseidon/Dev/Diag/MapEditorTab.hpp>
#include <Poseidon/Dev/Diag/TerrainBrush.hpp>
#include <Poseidon/Dev/Debug/DevPanelWidgets.hpp>
#include <Poseidon/World/Terrain/Landscape.hpp>
#include <Poseidon/World/Terrain/TerrainCrater.hpp>
#include <algorithm>
#undef DebugLog
#include <imgui.h>

namespace Poseidon::Dev
{
MapEditorAction DrawMapEditorTab()
{
    ImGui::SeparatorText("Terrain height painting");
    auto& brush = TerrainBrush();
    Checkbox("Enable terrain brush", &brush.enabled);
    Checkbox("Show brush radius at cursor", &brush.showRadius);
    if (RadioButton("Raise / build hills", brush.raise)) brush.raise = true;
    PanelSameLine();
    if (RadioButton("Lower / dig craters", !brush.raise)) brush.raise = false;
    SliderFloat("Brush radius", &brush.radius, 0.5f, 250.0f, "%.1f m");
    SliderFloat("Height change per second", &brush.metresPerSecond, 0.1f, 32.0f, "%.1f m/s");
    if (GLandscape)
    {
        const float grid = GLandscape->GetTerrainGrid();
        ImGui::Text("Terrain grid: %.1f m | Effective radius: %.1f m", grid, TerrainBrushRadius(brush.radius, grid));
        if (GLandscape->GetTerrainRange() > 2049)
            ImGui::TextWrapped("Painting unavailable: this world exceeds the current 2049-sample editing limit.");
    }
    MapEditorAction action = Button("Start painting (free-fly)") ? MapEditorAction::StartPainting : MapEditorAction::None;
    ImGui::BeginDisabled(!HasTerrainEditBaseline());
    if (Button("Restore terrain: all craters and painted hills")) ImGui::OpenPopup("Restore terrain?");
    ImGui::EndDisabled();
    if (ImGui::BeginPopupModal("Restore terrain?", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::TextUnformatted("Restore terrain to before the first edit in this mission?\n"
            "This removes brush edits, rocket craters and the LAB crater.\n"
            "It may pause briefly. Objects and settings are not reset.");
        if (Button("Restore all terrain edits"))
        {
            action = MapEditorAction::RestoreTerrain;
            ImGui::CloseCurrentPopup();
        }
        PanelSameLine();
        if (Button("Cancel terrain restore")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::SeparatorText("How to paint");
    ImGui::TextWrapped("1. Choose Raise or Lower, radius and speed.\n"
        "2. Click Start painting to enter free-fly and close this panel.\n"
        "3. Point the cursor at land and HOLD LEFT MOUSE. Longer holds change more height.\n"
        "4. WHEEL changes radius. Hold LEFT ALT while painting to reverse Raise/Lower temporarily.\n"
        "5. Release LEFT MOUSE to stop. Hold RIGHT MOUSE to look; WASD moves, Q/Z moves up/down.\n"
        "6. Ctrl+` / Ctrl+; reopens Dev Tools to change the brush or disable it.");
    ImGui::TextColored(ImVec4(1.0f,0.8f,0.3f,1.0f), "Mission-local only. No save/export yet.");
    if (ImGui::CollapsingHeader("Limitations / future map creation"))
    {
    ImGui::TextWrapped("Single-player only. The first stroke may briefly pause while collision is prepared. "
        "Coarse terrain uses a minimum radius of 1.5 grid cells. Water cannot be painted. "
        "Restore terrain removes all edits since the first stroke; reload also restores the mission. "
        "Buildings and AI paths are not updated.");
    ImGui::TextWrapped("Sideways caves and tunnels are not supported by height painting. "
        "They need 3D terrain geometry plus matching movement, projectile and physics collision; "
        "lowering this brush creates an open crater, not a cave with a roof.");
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
    return action;
}
}
