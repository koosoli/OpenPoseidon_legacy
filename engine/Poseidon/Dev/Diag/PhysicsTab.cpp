// PHY-020: spawn a ball, watch it fall on Everon.
//
// Kept small on purpose. This is a probe UI, not a physics editor -- the question
// it exists to answer is whether Box3D can move a dynamic body at 60 Hz across
// the real terrain and the real Geometry-LOD colliders, and every control here
// earns its place by making that answer easier to see.

#include <Poseidon/Dev/Diag/PhysicsTab.hpp>

#include <Poseidon/Dev/Diag/PhysicsProbe.hpp>
#include <Poseidon/World/Physics/PhysicsWorld.hpp>

// See the note in PictureModeTab.cpp: Foundation's DebugLog macro eats an ImGui
// member of the same name. Nothing here logs.
#undef DebugLog

#include <imgui.h>

namespace Poseidon::Dev
{

void DrawPhysicsTab()
{
    Physics::PhysicsWorld* world = Physics::GetPhysicsWorld();
    const bool             live = world && world->IsCreated();

    ImGui::SeparatorText("Physics world");
    if (!live)
    {
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "No physics world yet.");
        ImGui::TextDisabled("It is created on the first Drop or Throw. Nothing in the game\n"
                            "uses physics, so the world is not built until asked for.");
    }
    else
    {
        const Physics::PhysicsStats stats = world->GetStats();
        ImGui::Text("Backend: %s", world->BackendName());
        ImGui::Text("Static bodies %u, shapes %u", stats.bodies, stats.shapes);
        ImGui::Text("Terrain: %s (%ux%u)", stats.terrainRegistered ? "registered" : "none", stats.terrainWidth,
                    stats.terrainHeight);
        ImGui::Text("Live probes: %u", stats.probes);
        if (stats.modelsRegistered > 0)
        {
            ImGui::TextDisabled("Models registered %u | no geometry %u | degenerate %u | hull refusals %u",
                                stats.modelsRegistered, stats.modelsWithoutGeometry, stats.degenerateComponents,
                                stats.hullFailures);
        }
        else
        {
            ImGui::TextDisabled("No world geometry registered — set POSEIDON_PHYSICS_CORPUS=1\n"
                                "to have the whole map registered on load, or the ball will\n"
                                "fall through everything except the terrain.");
        }
    }

    PhysicsProbeSettings& s = ProbeSettings();

    ImGui::SeparatorText("Probe");
    ImGui::SetNextItemWidth(200.0f);
    ImGui::SliderFloat("Radius (m)", &s.radius, 0.02f, 2.0f, "%.2f");
    ImGui::SetNextItemWidth(200.0f);
    ImGui::SliderFloat("Mass (kg)", &s.mass, 0.01f, 100.0f, "%.2f", ImGuiSliderFlags_Logarithmic);
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Kilograms, converted to Box3D's density through the sphere's own\n"
                          "volume. A slider in kg/m3 would be exact and useless to picture.");
    }
    ImGui::SetNextItemWidth(200.0f);
    ImGui::SliderFloat("Friction", &s.friction, 0.0f, 2.0f, "%.2f");
    ImGui::SetNextItemWidth(200.0f);
    ImGui::SliderFloat("Restitution", &s.restitution, 0.0f, 1.0f, "%.2f");
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Bounciness. 0 = dead drop, 1 = returns to its release height.");
    }
    ImGui::SetNextItemWidth(200.0f);
    ImGui::SliderFloat("Throw speed (m/s)", &s.throwSpeed, 1.0f, 60.0f, "%.1f");

    ImGui::Spacing();
    if (ImGui::Button("Drop"))
    {
        SpawnPhysicsProbe(0.0f);
    }
    ImGui::SameLine();
    if (ImGui::Button("Throw"))
    {
        SpawnPhysicsProbe(s.throwSpeed);
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear"))
    {
        ClearPhysicsProbes();
    }
    ImGui::SameLine();
    ImGui::Checkbox("Draw", &s.draw);
    ImGui::TextDisabled("Spawns 2 m in front of the camera.");

    ImGui::SeparatorText("Wind");
    ImGui::Checkbox("Apply world wind", &s.applyWind);
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Routes Poseidon's own weather wind into the probe through\n"
                          "Box3D's b3Shape_ApplyWind, which computes the relative air\n"
                          "speed itself. No second drag model is involved — two models\n"
                          "for one force is how they end up disagreeing.");
    }
    ImGui::BeginDisabled(!s.applyWind);
    ImGui::SetNextItemWidth(200.0f);
    ImGui::SliderFloat("Wind scale", &s.windScale, 0.0f, 50.0f, "%.1fx");
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("World wind is a few m/s and barely moves a half-kilo ball.\n"
                          "Turn this up to make the effect visible while checking, then\n"
                          "back to 1 for anything you want to believe.");
    }
    ImGui::SetNextItemWidth(200.0f);
    ImGui::SliderFloat("Drag", &s.windDrag, 0.0f, 8.0f, "%.2f");
    ImGui::EndDisabled();

    if (live)
    {
        const Physics::WindSettings& wind = world->GetWind();
        ImGui::Text("Wind now: %.2f, %.2f, %.2f  (%.2f m/s)", wind.velocity[0], wind.velocity[1], wind.velocity[2],
                    wind.velocity.Size());
    }
}

} // namespace Poseidon::Dev
