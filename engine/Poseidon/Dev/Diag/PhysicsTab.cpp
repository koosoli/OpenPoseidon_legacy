// PHY-020: spawn a ball, watch it fall on Everon.
//
// Kept small on purpose. This is a probe UI, not a physics editor -- the question
// it exists to answer is whether Box3D can move a dynamic body at 60 Hz across
// the real terrain and the real Geometry-LOD colliders, and every control here
// earns its place by making that answer easier to see.

#include <Poseidon/Dev/Diag/PhysicsTab.hpp>

#include <Poseidon/Dev/Diag/PhysicsCorpus.hpp>
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
            ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f),
                               "Terrain only -- the ball will fall THROUGH buildings.\nPress Register world colliders.");
        }
    }

    ImGui::Spacing();
    ImGui::SetNextItemWidth(200.0f);
    ImGui::SliderInt("Terrain detail", &TerrainSubdivision(), 1, 12, "%dx");
    ImGui::SameLine();
    ImGui::TextDisabled("%.2f m samples", TerrainSampleSpacing());
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip(
            "Height samples per 50 m landscape cell.\n"
            "\n"
            "Between samples Box3D spans a straight chord while the visible ground\n"
            "curves above it, so on a hillside a ball rests BELOW the slope you can\n"
            "see. Raising this is what closes that gap. 4x = 12.5 m and 4 MB,\n"
            "8x = 6.25 m and 17 MB.\n"
            "\n"
            "Past a point the remaining error is the engine's own interpolation\n"
            "rather than our sampling, so more stops helping.\n"
            "\n"
            "Takes effect when you register again.");
    }

    if (ImGui::Button(PhysicsCorpusHasRun() ? "Re-register world colliders" : "Register world colliders"))
    {
        RunPhysicsCorpus();
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip(
                          "Builds the terrain height field and a collider for every object on\n"
                          "the map -- about 82,000 bodies on Everon, a second or two.\n"
                          "\n"
                          "Until this is pressed the ball only meets the ground, because\n"
                          "nothing in the game uses physics yet, and building 82,000 bodies\n"
                          "for every session that never opens this tab would be rude.\n"
                          "\n"
                          "POSEIDON_PHYSICS_CORPUS=1 does the same at load, for capture runs.\n");
    }

    PhysicsProbeSettings& s = ProbeSettings();

    ImGui::SeparatorText("Probe");
    static const char* shapeNames[] = {"Sphere", "Capsule", "Box", "Cylinder"};
    ImGui::SetNextItemWidth(200.0f);
    ImGui::Combo("Shape", &s.shape, shapeNames, 4);
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip(
            "A capsule topples and slides the way something body-shaped does,\n"
            "which a sphere cannot. It is NOT a ragdoll and is not one step away\n"
            "from being one: a ragdoll needs a joint per bone pair and a skeleton\n"
            "hierarchy the engine does not carry. RTM stores a matrix per named\n"
            "bone but as a FLAT LIST with no parent/child relation, so there is\n"
            "nothing yet to connect. That table is PHY-040.\n");
    }
    if (s.shape == 1 || s.shape == 3)
    {
        ImGui::SetNextItemWidth(200.0f);
        ImGui::SliderFloat("Half length (m)", &s.halfLength, 0.05f, 3.0f, "%.2f");
        ImGui::SameLine();
        ImGui::TextDisabled("total %.2f m", 2.0f * (s.halfLength + (s.shape == 1 ? s.radius : 0.0f)));
    }
    if (s.shape == 2)
    {
        ImGui::SetNextItemWidth(200.0f);
        ImGui::SliderFloat("Half width X", &s.boxX, 0.02f, 3.0f, "%.2f");
        ImGui::SetNextItemWidth(200.0f);
        ImGui::SliderFloat("Half height Y", &s.boxY, 0.02f, 3.0f, "%.2f");
        ImGui::SetNextItemWidth(200.0f);
        ImGui::SliderFloat("Half depth Z", &s.boxZ, 0.02f, 3.0f, "%.2f");
        if (ImGui::Button("Domino"))
        {
            s.boxX = 0.03f;
            s.boxY = 0.25f;
            s.boxZ = 0.12f;
            s.mass = 0.4f;
            s.friction = 0.6f;
            s.restitution = 0.05f;
        }
        ImGui::SameLine();
        if (ImGui::Button("Brick"))
        {
            s.boxX = 0.11f;
            s.boxY = 0.05f;
            s.boxZ = 0.24f;
            s.mass = 3.0f;
            s.friction = 0.8f;
            s.restitution = 0.02f;
        }
        ImGui::SameLine();
        ImGui::TextDisabled("starting points");
    }
    ImGui::SetNextItemWidth(200.0f);
    ImGui::SliderFloat("Radius (m)", &s.radius, 0.02f, 10.0f, "%.2f", ImGuiSliderFlags_Logarithmic);
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
    ImGui::SliderFloat("Rolling resistance", &s.rollingResistance, 0.0f, 0.5f, "%.3f");
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip(
                          "0 makes a sphere roll for ever on any slope: sliding friction\n"
                          "never opposes rolling, so raising Friction does not stop it.\n"
                          "This is the control that actually lets a ball settle.\n");
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
    ImGui::Checkbox("Place on the ground under the crosshair", &s.placeOnGround);
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip(
            "Off: the body appears 2 m in front of the camera and falls.\n"
            "\n"
            "On: Drop sets it down on the ground you are looking at, already\n"
            "resting. A domino row is unbuildable without this -- every piece\n"
            "would arrive falling and land wherever it bounced.\n"
            "\n"
            "Throw ignores it: a thrown body is meant to be in the air.\n");
    }

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
