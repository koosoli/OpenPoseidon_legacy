#include <Poseidon/Dev/Diag/MapEditorTab.hpp>
#include <Poseidon/Dev/Diag/TerrainBrush.hpp>
#include <Poseidon/Dev/Diag/CaveEditor.hpp>
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
    if (Checkbox("Enable terrain brush", &brush.enabled) && brush.enabled) CaveEditor().enabled = false;
    Checkbox("Show brush radius at cursor", &brush.showRadius);
    if (RadioButton("Raise / build hills", brush.raise)) brush.raise = true;
    PanelSameLine();
    if (RadioButton("Lower / dig craters", !brush.raise)) brush.raise = false;
    SliderFloat("Brush radius", &brush.radius, 0.1f, 250.0f, "%.1f m");
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
    ImGui::SeparatorText("Caves and straight trenches");
    auto& cave = CaveEditor();
    if (RadioButton("Horizontal cave", cave.tool==ExcavationTool::Cave)) SetExcavationTool(ExcavationTool::Cave);
    if (RadioButton("Infantry trench", cave.tool==ExcavationTool::InfantryTrench)) SetExcavationTool(ExcavationTool::InfantryTrench);
    if (RadioButton("Tank trench", cave.tool==ExcavationTool::TankTrench)) SetExcavationTool(ExcavationTool::TankTrench);
    Checkbox("Enable shaped excavation", &cave.enabled);
    if (cave.enabled) brush.enabled = false;
    const bool isCave=cave.tool==ExcavationTool::Cave;
    SliderFloat("Opening width", &cave.width, 0.1f, 12.0f, "%.2f m");
    SliderFloat(isCave ? "Opening height" : "Trench depth", &cave.height, 0.1f, isCave ? 8.0f : 4.0f, "%.2f m");
    SliderFloat("Length into ground", &cave.length, 1.0f, 64.0f, "%.1f m");
    if (isCave) SliderFloat("Entrance floor offset", &cave.floorOffset, -4.0f, 2.0f, "%.2f m");
    else ImGui::Text("Access ramp: %.1f m, flat floor: %.1f m",
        cave.height*(cave.tool==ExcavationTool::TankTrench ? 6.0f : 3.0f),
        std::max(0.0f,cave.length-cave.height*(cave.tool==ExcavationTool::TankTrench ? 6.0f : 3.0f)));
    if (Button("Place excavation (free-fly)")) action = MapEditorAction::StartCave;
    ImGui::BeginDisabled(EditorCaveCount() == 0);
    if (Button("Undo last excavation")) action = MapEditorAction::UndoCave;
    ImGui::EndDisabled();
    ImGui::Text("Excavations this mission: %zu / 8", EditorCaveCount());
    ImGui::TextWrapped("%s", EditorCaveStatus());
    ImGui::TextWrapped("LEFT MOUSE places the selected shape, pointing along the camera's compass heading. "
        "WHEEL resizes the opening down to 10 cm; SHIFT+WHEEL changes length. "
        "Caves need rising ground and preserve terrain above their ceiling. "
        "Straight trenches need gently sloping ground; their ramps descend to a flat floor. "
        "Infantry and tank presets have different widths and access ramps. "
        "Very small openings cannot fit a soldier. Mission-local prototype; no save/export or AI routing yet.");
    ImGui::SeparatorText("How to paint");
    ImGui::TextWrapped("1. Choose Raise or Lower, radius and speed.\n"
        "2. Click Start painting to enter free-fly and close this panel.\n"
        "3. Point the cursor at land and HOLD LEFT MOUSE. Longer holds change more height.\n"
        "4. WHEEL changes radius. Hold SHIFT to dig/raise four times faster; LEFT ALT reverses Raise/Lower.\n"
        "5. Release LEFT MOUSE to stop. Hold RIGHT MOUSE to look; WASD moves, Q/Z moves up/down.\n"
        "6. Ctrl+` / Ctrl+; reopens Dev Tools to change the brush or disable it.");
    ImGui::TextColored(ImVec4(1.0f,0.8f,0.3f,1.0f), "Mission-local only. No save/export yet.");
    if (ImGui::CollapsingHeader("Limitations / future map creation"))
    {
    ImGui::TextWrapped("Single-player only. The first stroke may briefly pause while collision is prepared. "
        "Minimum radius is 0.75 grid cells; small brushes move existing vertices, not sub-grid geometry. Water cannot be painted. "
        "Restore terrain removes all edits since the first stroke; reload also restores the mission. "
        "Buildings and AI paths are not updated.");
    ImGui::TextWrapped("Use Horizontal holes / caves for sideways openings. "
        "Height painting creates open craters; its smallest effective radius depends on the terrain grid. "
        "Cave openings can be smaller than that grid. Box3D dynamic bodies still use the original terrain heightfield.");
    ImGui::TextWrapped("New maps from scratch, save/export and undo history are not implemented yet. "
        "This tab is the home for those future editor tools, not a finished map authoring pipeline.");
    }
    if (Button("Reset brush settings (not terrain)")) ResetTerrainBrush();
    ImGui::SeparatorText("Ballistic impact craters");
    auto& craters = RocketCraters();
    Checkbox("Rockets / tank shells create terrain craters", &craters.enabled);
    SliderFloat("Reference crater radius (LAW)", &craters.radius, 8.0f, 100.0f, "%.1f m");
    SliderFloat("Reference crater depth (LAW)", &craters.depth, 0.1f, 8.0f, "%.1f m");
    ImGui::TextWrapped("Radius and depth scale with ammo blast damage (cube root, 0.25x to 2x). "
        "Defaults: LAW reference 12 m radius / 1 m depth; terrain grid can widen small craters.");
    ImGui::TextWrapped("Experimental, single-player, mission-local. Land impacts only; "
        "minimum radius is 0.75 terrain grid cells. Buildings and AI paths are not updated. "
        "The first impact may pause briefly to prepare terrain collision. Turning off does not undo craters.");
    if (Button("Reset impact crater settings")) ResetRocketCraters();
    return action;
}
}
