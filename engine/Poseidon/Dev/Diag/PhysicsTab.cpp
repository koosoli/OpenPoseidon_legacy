// PHY-020: spawn a ball, watch it fall on Everon.
//
// Kept small on purpose. This is a probe UI, not a physics editor -- the question
// it exists to answer is whether Box3D can move a dynamic body at 60 Hz across
// the real terrain and the real Geometry-LOD colliders, and every control here
// earns its place by making that answer easier to see.

#include <Poseidon/Dev/Diag/PhysicsTab.hpp>

#include <Poseidon/Dev/Diag/PhysicsCorpus.hpp>
#include <Poseidon/Dev/Diag/PhysicsProbe.hpp>
#include <Poseidon/Dev/Diag/PhysicsRayAudit.hpp>
#include <Poseidon/World/Physics/PhysicsWorld.hpp>

// See the note in PictureModeTab.cpp: Foundation's DebugLog macro eats an ImGui
// member of the same name. Nothing here logs.
#undef DebugLog

#include <imgui.h>

namespace Poseidon::Dev
{

namespace
{

/// Hotkeys read straight from ImGui, not through the engine's cheat actions.
///
/// The first attempt used GetCheat2ToDo, and Cheat2 is a BOUND ACTION rather than
/// a fixed key -- so "Ctrl+B" was an invention, and with nothing bound to that
/// action the keys did nothing at all. ImGui sees the physical keys regardless of
/// what the game's bindings say, which is the right level for a dev tool.
///
/// Ctrl+J / K / L, chosen to miss the map (M), the journal and the rest of the
/// stock bindings -- and Ctrl is held, so they cannot collide with a bare key.
void PollHotkeys(PhysicsProbeSettings& s)
{
    if (!s.hotkeys)
    {
        return;
    }
    const ImGuiIO& io = ImGui::GetIO();
    // Never while a field is being typed into: J in a model name must stay a J.
    if (io.WantTextInput || !io.KeyCtrl)
    {
        return;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_J, false))
    {
        PlaceProbeAtCrosshair();
    }
    if (ImGui::IsKeyPressed(ImGuiKey_K, false))
    {
        SpawnPhysicsProbe(0.0f);
    }
    if (ImGui::IsKeyPressed(ImGuiKey_L, false))
    {
        SpawnPhysicsProbe(s.throwSpeed);
    }
}

} // namespace

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
        if (stats.stepsTimed > 0)
        {
            ImGui::Text("Step cost: %.3f ms mean, %.3f ms worst (%llu steps)", stats.meanStepMs, stats.worstStepMs,
                        static_cast<unsigned long long>(stats.stepsTimed));
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip(
                    "Cost of ONE simulation step. Multiply by the Fixed Step tab's\n"
                    "ticks-per-frame for the per-frame cost -- kept separate because a\n"
                    "step is a fixed unit of work and a frame is not.\n"
                    "\n"
                    "The worst figure matters more than the mean: a mean hides the one\n"
                    "hitch a player actually feels.");
            }
        }
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
    ImGui::SliderInt("Terrain detail", &TerrainSubdivision(), 1, 16, "%dx");
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
    static const char* shapeNames[] = {"Sphere", "Capsule", "Box", "Cylinder", "Model"};
    ImGui::SetNextItemWidth(200.0f);
    ImGui::Combo("Shape", &s.shape, shapeNames, 5);
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
    ImGui::Checkbox("Solid", &s.drawSolid);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(160.0f);
    ImGui::ColorEdit3("Colour", s.solidColor, ImGuiColorEditFlags_NoInputs);
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip(
            "Primitives only. A model probe keeps its own textures --\n"
            "tinting a jeep red would hide the thing you spawned it to see.");
    }
    ImGui::SameLine();
    ImGui::Checkbox("Outline", &s.drawOutline);
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Wireframe on top of the solid. Useful when a probe is resting\n"
                          "against scenery of a similar colour.");
    }
    PollHotkeys(s);
    ImGui::Checkbox("Hotkeys", &s.hotkeys);
    ImGui::SameLine();
    ImGui::TextDisabled("Ctrl+J place | Ctrl+K drop | Ctrl+L throw");
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip(
            "Keys rather than buttons: the panel holds the mouse while it is open,\n"
            "so a domino row has to be laid by aiming and tapping. Clicking a button\n"
            "and hoping the camera has not moved is not placing anything.\n"
            "\n"
            "OFF by default -- until you ask for them these stay ordinary game keys,\n"
            "and a dev tool should not take three of them from a player who never\n"
            "opened this tab.");
    }
    if (s.shape == 2)
    {
        static int towerHeight = 12;
        ImGui::SetNextItemWidth(140.0f);
        ImGui::SliderInt("Tower bricks", &towerHeight, 2, 60);
        ImGui::SameLine();
        if (ImGui::Button("Build tower"))
        {
            SpawnBrickTower(towerHeight);
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip(
                              "Alternate courses are turned a quarter, the way a real wall is\n"
                              "laid. Aligned blocks are a set of separate columns and fall\n"
                              "apart at a touch.\n");
        }
    }
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

    ImGui::SeparatorText("Model probe");
    ImGui::SetNextItemWidth(200.0f);
    ImGui::InputText("Model name contains", s.modelFilter, sizeof(s.modelFilter));
    ImGui::SetNextItemWidth(200.0f);
    ImGui::SliderFloat("Model mass (kg)", &s.modelMass, 10.0f, 20000.0f, "%.0f", ImGuiSliderFlags_Logarithmic);
    ImGui::TextDisabled("Pick Shape = Model, then Drop or Throw. e.g. jeep, uaz, t72");
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip(
            "Takes a model already loaded in this world and makes a DYNAMIC copy of\n"
            "it: the real visual shape, with a body built from its authored Geometry\n"
            "LOD -- several convex hulls, not one box -- so it tumbles according to\n"
            "its own collision shape.\n"
            "\n"
            "It is a separate instance. It is not a gameplay vehicle, it is not in the\n"
            "landscape object lists, no AI or vehicle simulation touches it, it is not\n"
            "networked, and it cannot reach a mission or a save. Clear removes it\n"
            "completely.\n");
    }

    ImGui::SeparatorText("Interaction");
    ImGui::SetNextItemWidth(200.0f);
    ImGui::SliderFloat("Hit impulse scale", &s.hitImpulseScale, 0.0f, 500.0f, "%.0fx");
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip(
            "Shots push probes. The physics world is queried with the bullet's own\n"
            "segment each tick, and the impulse is applied AT THE HIT POINT -- so an\n"
            "off-centre hit spins the target instead of merely shoving it.\n"
            "\n"
            "A rifle round really carries about 4 Ns, which moves a domino and does\n"
            "nothing at all to a jeep. This is an openly labelled DEV MULTIPLIER, not\n"
            "a physical bullet mass: inventing one would look like data and be a\n"
            "guess.\n"
            "\n"
            "Legacy collision and damage are untouched.\n");
    }
    ImGui::Checkbox("Player can push probes", &s.playerProxy);
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip(
            "Follows the player with a KINEMATIC capsule: it shoves dynamic probes\n"
            "and nothing shoves it back. Physics gains no authority over where you\n"
            "are -- the flow stays player -> physics, never the reverse.\n");
    }
    if (s.playerProxy)
    {
        ImGui::SetNextItemWidth(200.0f);
        ImGui::SliderFloat("Player radius", &s.playerRadius, 0.1f, 1.5f, "%.2f");
        ImGui::SetNextItemWidth(200.0f);
        ImGui::SliderFloat("Player height", &s.playerHeight, 0.5f, 3.0f, "%.2f");
    }

    ImGui::SeparatorText("Ray audit (PHY-030)");
    bool audit = RayAuditEnabled();
    if (ImGui::Checkbox("Compare physics rays with the 2001 path", &audit))
    {
        SetRayAudit(audit);
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip(
            "Every projectile segment is asked of BOTH collision systems and the\n"
            "disagreements are counted. Replaces nothing -- the legacy answer is\n"
            "still the one the game acts on.\n"
            "\n"
            "The corpus census says 82,381 objects converted. It cannot say whether\n"
            "they converted to the right SHAPE. Two rays over one segment can.\n"
            "\n"
            "Costs a second ray per projectile step, so it is off by default.");
    }
    {
        const RayAuditStats a = GetRayAuditStats();
        ImGui::Text("Segments %llu | agree hit %llu | agree miss %llu",
                    static_cast<unsigned long long>(a.segments), static_cast<unsigned long long>(a.agreeHit),
                    static_cast<unsigned long long>(a.agreeMiss));
        ImGui::TextColored(a.legacyOnly ? ImVec4(1.0f, 0.7f, 0.3f, 1.0f) : ImVec4(0.6f, 0.6f, 0.6f, 1.0f),
                           "Legacy hit, physics missed: %llu",
                           static_cast<unsigned long long>(a.legacyOnly));
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip(
                "The interesting one: geometry the game collides with that the\n"
                "physics world does not have, or has smaller than it draws.");
        }
        ImGui::TextDisabled("Physics hit, legacy missed: %llu",
                            static_cast<unsigned long long>(a.physicsOnly));
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip(
                "Expected to be non-zero: the terrain is in the physics world and\n"
                "the projectile path tests it separately, so this is not a fault\n"
                "by itself.");
        }
        if (ImGui::Button("Reset audit"))
        {
            ResetRayAudit();
        }
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
