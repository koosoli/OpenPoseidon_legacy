// The dev-panel "Smoke" tab.
//
// Three jobs, in the order the panel presents them:
//
//   1. Spawn plumes — new-system or legacy — so the two can be put next to each
//      other under one wind and one camera. Comparing them any other way is
//      guesswork.
//   2. Tune the new system live. Every slider affects particles already in the
//      air, not only newly emitted ones, because "raise the thickness and see"
//      is the whole workflow.
//   3. Cost. Particle count, simulate and draw microseconds, and world-query
//      sweeps per frame, for both systems. The sweep counter is the one that
//      matters: it is the number that says whether the collision budget is
//      actually being rationed, and unlike a frame time it cannot be flattered
//      by a quiet scene.

#include <Poseidon/Foundation/Framework/Log.hpp>
#include <Poseidon/Foundation/platform.hpp>

#ifdef DebugLog
#undef DebugLog
#endif

#include <imgui.h>

#include <Poseidon/Dev/Diag/SmokeTab.hpp>
#include <Poseidon/Core/Global.hpp>
#include <Poseidon/World/World.hpp>
#include <Poseidon/World/Scene/Scene.hpp>
#include <Poseidon/World/Scene/Camera/Camera.hpp>
#include <Poseidon/World/Effects/SmokeSystem.hpp>
#include <Poseidon/World/Effects/SmokeWorldQuery.hpp>
#include <Poseidon/World/Weather/AirflowField.hpp>
#include <Poseidon/World/Weather/WindModel.hpp>

#include <algorithm>
#include <cmath>

namespace Poseidon::Dev
{

namespace
{

/// The parameter block the spawn buttons use. Persisted in the panel rather
/// than in the system: it is the developer's working set, not world state.
SmokeParams GParams{};
int GColorPreset = 0;
float GSpawnDuration = -1.0f;
float GSpawnDistance = 8.0f;
float GLegacyDensity = 1.0f;
float GLegacySize = 1.5f;

/// Rolling averages. A single frame's microsecond count is noise; the thing you
/// want to read off a benchmark is where it settles.
constexpr int kHistory = 120;
float GSimHistory[kHistory] = {};
float GDrawHistory[kHistory] = {};
int GHistoryCursor = 0;
bool GHistoryPrimed = false;

void PushHistory(float simUs, float drawUs)
{
    GSimHistory[GHistoryCursor] = simUs;
    GDrawHistory[GHistoryCursor] = drawUs;
    GHistoryCursor = (GHistoryCursor + 1) % kHistory;
    if (GHistoryCursor == 0)
    {
        GHistoryPrimed = true;
    }
}

float Average(const float (&values)[kHistory])
{
    const int count = GHistoryPrimed ? kHistory : std::max(GHistoryCursor, 1);
    float total = 0.0f;
    for (int i = 0; i < count; ++i)
    {
        total += values[i];
    }
    return total / static_cast<float>(count);
}

/// Where a spawn goes: `GSpawnDistance` metres in front of the camera, dropped
/// to the ground. Ground-level is what you want almost always — a plume born in
/// mid-air tells you nothing about collision or pooling.
///
/// The SCENE camera, not GWorld->CameraOn(). CameraOn() is the entity the view
/// is attached to; in a free-fly / Zeus flight that is the player left standing
/// wherever they were, while the picture on screen comes from a CameraVehicle
/// the panel owns privately. Spawning off CameraOn() put the plume at the
/// player's feet, out of frame, which read as "spawn does nothing in freefly".
/// GScene->GetCamera() is what is actually being rendered, in every mode.
bool SpawnPosition(Vector3& out)
{
    if (GWorld == nullptr || GScene == nullptr || GScene->GetCamera() == nullptr)
    {
        return false;
    }

    const Camera& camera = *GScene->GetCamera();
    const Vector3 forward = camera.Direction();
    Vector3 position = camera.Position() + forward * GSpawnDistance;
    // Drop to the floor of the storey the CAMERA is on, not the topmost
    // surface. Indoors the topmost surface is the roof, and clamping to it put
    // every plume on top of the building -- which is why spawning indoors
    // "almost always" landed outside. Probing from the camera's height finds
    // the room's floor.
    Vector3 probe = position;
    probe[1] = camera.Position().Y();
    position[1] = GSmokeWorldQuery().FloorHeightBelow(probe) + 0.2f;
    out = position;
    return true;
}

/// Spawn right where the camera is, floor-clamped. The only reliable way to put
/// a plume in a specific room: walk in, press the button.
bool SpawnPositionHere(Vector3& out)
{
    if (GWorld == nullptr || GScene == nullptr || GScene->GetCamera() == nullptr)
    {
        return false;
    }
    const Camera& camera = *GScene->GetCamera();
    Vector3 position = camera.Position();
    position[1] = GSmokeWorldQuery().FloorHeightBelow(position) + 0.2f;
    out = position;
    return true;
}

void DrawSpawnSection()
{
    ImGui::SeparatorText("Spawn");

    const bool haveWorld = GWorld != nullptr && GScene != nullptr && GScene->GetCamera() != nullptr;
    if (!haveWorld)
    {
        ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), "No world / no camera - nothing can be spawned.");
        return;
    }

    ImGui::SetNextItemWidth(140.0f);
    ImGui::SliderFloat("Distance ahead (m)", &GSpawnDistance, 2.0f, 60.0f, "%.0f");

    ImGui::SetNextItemWidth(140.0f);
    ImGui::InputFloat("Emit duration (s)", &GSpawnDuration, 0.0f, 0.0f, "%.0f");
    ImGui::SameLine();
    ImGui::TextDisabled("(negative = forever)");

    // -- colour -----------------------------------------------------------
    const char* presetNames[] = {"White", "Black", "Red", "Green", "Blue", "Yellow", "Purple", "Orange"};
    ImGui::SetNextItemWidth(140.0f);
    if (ImGui::Combo("Colour", &GColorPreset, presetNames, IM_ARRAYSIZE(presetNames)))
    {
        ApplySmokeColorPreset(GParams, static_cast<SmokeColorPreset>(GColorPreset));
    }
    ImGui::SameLine();
    ImGui::ColorButton("##smokeTint", ImVec4(GParams.red, GParams.green, GParams.blue, 1.0f),
                       ImGuiColorEditFlags_NoTooltip, ImVec2(20, 20));
    ImGui::SameLine();
    float tint[3] = {GParams.red, GParams.green, GParams.blue};
    ImGui::SetNextItemWidth(180.0f);
    if (ImGui::ColorEdit3("Custom tint", tint, ImGuiColorEditFlags_NoInputs))
    {
        GParams.red = tint[0];
        GParams.green = tint[1];
        GParams.blue = tint[2];
    }

    ImGui::Spacing();

    Vector3 position;
    const bool havePosition = SpawnPosition(position);

    ImGui::Checkbox("Fire at the base", &GParams.fire);
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Adds a flame layer at the emitter: short-lived additive sprites on\n"
                          "a white-yellow-orange-red cooling ramp, and extra lift on the smoke\n"
                          "above them. Pair with the Black colour preset for a burning wreck.");
    }
    ImGui::SameLine();
    if (ImGui::Button("Burning wreck"))
    {
        GParams = SmokeParams{};
        GParams.fire = true;
        GColorPreset = 1; // Black
        ApplySmokeColorPreset(GParams, SmokeColorPreset::Black);
        GParams.opacity = 0.68f;
        GParams.selfShadowStrength = 3.0f;
        GParams.groundShadow = 2.2f;
        GParams.rate = 45.0f;
        GParams.emitterRadius = 1.2f;
        GParams.startRadius = 0.8f;
        GParams.endRadius = 6.0f;
        GParams.particleLifetime = 18.0f;
    }
    ImGui::SameLine();
    if (ImGui::Button("Campfire"))
    {
        GParams = SmokeParams{};
        GParams.fire = true;
        GColorPreset = 0;
        ApplySmokeColorPreset(GParams, SmokeColorPreset::White);
        GParams.opacity = 0.18f;
        GParams.emitterRadius = 0.35f;
        GParams.fireRate = 30.0f;
        GParams.fireStartRadius = 0.25f;
        GParams.fireEndRadius = 0.6f;
        GParams.fireRiseSpeed = 1.8f;
        GParams.fireLifetime = 0.7f;
        GParams.fireSmokeYield = 0.25f;
        GParams.startRadius = 0.3f;
        GParams.endRadius = 2.5f;
        GParams.fireLightRadius = 8.0f;
        GParams.fireLightIntensity = 0.6f;
    }
    ImGui::SameLine();
    if (ImGui::Button("INFERNO"))
    {
        // Everything up: a wide, hot, roaring fire that chokes on its own smoke.
        GParams = SmokeParams{};
        GParams.fire = true;
        GColorPreset = 1;
        ApplySmokeColorPreset(GParams, SmokeColorPreset::Black);
        GParams.opacity = 0.8f;
        GParams.selfShadowStrength = 3.2f;
        GParams.groundShadow = 2.6f;
        GParams.emitterRadius = 4.0f;
        GParams.fireRate = 220.0f;
        GParams.maxFlames = 400;
        GParams.fireStartRadius = 1.2f;
        GParams.fireEndRadius = 3.5f;
        GParams.fireRiseSpeed = 6.0f;
        GParams.fireLifetime = 1.4f;
        GParams.fireIntensity = 2.2f;
        GParams.fireStretch = 2.2f;
        GParams.fireSmokeYield = 0.9f;
        GParams.fireSmokeLift = 5.0f;
        GParams.startRadius = 1.5f;
        GParams.endRadius = 10.0f;
        GParams.particleLifetime = 25.0f;
        GParams.maxParticles = 800;
        GParams.fireLightRadius = 22.0f;
        GParams.fireLightIntensity = 1.8f;
        // A COLUMN, not a swarm of puffs.
        GParams.fireColumnHeight = 10.0f;
        GParams.fireColumnCohesion = 3.5f;
        GParams.fireLifetime = 2.2f;
        GParams.fireRiseSpeed = 5.0f;
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Wide, hot, roaring. Also try 'Spawn ring of fires' below to cover an\n"
                          "area - a burning building is many fires, not one big one.");
    }

    ImGui::BeginDisabled(!havePosition);
    if (ImGui::Button("Spawn NEW smoke", ImVec2(160, 0)))
    {
        GSmokeSystem.Spawn(position, GParams, GSpawnDuration);
    }
    ImGui::SameLine();
    if (ImGui::Button("Spawn LEGACY smoke", ImVec2(170, 0)))
    {
        GSmokeSystem.SpawnLegacy(position, GLegacyDensity, GLegacySize, GSpawnDuration);
    }
    ImGui::EndDisabled();

    static int ringCount = 5;
    static float ringRadius = 6.0f;
    ImGui::SetNextItemWidth(90.0f);
    ImGui::SliderInt("##ringN", &ringCount, 2, 16);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90.0f);
    ImGui::SliderFloat("##ringR", &ringRadius, 1.0f, 30.0f, "%.0f m");
    ImGui::SameLine();
    ImGui::BeginDisabled(!havePosition);
    if (ImGui::Button("Spawn ring of fires"))
    {
        // N plumes on a circle around the spawn point, each with the current
        // params. A large fire is many sources: this is how you make a
        // building or a fuel dump burn rather than a single tall column.
        for (int i = 0; i < ringCount; ++i)
        {
            const float a = static_cast<float>(i) / static_cast<float>(ringCount) * 6.2831853f;
            Vector3 p = position + Vector3(std::cos(a) * ringRadius, 0.0f, std::sin(a) * ringRadius);
            p[1] = GSmokeWorldQuery().FloorHeight(p.X(), p.Z()) + 0.2f;
            GSmokeSystem.Spawn(p, GParams, GSpawnDuration);
        }
    }
    ImGui::EndDisabled();

    Vector3 here;
    const bool haveHere = SpawnPositionHere(here);
    ImGui::BeginDisabled(!haveHere);
    if (ImGui::Button("Spawn NEW smoke HERE (at camera)", ImVec2(260, 0)))
    {
        GSmokeSystem.Spawn(here, GParams, GSpawnDuration);
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Right where you are standing, on the floor of THIS storey. Walk into\n"
                          "a room and press it - the only reliable way to put smoke indoors.\n"
                          "Then step back to watch it.");
    }

    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("SmokeSourceVehicle - the 2001 engine's standalone emitter, the\n"
                          "one a burning wreck carries. It is the fairest available\n"
                          "comparison: there is no simpler legacy 'just a plume' entity.\n"
                          "It has no collision and bakes wind in at spawn, so changing\n"
                          "the wind will not move smoke it has already dropped.");
    }

    ImGui::SetNextItemWidth(110.0f);
    ImGui::SliderFloat("Legacy density", &GLegacyDensity, 0.1f, 3.0f, "%.2f");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110.0f);
    ImGui::SliderFloat("Legacy size", &GLegacySize, 0.2f, 6.0f, "%.2f");

    ImGui::Spacing();
    if (ImGui::Button("Extinguish all"))
    {
        GSmokeSystem.ExtinguishAll();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("emitters stop; existing particles disperse");

    if (havePosition)
    {
        ImGui::TextDisabled("Next spawn at  %.1f, %.1f, %.1f", position.X(), position.Y(), position.Z());
    }
}

void DrawEmitterList()
{
    GSmokeSystem.Prune();

    const std::vector<SmokeSystem::Entry>& entries = GSmokeSystem.Entries();

    ImGui::SeparatorText("Live emitters");

    if (entries.empty())
    {
        ImGui::TextDisabled("None spawned from this panel.");
        return;
    }

    if (ImGui::BeginTable("smokeEmitters", 4,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchProp))
    {
        ImGui::TableSetupColumn("id", ImGuiTableColumnFlags_WidthFixed, 36.0f);
        ImGui::TableSetupColumn("system");
        ImGui::TableSetupColumn("particles");
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 90.0f);
        ImGui::TableHeadersRow();

        for (const SmokeSystem::Entry& entry : entries)
        {
            Entity* entity = entry.entity;
            if (entity == nullptr)
            {
                continue;
            }

            ImGui::TableNextRow();
            ImGui::PushID(entry.id);

            ImGui::TableNextColumn();
            ImGui::Text("%d", entry.id);

            ImGui::TableNextColumn();
            if (entry.legacy)
            {
                ImGui::TextColored(ImVec4(0.9f, 0.7f, 0.4f, 1.0f), "legacy");
            }
            else
            {
                ImGui::TextColored(ImVec4(0.5f, 0.9f, 1.0f, 1.0f), "new");
            }

            ImGui::TableNextColumn();
            if (const SmokeVolume* volume = dynamic_cast<const SmokeVolume*>(entity))
            {
                ImGui::Text("%d%s", static_cast<int>(volume->ParticleCount()),
                            volume->IsEmitting() ? "" : "  (dispersing)");
            }
            else
            {
                // Legacy cloudlets are independent entities in the world list;
                // there is no way to attribute them back to their source.
                ImGui::TextDisabled("n/a");
            }

            ImGui::TableNextColumn();
            if (ImGui::SmallButton("Extinguish"))
            {
                GSmokeSystem.Extinguish(entry.id);
            }

            ImGui::PopID();
        }

        ImGui::EndTable();
    }
}

void DrawTuningSection()
{
    if (!ImGui::CollapsingHeader("Tuning (applies live to existing particles)"))
    {
        return;
    }

    ImGui::SeparatorText("Look");
    ImGui::SliderFloat("Thickness", &GParams.opacity, 0.02f, 1.0f, "%.3f");
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Peak alpha per particle. Overlap compounds, so a plume core\n"
                          "reads as solid well before this reaches 1.");
    }
    ImGui::SliderFloat("Start radius (m)", &GParams.startRadius, 0.1f, 4.0f, "%.2f");
    ImGui::SliderFloat("End radius (m)", &GParams.endRadius, 0.5f, 20.0f, "%.2f");
    ImGui::SliderFloat("Self-shadow", &GParams.selfShadowStrength, 0.0f, 4.0f, "%.2f");
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Each particle marches toward the sun through the plume and darkens by the smoke it passes. Dark core and underside, bright sun rim. 0 = flat sticker.");
    }
    ImGui::SliderInt("Self-shadow reach", &GParams.selfShadowSteps, 0, 10);
    ImGui::SliderFloat("Sky ambient", &GParams.skyAmbient, 0.0f, 1.5f, "%.2f");
    ImGui::SliderFloat("Ground shadow (this plume)", &GParams.groundShadow, 0.0f, 2.0f, "%.2f");
    {
        float g = GSmokeSystem.GroundShadowStrength();
        if (ImGui::SliderFloat("Ground shadow (global)", &g, 0.0f, 3.0f, "%.2f"))
        {
            GSmokeSystem.SetGroundShadowStrength(g);
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Smoke particles are splatted into the sun shadow map the terrain and objects already sample, so everything lit under a plume darkens. Texel is ~8 m: a soft ground shadow for a plume, not a crisp one for a puff.");
        }
    }
    ImGui::SliderFloat("Fade in", &GParams.fadeIn, 0.0f, 0.9f, "%.2f");
    ImGui::SliderFloat("Fade out", &GParams.fadeOut, 0.0f, 0.9f, "%.2f");

    ImGui::SeparatorText("Emission");
    ImGui::SliderFloat("Rate (/s)", &GParams.rate, 1.0f, 200.0f, "%.0f");
    ImGui::SliderFloat("Lifetime (s)", &GParams.particleLifetime, 1.0f, 180.0f, "%.0f",
                       ImGuiSliderFlags_Logarithmic);
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Up to 3 minutes. Log scale: fine control at the short end. Long lifetimes with a high rate need Max particles raised too, or the cap throttles emission.");
    }
    ImGui::SliderFloat("Nozzle speed (m/s)", &GParams.initialSpeed, 0.0f, 15.0f, "%.2f");
    ImGui::SliderFloat("Nozzle radius (m)", &GParams.emitterRadius, 0.0f, 4.0f, "%.2f");
    ImGui::SliderFloat("Spread", &GParams.spread, 0.0f, 1.5f, "%.2f");
    ImGui::SliderInt("Max particles", &GParams.maxParticles, 10, 4000);

    ImGui::SeparatorText("Motion");
    ImGui::SliderFloat("Buoyancy (m/s^2)", &GParams.buoyancy, -2.0f, 10.0f, "%.2f");
    ImGui::SliderFloat("Buoyancy decay", &GParams.buoyancyDecay, 0.02f, 1.0f, "%.2f");
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Fraction of lifetime over which the plume cools. Turn it up and\n"
                          "the plume rises forever; the flat drifting layer at the top of a\n"
                          "real plume is this term running out.");
    }
    ImGui::SliderFloat("Calm rise boost", &GParams.calmRiseBoost, 1.0f, 8.0f, "%.1f");
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("How much longer the plume stays buoyant in still air. Wind mixes and cools a plume; with no wind it climbs far higher. Set the Weather override to 0 m/s to see it.");
    }
    ImGui::SliderFloat("Drag (1/s)", &GParams.drag, 0.05f, 6.0f, "%.2f");
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("How hard a particle is pulled toward the air velocity around it.\n"
                          "This is what makes wind work: raise it and the plume snaps to the\n"
                          "wind, lower it and it coasts through gusts.");
    }
    ImGui::SliderFloat("Turbulence", &GParams.turbulence, 0.0f, 4.0f, "%.2f");
    ImGui::SliderFloat("Wind response", &GParams.windResponse, 0.0f, 2.0f, "%.2f");

    ImGui::SeparatorText("Fire");
    ImGui::Checkbox("Fire enabled", &GParams.fire);
    ImGui::SliderFloat("Flame rate (/s)", &GParams.fireRate, 5.0f, 300.0f, "%.0f");
    ImGui::SliderFloat("Flame lifetime (s)", &GParams.fireLifetime, 0.2f, 3.0f, "%.2f");
    ImGui::SliderFloat("Flame start radius", &GParams.fireStartRadius, 0.1f, 3.0f, "%.2f");
    ImGui::SliderFloat("Flame end radius", &GParams.fireEndRadius, 0.1f, 5.0f, "%.2f");
    ImGui::SliderFloat("Flame rise (m/s)", &GParams.fireRiseSpeed, 0.5f, 10.0f, "%.2f");
    ImGui::SliderFloat("Flame intensity", &GParams.fireIntensity, 0.2f, 5.0f, "%.2f");
    ImGui::SliderFloat("Smoke lift from fire", &GParams.fireSmokeLift, 0.0f, 8.0f, "%.2f");
    ImGui::SliderFloat("Smoke yield", &GParams.fireSmokeYield, 0.0f, 1.0f, "%.2f");
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Fraction of dying flames that become smoke. With fire on this is\n"
                          "the ONLY smoke source - the plume is what the fire produces.\n"
                          "1 = heavy oil fire, 0.2 = clean hot burn with wisps.");
    }
    ImGui::SliderFloat("Flame stretch", &GParams.fireStretch, 0.0f, 3.0f, "%.2f");
    ImGui::SliderFloat("Column height (m)", &GParams.fireColumnHeight, 0.0f, 25.0f, "%.1f");
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("0 = independent puffs. Above 0, flames fill a column of this height as one body: born throughout it, pulled toward the axis, alive for the whole rise. Pair with a longer flame lifetime.");
    }
    ImGui::SliderFloat("Column cohesion", &GParams.fireColumnCohesion, 0.0f, 10.0f, "%.1f");
    ImGui::SliderFloat("Flame wind lean", &GParams.fireWindLean, 0.0f, 1.0f, "%.2f");
    ImGui::SliderFloat("Fire light intensity", &GParams.fireLightIntensity, 0.0f, 4.0f, "%.2f");
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("A warm point light at the flame centroid, flickering with the fire.\n"
                          "Point lights here cast no shadow maps, so 'flickering shadows' is\n"
                          "what the light does to nearby surfaces. Best judged at night.");
    }
    ImGui::SliderFloat("Fire light radius (m)", &GParams.fireLightRadius, 2.0f, 60.0f, "%.0f");
    ImGui::SliderFloat("Fire light HDR scale", &GParams.fireLightHdrScale, 0.5f, 20.0f, "%.1f");
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Radiance multiplier so the glow reads in DAYLIGHT. The renderer is\n"
                          "HDR; at 1.0 the fire's light vanished at noon. Raise for day, the\n"
                          "night exposure keeps it from blowing out.");
    }
    ImGui::SliderInt("Max flames", &GParams.maxFlames, 10, 500);

    ImGui::SeparatorText("Collision and shelter");
    ImGui::Checkbox("Collide with world", &GParams.collide);
    ImGui::SameLine();
    ImGui::Checkbox("Hard contain", &GParams.contain);
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Sweep every tick and refuse to let a particle cross a surface it came from behind. Smoke stays in the room and finds the door. Costs 7 rays per particle per frame.");
    }
    ImGui::SliderFloat("Restitution", &GParams.restitution, 0.0f, 1.0f, "%.2f");
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("0 slides along the surface, 1 is elastic. Smoke is not a ball -\n"
                          "above about 0.3 it stops reading as smoke.");
    }
    ImGui::SliderFloat("Surface friction", &GParams.friction, 0.0f, 1.0f, "%.2f");
    ImGui::SliderFloat("Collision radius x", &GParams.collisionRadiusScale, 0.05f, 1.0f, "%.2f");
    ImGui::SliderFloat("Sweep interval (s)", &GParams.sweepInterval, 0.02f, 0.5f, "%.3f");
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Seconds between real world sweeps per particle. Between sweeps the\n"
                          "cached contact plane still blocks the particle, so raising this\n"
                          "trades exactness for cost - it does not switch collision off.\n"
                          "Watch the sweeps/frame counter below as you move it.");
    }
    ImGui::SliderFloat("Sheltered wind x", &GParams.shelteredWindScale, 0.0f, 1.0f, "%.2f");
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("What the prevailing wind is multiplied by when a particle has a\n"
                          "roof over it. 0 is dead-still indoor smoke; a little residual\n"
                          "keeps a room from looking airless.");
    }
    ImGui::SliderFloat("Roof probe (m)", &GParams.shelterProbeHeight, 2.0f, 40.0f, "%.0f");

    ImGui::Spacing();
    if (ImGui::Button("Reset to defaults"))
    {
        GParams = SmokeParams{};
        ApplySmokeColorPreset(GParams, static_cast<SmokeColorPreset>(GColorPreset));
    }

    // Push the working set onto every live new-system plume. Unconditionally,
    // every frame the header is open: cheap (a struct copy per plume) and it
    // means a slider drag is visible in the air immediately rather than only in
    // the next thing you spawn. Particles keep the lifetime they were born
    // with; everything else is read fresh on their next tick.
    for (const SmokeSystem::Entry& entry : GSmokeSystem.Entries())
    {
        if (SmokeVolume* volume = dynamic_cast<SmokeVolume*>(static_cast<Entity*>(entry.entity)))
        {
            volume->SetParams(GParams);
        }
    }
}

void DrawAirflowSection()
{
    if (!ImGui::CollapsingHeader("Local airflow (rotor downwash)"))
    {
        return;
    }

    bool enabled = GAirflow.LocalEnabled();
    if (ImGui::Checkbox("Local airflow enabled", &enabled))
    {
        GAirflow.SetLocalEnabled(enabled);
    }

    float scale = GAirflow.LocalScale();
    if (ImGui::SliderFloat("Strength x", &scale, 0.0f, 5.0f, "%.2f"))
    {
        GAirflow.SetLocalScale(scale);
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Exaggerating this is the quickest way to answer 'is the downwash\n"
                          "reaching the smoke at all', which is hard to see at 1x from the\n"
                          "ground.");
    }

    const std::size_t count = GAirflow.RotorCount();
    if (count == 0)
    {
        ImGui::TextDisabled("No powered rotors near the camera.");
        ImGui::TextDisabled("The field tracks helicopters by ROTOR SPEED, not by being");
        ImGui::TextDisabled("airborne, so a spooling-up aircraft on the ground counts.");
        return;
    }

    for (std::size_t i = 0; i < count; ++i)
    {
        const RotorWash& wash = GAirflow.Rotor(i);
        ImGui::BulletText("%.0f, %.0f, %.0f   disc %.1f m   %.1f m/s   %.0f m up", wash.position.X(),
                          wash.position.Y(), wash.position.Z(), wash.discRadius, wash.strength,
                          wash.position.Y() - wash.groundY);
    }
}

void DrawBenchmarkSection()
{
    ImGui::SeparatorText("Cost");

    const SmokeStats stats = GSmokeSystem.Sample();
    PushHistory(static_cast<float>(stats.simulateMicroseconds), static_cast<float>(stats.drawMicroseconds));

    const float simAverage = Average(GSimHistory);
    const float drawAverage = Average(GDrawHistory);

    if (ImGui::BeginTable("smokeCost", 2, ImGuiTableFlags_SizingFixedFit))
    {
        const auto row = [](const char* label, const char* format, auto... values)
        {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", label);
            ImGui::TableNextColumn();
            ImGui::Text(format, values...);
        };

        row("volumes", "%d", stats.volumes);
        row("particles (new)", "%d", stats.particles);
        row("cloudlets (legacy)", "%d", stats.legacyCloudlets);
        row("simulate", "%.1f us   (avg %.1f)", stats.simulateMicroseconds, simAverage);
        row("draw", "%.1f us   (avg %.1f)", stats.drawMicroseconds, drawAverage);
        row("world sweeps / frame", "%lld", stats.sweeps);

        if (stats.particles > 0)
        {
            row("per particle", "%.3f us sim + %.3f us draw",
                simAverage / static_cast<float>(stats.particles),
                drawAverage / static_cast<float>(stats.particles));
        }

        ImGui::EndTable();
    }

    // Reset the sweep counter so next frame's figure is per-frame rather than
    // a running total. Done here, after Sample, because this is the only place
    // that reads it.
    GSmokeSystem.BeginFrame();

    ImGui::Spacing();
    ImGui::TextDisabled("simulate/draw are wall-clock inside SmokeVolume only. They");
    ImGui::TextDisabled("exclude the legacy cloudlets entirely - the legacy system has");
    ImGui::TextDisabled("no equivalent instrumentation, so compare the two by frame time");
    ImGui::TextDisabled("in the Perf tab with one system up at a time.");

    ImGui::Spacing();
    ImGui::SeparatorText("Stress");

    static int stressCount = 8;
    static bool stressLegacy = false;
    ImGui::SetNextItemWidth(120.0f);
    ImGui::SliderInt("Plumes", &stressCount, 1, 64);
    ImGui::SameLine();
    ImGui::Checkbox("legacy", &stressLegacy);

    if (ImGui::Button("Spawn grid"))
    {
        Vector3 centre;
        if (SpawnPosition(centre))
        {
            // A square-ish grid on 12 m centres, so the plumes are close enough
            // to overlap in the frame (which is where alpha cost actually
            // shows) but not so close that they read as one.
            const int side = static_cast<int>(std::ceil(std::sqrt(static_cast<float>(stressCount))));
            for (int i = 0; i < stressCount; ++i)
            {
                const float x = static_cast<float>(i % side - side / 2) * 12.0f;
                const float z = static_cast<float>(i / side - side / 2) * 12.0f;
                Vector3 position = centre + Vector3(x, 0.0f, z);
                position[1] = GSmokeWorldQuery().FloorHeight(position.X(), position.Z()) + 0.2f;

                if (stressLegacy)
                {
                    GSmokeSystem.SpawnLegacy(position, GLegacyDensity, GLegacySize, GSpawnDuration);
                }
                else
                {
                    GSmokeSystem.Spawn(position, GParams, GSpawnDuration);
                }
            }
        }
    }
    ImGui::SameLine();
    ImGui::TextDisabled("then read the Perf tab's frame time");
}

void DrawWindSummary()
{
    // A one-line reminder of what the air is doing, so the smoke tab does not
    // require flipping to the Weather tab to interpret what you are watching.
    if (!GWind.IsActive())
    {
        ImGui::TextDisabled("Wind authority inactive.");
        return;
    }

    const WindSample& wind = GWind.Sample();
    const float bearing = std::fmod(450.0f - wind.directionRad * (180.0f / 3.14159265f), 360.0f);
    ImGui::TextDisabled("Wind %.1f m/s toward %.0f deg%s   -   set it in the Weather tab", wind.speed, bearing,
                        GWind.IsOverridden() ? "  [OVERRIDDEN]" : "");
}

} // namespace

void DrawSmokeTab()
{
    DrawWindSummary();
    ImGui::Spacing();
    DrawSpawnSection();
    ImGui::Spacing();
    DrawEmitterList();
    ImGui::Spacing();
    DrawTuningSection();
    DrawAirflowSection();
    ImGui::Spacing();
    DrawBenchmarkSection();
}

} // namespace Poseidon::Dev
