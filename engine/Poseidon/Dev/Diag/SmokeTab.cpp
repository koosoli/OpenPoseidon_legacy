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

#include <Poseidon/Dev/Debug/DevPanelWidgets.hpp>
#include <Poseidon/Dev/Diag/SmokeTab.hpp>
#include <Poseidon/Graphics/Rendering/Effects/Smokes.hpp> // SMK-020 GLegacySmokeScale
#include <Poseidon/Graphics/Rendering/Effects/SmokeVolumetric.hpp> // SMK-037 soft particles
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

/// Shared indoor guard: is the camera under a roof (room query first, then the
/// cheap shelter probe for buildings with no Paths data)? Indoors the floor
/// drop below must NOT run: FloorHeightBelow finds the nearest surface UNDER
/// the point, and in a building whose interior carries no roadway LOD that is
/// the terrain beneath the floor slab -- the plume was born in the crawl space
/// and never appeared in the room. The camera height is already correct; it is
/// standing in the room.
bool CameraIndoors(const ISmokeWorldQuery& query)
{
    if (GScene == nullptr || GScene->GetCamera() == nullptr)
    {
        return false;
    }
    const Vector3 cam = GScene->GetCamera()->Position();
    int room = -1;
    if (query.IndoorRoomAt(cam, room))
    {
        return true;
    }
    // No room data (or outdoors by it): a roof overhead still means the floor
    // drop risks the crawl space, so treat "sheltered" as indoors too.
    return query.IsSheltered(cam, 6.0f);
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

    const ISmokeWorldQuery& query = GSmokeWorldQuery();
    const Camera& camera = *GScene->GetCamera();
    const Vector3 forward = camera.Direction();
    Vector3 position = camera.Position() + forward * GSpawnDistance;
    if (!CameraIndoors(query))
    {
        // Outdoors only: drop to the floor of the storey the CAMERA is on, not
        // the topmost surface. Indoors the topmost surface is the roof, and
        // clamping to it put every plume on top of the building -- which is why
        // spawning indoors "almost always" landed outside.
        Vector3 probe = position;
        probe[1] = camera.Position().Y();
        position[1] = query.FloorHeightBelow(probe) + 0.2f;
    }
    else
    {
        // Indoors trust the camera height, nudged to just above knee height so
        // the plume visibly rises through the room rather than at eye level.
        position[1] = camera.Position().Y() - 0.8f;
    }
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
    const ISmokeWorldQuery& query = GSmokeWorldQuery();
    const Camera& camera = *GScene->GetCamera();
    Vector3 position = camera.Position();
    if (CameraIndoors(query))
    {
        position[1] -= 0.8f;
    }
    else
    {
        position[1] = query.FloorHeightBelow(position) + 0.2f;
    }
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
    Dev::SliderFloat("Distance ahead (m)", &GSpawnDistance, 2.0f, 60.0f, "%.0f");

    ImGui::SetNextItemWidth(140.0f);
    ImGui::InputFloat("Emit duration (s)", &GSpawnDuration, 0.0f, 0.0f, "%.0f");
    Dev::PanelSameLine();
    ImGui::TextDisabled("(negative = forever)");

    // -- colour -----------------------------------------------------------
    const char* presetNames[] = {"White", "Black", "Red", "Green", "Blue", "Yellow", "Purple", "Orange"};
    ImGui::SetNextItemWidth(140.0f);
    if (Dev::Combo("Colour", &GColorPreset, presetNames, IM_ARRAYSIZE(presetNames)))
    {
        ApplySmokeColorPreset(GParams, static_cast<SmokeColorPreset>(GColorPreset));
    }
    Dev::PanelSameLine();
    ImGui::ColorButton("##smokeTint", ImVec4(GParams.red, GParams.green, GParams.blue, 1.0f),
                       ImGuiColorEditFlags_NoTooltip, ImVec2(20, 20));
    Dev::PanelSameLine();
    float tint[3] = {GParams.red, GParams.green, GParams.blue};
    ImGui::SetNextItemWidth(180.0f);
    if (Dev::ColorEdit3("Custom tint", tint, ImGuiColorEditFlags_NoInputs))
    {
        GParams.red = tint[0];
        GParams.green = tint[1];
        GParams.blue = tint[2];
    }

    ImGui::Spacing();

    Vector3 position;
    const bool havePosition = SpawnPosition(position);

    Dev::Checkbox("Fire at the base", &GParams.fire);
    if (Dev::PanelItemHovered())
    {
        ImGui::SetTooltip("Adds a flame layer at the emitter: short-lived additive sprites on\n"
                          "a white-yellow-orange-red cooling ramp, and extra lift on the smoke\n"
                          "above them. Pair with the Black colour preset for a burning wreck.");
    }
    Dev::PanelSameLine();
    if (Dev::Button("Burning wreck"))
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
        GParams.endRadius = 14.0f;
        GParams.particleLifetime = 95.0f;
        GParams.maxParticles = 900;
        GParams.diffusion = 1.3f;
        GParams.densityFalloff = 0.9f;
        GParams.buoyancyDecay = 0.5f;
        GParams.calmRiseBoost = 4.0f;
    }
    Dev::PanelSameLine();
    if (Dev::Button("Campfire"))
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
    Dev::PanelSameLine();
    if (Dev::Button("Balloons"))
    {
        GParams = SmokeParams{};
        GParams.balloons = true;
        GParams.fire = false;
        GParams.rate = 9.0f;
        GParams.particleLifetime = 60.0f;
        GParams.maxParticles = 500;
        GParams.emitterRadius = 1.4f;
        GParams.initialSpeed = 0.6f;
        GParams.opacity = 1.0f;
        GParams.fadeIn = 0.02f;
        GParams.fadeOut = 0.06f;
        GParams.drag = 0.5f; // they follow the wind, gently
        GParams.turbulence = 0.0f;
        GParams.diffusion = 0.0f;
        GParams.densityFalloff = 0.0f;
        GParams.restitution = 0.55f; // and they bounce off walls
        GParams.collisionRadiusScale = 1.0f;
        GParams.groundShadow = 0.0f;
        GParams.selfShadowStrength = 0.0f;
    }
    if (Dev::PanelItemHovered())
    {
        ImGui::SetTooltip(
            "Coloured balloons instead of smoke. Also the clearest test rig there is: they are countable, they hold "
            "their size, they bounce, and their colour makes the lighting and the wind obvious.");
    }
    Dev::PanelSameLine();
    if (Dev::Button("INFERNO"))
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
        GParams.endRadius = 26.0f;
        GParams.particleLifetime = 150.0f;
        GParams.maxParticles = 2000;
        GParams.diffusion = 1.6f;
        GParams.densityFalloff = 0.95f;
        GParams.buoyancyDecay = 0.6f;
        GParams.calmRiseBoost = 5.0f;
        GParams.fireLightRadius = 22.0f;
        GParams.fireLightIntensity = 1.8f;
        // A COLUMN, not a swarm of puffs.
        GParams.fireColumnHeight = 10.0f;
        GParams.fireColumnCohesion = 3.5f;
        GParams.fireLifetime = 2.2f;
        GParams.fireRiseSpeed = 5.0f;
    }
    if (Dev::PanelItemHovered())
    {
        ImGui::SetTooltip("Wide, hot, roaring. Also try 'Spawn ring of fires' below to cover an\n"
                          "area - a burning building is many fires, not one big one.");
    }

    ImGui::BeginDisabled(!havePosition);
    if (Dev::Button("Spawn NEW smoke", ImVec2(160, 0)))
    {
        GSmokeSystem.Spawn(position, GParams, GSpawnDuration);
    }
    Dev::PanelSameLine();
    if (Dev::Button("Spawn LEGACY smoke", ImVec2(170, 0)))
    {
        GSmokeSystem.SpawnLegacy(position, GLegacyDensity, GLegacySize, GSpawnDuration);
    }
    ImGui::EndDisabled();

    static int ringCount = 5;
    static float ringRadius = 6.0f;
    ImGui::SetNextItemWidth(90.0f);
    Dev::SliderInt("##ringN", &ringCount, 2, 16);
    Dev::PanelSameLine();
    ImGui::SetNextItemWidth(90.0f);
    Dev::SliderFloat("##ringR", &ringRadius, 1.0f, 30.0f, "%.0f m");
    Dev::PanelSameLine();
    ImGui::BeginDisabled(!havePosition);
    if (Dev::Button("Spawn ring of fires"))
    {
        // N plumes on a circle around the spawn point, each with the current
        // params. A large fire is many sources: this is how you make a
        // building or a fuel dump burn rather than a single tall column.
        // Indoors the per-point FloorHeight would land half the ring on the
        // roof and half in the crawl space; keep the ring on the centre's
        // floor instead.
        const bool ringIndoors = CameraIndoors(GSmokeWorldQuery());
        for (int i = 0; i < ringCount; ++i)
        {
            const float a = static_cast<float>(i) / static_cast<float>(ringCount) * 6.2831853f;
            Vector3 p = position + Vector3(std::cos(a) * ringRadius, 0.0f, std::sin(a) * ringRadius);
            if (ringIndoors)
            {
                p[1] = position[1];
            }
            else
            {
                p[1] = GSmokeWorldQuery().FloorHeight(p.X(), p.Z()) + 0.2f;
            }
            GSmokeSystem.Spawn(p, GParams, GSpawnDuration);
        }
    }
    ImGui::EndDisabled();

    Vector3 here;
    const bool haveHere = SpawnPositionHere(here);
    ImGui::BeginDisabled(!haveHere);
    if (Dev::Button("Spawn NEW smoke HERE (at camera)", ImVec2(260, 0)))
    {
        GSmokeSystem.Spawn(here, GParams, GSpawnDuration);
    }
    ImGui::EndDisabled();
    if (Dev::PanelItemHovered())
    {
        ImGui::SetTooltip("Right where you are standing, on the floor of THIS storey. Walk into\n"
                          "a room and press it - the only reliable way to put smoke indoors.\n"
                          "Then step back to watch it.");
    }

    if (Dev::PanelItemHovered())
    {
        ImGui::SetTooltip("SmokeSourceVehicle - the 2001 engine's standalone emitter, the\n"
                          "one a burning wreck carries. It is the fairest available\n"
                          "comparison: there is no simpler legacy 'just a plume' entity.\n"
                          "It has no collision and bakes wind in at spawn, so changing\n"
                          "the wind will not move smoke it has already dropped.");
    }

    // SMK-038: WHICH SMOKE SYSTEM draws. The head of the tab, because everything below it
    // is a lever on one system or the other.
    {
        SmokeVolumetricParams& vol = GSmokeVolumetric();
        int mode = static_cast<int>(vol.mode);
        const char* modes[] = {"Legacy billboards (2001)", "Volumetric field", "Both (A/B, double-counts)"};
        ImGui::SetNextItemWidth(240.0f);
        if (Dev::Combo("Smoke system (SMK-038)", &mode, modes, IM_ARRAYSIZE(modes)))
        {
            vol.mode = static_cast<SmokeSystemMode>(mode);
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("POSEIDON_SMOKE_SYSTEM=0/1/2. Legacy is the 2001 billboard system every\n"
                              "grenade, wreck and shell burst has always used, with the SMK-034/035/036/037\n"
                              "levers below. Volumetric scatters the same particles into a camera-anchored\n"
                              "128x64x128 froxel grid and raymarches it: real depth intersection, a real\n"
                              "interior, and a self-shadow marched toward the sun instead of guessed from a\n"
                              "particle's age. In Volumetric the injected billboards STOP drawing, or the\n"
                              "smoke would be rendered twice; 'Both' keeps them on purpose so the two can be\n"
                              "seen in one frame, and is an A/B view, not a setting to play with.\n"
                              "REACH IS THE TRADE: the grid is cell size x 128/64/128 metres around the\n"
                              "camera and smoke outside it is not drawn at all in Volumetric.");
        if (vol.mode != SmokeSystemMode::Legacy)
        {
            ImGui::SetNextItemWidth(110.0f);
            Dev::SliderFloat("Froxel cell m", &vol.cellSize, 0.25f, 4.0f, "%.2f");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("POSEIDON_SMOKE_VOL_CELL. Metres per grid cell. The grid is a fixed\n"
                                  "128 x 64 x 128 CELLS, so this alone sets both the detail and the reach:\n"
                                  "1 m gives a 128 m box with metre-scale detail, 2 m gives 256 m and half\n"
                                  "the detail. There is no free lunch here -- the memory is fixed.");
            Dev::PanelSameLine();
            ImGui::SetNextItemWidth(110.0f);
            Dev::SliderInt("March steps", &vol.marchSteps, 8, 128);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("POSEIDON_SMOKE_VOL_STEPS. Samples along each view ray. This is the single\n"
                                  "biggest cost term; the integration is analytic per step, so lowering it\n"
                                  "loses detail rather than changing the plume's overall opacity.");
            ImGui::SetNextItemWidth(110.0f);
            Dev::SliderInt("Sun march steps", &vol.sunSteps, 0, 12);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("POSEIDON_SMOKE_VOL_SUN_STEPS. The SECOND march, from each sample toward\n"
                                  "the sun, which is the self-shadow. 0 makes the plume uniformly lit and is\n"
                                  "the cheapest ablation for measuring what it costs. Steps widen\n"
                                  "geometrically, so 4 already reaches ~11 m at the default step length.");
            Dev::PanelSameLine();
            ImGui::SetNextItemWidth(110.0f);
            Dev::SliderFloat("Self-shadow", &vol.selfShadow, 0.0f, 1.0f, "%.2f");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("POSEIDON_SMOKE_VOL_SELFSHADOW. Blends between a flat plume and the full\n"
                                  "sun march. Unlike the step count this costs nothing to change -- the march\n"
                                  "still runs -- so it is the LOOK knob and the step count is the COST knob.");
            ImGui::SetNextItemWidth(110.0f);
            Dev::SliderFloat("Density x", &vol.density, 0.0f, 8.0f, "%.2f");
            Dev::PanelSameLine();
            ImGui::SetNextItemWidth(110.0f);
            Dev::SliderFloat("Extinction x", &vol.extinction, 0.0f, 8.0f, "%.2f");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("POSEIDON_SMOKE_VOL_DENSITY scales what is written INTO the field;\n"
                                  "POSEIDON_SMOKE_VOL_EXTINCTION scales how much the march is absorbed by it.\n"
                                  "They look similar and are not: density also feeds the sun march, so raising\n"
                                  "it darkens the plume's core, while extinction alone just makes it opaquer.");
            ImGui::SetNextItemWidth(110.0f);
            Dev::SliderFloat("Albedo", &vol.albedo, 0.0f, 1.0f, "%.2f");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("POSEIDON_SMOKE_VOL_ALBEDO. Single-scattering albedo: how much of what the\n"
                                  "medium takes out of a ray it scatters rather than absorbs. Sooty diesel\n"
                                  "smoke is low and dark; steam and white smoke grenades are near 1.");
            Dev::PanelSameLine();
            ImGui::SetNextItemWidth(110.0f);
            Dev::SliderFloat("Forward scatter g", &vol.anisotropy, -0.9f, 0.9f, "%.2f");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("POSEIDON_SMOKE_VOL_G. Henyey-Greenstein asymmetry. Positive throws light\n"
                                  "forward, which is what makes a backlit plume glow at its rim; 0 is\n"
                                  "isotropic and reads flat.");
            ImGui::SetNextItemWidth(110.0f);
            Dev::SliderInt("March scale", &vol.scale, 1, 4);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("POSEIDON_SMOKE_VOL_SCALE. 1 marches at render resolution, 2 at half and\n"
                                  "upsamples with a nearest-DEPTH pick (not bilinear -- bilinear bleeds smoke\n"
                                  "that marched past a foreground silhouette back over it). Half resolution is\n"
                                  "the standard cost control and quarters the march work.");
            ImGui::TextDisabled("particles injected %lld   frames marched %lld", vol.blobs, vol.framesMarched);
            if (vol.framesMarched > 0 && vol.blobs == 0)
            {
                ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f),
                                   "Marching, but NO particles reached the field - throw some smoke.");
            }
        }
        ImGui::Separator();
    }
    // SMK-037: how the game's smoke MEETS the world. Renderer state, not simulation, so it
    // sits at the head of the legacy block rather than among the containment queries.
    {
        SmokeSoftParticles& soft = GSmokeSoftParticles();
        Dev::Checkbox("Soft particles: smoke fades into geometry (SMK-037)", &soft.enabled);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("POSEIDON_SMOKE_SOFT=0 turns it off; ON is the shipped default.\n"
                              "Every cloudlet is a flat screen-space quad depth-tested against the world, so\n"
                              "where its plane crosses the ground, a wall or a soldier the test kills one half\n"
                              "and keeps the other and the boundary is a HARD LINE across the sprite. A puff\n"
                              "resting on the ground is cut by a razor edge along the terrain. This fades the\n"
                              "sprite's alpha by how close it is to whatever is behind it, so the billboard\n"
                              "meets geometry as a soft intersection -- the single most visible reason the\n"
                              "smoke reads as a decal glued in front of the world rather than a body of gas\n"
                              "standing in it.\n"
                              "Costs one full-screen scene-depth copy per frame, recorded ONLY while this is\n"
                              "on: the transparent pass has the depth buffer attached writable and so cannot\n"
                              "also sample it.");
        ImGui::SetNextItemWidth(110.0f);
        Dev::SliderFloat("Contact fade m", &soft.fade, 0.05f, 4.0f, "%.2f");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("POSEIDON_SMOKE_SOFT_FADE. Metres of separation at which the sprite reaches full\n"
                              "opacity. 0.5 m is the measured default, and a puff RADIUS (1.5 m) is the wrong\n"
                              "instinct: looking down a lawn at a shallow angle puts the ground a metre or two\n"
                              "behind nearly every puff along the view ray, so 1.5 m guts the whole plume.\n"
                              "0.05 m reproduces the old hard-edged image almost exactly, which is the setting\n"
                              "to compare against.");
        ImGui::TextDisabled("soft decal batches %lld", soft.batches);
    }
    // SMK-030: containment for the game's own smoke.
    {
        LegacySmokeContainment& contain = GLegacySmokeContainment();
        Dev::Checkbox("Game smoke stays in its room (SMK-030)", &contain.enabled);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("POSEIDON_SMOKE_CONTAIN_LEGACY=0 turns it off. Every cloudlet asks ONCE, at its\n"
                              "first step, whether it started inside a room; outdoor smoke never asks again.\n"
                              "An indoor cloudlet is kept inside by the room walk (no geometry sweep) and\n"
                              "follows open doorways. Measured: +0.03 ms of cloudlet simulation with 730\n"
                              "indoor particles.");
        ImGui::SetNextItemWidth(110.0f);
        Dev::SliderFloat("Wall slide", &contain.slide, 0.0f, 1.0f, "%.2f");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("POSEIDON_SMOKE_CONTAIN_SLIDE. How much speed a blocked particle keeps ALONG the\n"
                              "wall: 0 stops it dead, higher values let it curl and spread over the surface.");
        Dev::Checkbox("Outdoor smoke cannot drift into a house (SMK-031, costly)", &contain.blockOutsideIn);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("OFF by default; POSEIDON_SMOKE_CONTAIN_OUTSIDE_IN=1 turns it on. Measured at\n"
                              "21-24 ms of cloudlet simulation in a village against 0.25 ms for the rest of the\n"
                              "containment: the room query behind it runs two full Landscape::IsInside passes\n"
                              "per outdoor sample, because BuildingContaining caches only hits. The cheap gates\n"
                              "(cell holds a solid object, then once every quarter second) do not help in a\n"
                              "village, where every cell does.");
        Dev::Checkbox("Clamp the drawn puff to the room (SMK-032)", &contain.clampDraw);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("POSEIDON_SMOKE_CONTAIN_DRAW=0 turns it off. A billboard beside a wall paints\n"
                              "through it whatever the particle centre does. Indoors the sprite is clamped to\n"
                              "the room clearance plus a 10 cm soft edge, as SmokeVolume::Draw already does.");
        ImGui::TextDisabled("asked %lld  indoors %lld  steps %lld  blocked %lld", contain.spawnQueries,
                            contain.indoor, contain.stepQueries, contain.blocked);
        Dev::Checkbox("No wind indoors (SMK-033)", &contain.windOutdoorsOnly);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("POSEIDON_SMOKE_WIND_INDOORS=1 restores the old behaviour. Legacy smoke bakes the\n"
                              "wind into a particle's speed at the moment it is dropped and never consults the\n"
                              "wind again, so a plume lit INSIDE a house was blown across the room by the\n"
                              "weather outside -- and with containment holding it, piled against the wall.\n"
                              "Costs one geography read per spawn; the room query only near a solid object.");
        ImGui::TextDisabled("outside checks %lld  kept out %lld  draw queries %lld  clamped %lld",
                            contain.outsideChecks, contain.blockedIn, contain.drawQueries, contain.drawClamped);
        ImGui::TextDisabled("wind queries %lld  wind blocked %lld", contain.windQueries, contain.windBlocked);
    }
    // SMK-020: the game's own smoke (grenades, shells, craters) -- scaled at class load.
    {
        LegacySmokeScale& scale = GLegacySmokeScale();
        // SMK-034: the look multipliers. Applied when a cloudlet class is LOADED, so a change
        // here reaches shots at the next mission start, like the lifetime scale below.
        Dev::Checkbox("Denser, softer game smoke (SMK-034)", &scale.look);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("POSEIDON_SMOKE_LOOK=0 restores the authored CfgCloudlets values exactly.");
        ImGui::SetNextItemWidth(110.0f);
        Dev::SliderFloat("Particles x", &scale.density, 0.25f, 6.0f, "%.2f");
        Dev::PanelSameLine();
        ImGui::SetNextItemWidth(110.0f);
        Dev::SliderFloat("Puff size x", &scale.size, 0.25f, 3.0f, "%.2f");
        ImGui::SetNextItemWidth(110.0f);
        Dev::SliderFloat("Puff alpha x", &scale.alpha, 0.1f, 2.0f, "%.2f");
        Dev::PanelSameLine();
        ImGui::SetNextItemWidth(110.0f);
        Dev::SliderFloat("Fade/grow x", &scale.grow, 0.25f, 4.0f, "%.2f");
        ImGui::SetNextItemWidth(110.0f);
        Dev::SliderFloat("Game smoke lifetime x", &scale.lifetime, 0.1f, 10.0f, "%.2f");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("POSEIDON_SMOKE_LIFETIME_SCALE. Multiplies every CfgCloudlets particle life and\n"
                              "source emission time. Applies to cloudlet classes loaded from now on:\n"
                              "for shots that is the next mission start.");
        Dev::PanelSameLine();
        ImGui::SetNextItemWidth(110.0f);
        Dev::SliderFloat("Game smoke rise x", &scale.rise, 0.1f, 5.0f, "%.2f");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("POSEIDON_SMOKE_RISE_SCALE. Multiplies the cloudlet vertical speed; wind drift\n"
                              "already scales with the longer life.");
    }
    // SMK-035: lighting and self-shadowing for the game's own smoke. Live -- it is applied
    // per particle per step, so a change here shows on smoke already in the air.
    {
        LegacySmokeShading& shading = GLegacySmokeShading();
        Dev::Checkbox("Lit, self-shadowing game smoke (SMK-035)", &shading.enabled);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("POSEIDON_SMOKE_SHADE=0 restores the flat 2001 colour. Cloudlets are lit by\n"
                              "nothing at all: the colour is the authored cloudletColor times a temperature\n"
                              "ramp, identical for every particle in a plume, at every hour of the day. This\n"
                              "shades each particle from the scene's own sun and darkens it by how deep in\n"
                              "the column it sits, which is what gives a plume a light side, a dark side and\n"
                              "an interior. Hot particles (fireballs, muzzle flashes) are emissive and are\n"
                              "left exactly as authored.");
        ImGui::SetNextItemWidth(110.0f);
        Dev::SliderFloat("Self-shadow", &shading.selfShadow, 0.0f, 1.0f, "%.2f");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("POSEIDON_SMOKE_SHADE_SELFSHADOW. How dark the inside of a plume gets. 0 is a\n"
                              "uniformly lit plume; high values give a near-black core with bright fringes,\n"
                              "which is what a dense sooty column actually looks like.");
        Dev::PanelSameLine();
        ImGui::SetNextItemWidth(110.0f);
        Dev::SliderFloat("Translucency", &shading.wrap, 0.0f, 4.0f, "%.2f");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("POSEIDON_SMOKE_SHADE_WRAP. How far light wraps around the puff. 0 is a hard\n"
                              "surface lighting term that makes the away side black; higher is lit through,\n"
                              "and flatter.");
        ImGui::SetNextItemWidth(110.0f);
        Dev::SliderFloat("Sun x", &shading.diffuse, 0.0f, 3.0f, "%.2f");
        Dev::PanelSameLine();
        ImGui::SetNextItemWidth(110.0f);
        Dev::SliderFloat("Ambient x", &shading.ambient, 0.0f, 3.0f, "%.2f");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("POSEIDON_SMOKE_SHADE_DIFFUSE / _AMBIENT scale the scene's own two light terms.\n"
                              "Both at 1.0 means smoke is lit exactly like the rest of the world, so it goes\n"
                              "warm at dusk and dark at night instead of staying white.");
        ImGui::SetNextItemWidth(110.0f);
        Dev::SliderFloat("Emissive above K", &shading.emissiveT, 0.0f, 3000.0f, "%.0f");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("POSEIDON_SMOKE_SHADE_EMISSIVE_T. Particles hotter than this make their own\n"
                              "light and are not shaded at all; the shading fades in over the 400 K below it.\n"
                              "CfgCloudlets tracks a real temperature per particle, so this separates a\n"
                              "fireball from the smoke it leaves behind.");
        Dev::Checkbox("Game smoke shadows the ground (SMK-036)", &shading.groundShadow);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("POSEIDON_SMOKE_GROUND_SHADOW_LEGACY=0 turns it off. The renderer already had\n"
                              "this pass and the dev SmokeVolume fed it, which is why dev smoke darkened the\n"
                              "grass under it and a thrown smoke grenade did not: the game's own cloudlets\n"
                              "were never handed to it. They are now. The global strength slider for the\n"
                              "pass is the one this tab already exposes for the modern smoke.");
        Dev::PanelSameLine();
        ImGui::SetNextItemWidth(110.0f);
        Dev::SliderFloat("Ground x", &shading.groundWeight, 0.0f, 4.0f, "%.2f");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("POSEIDON_SMOKE_GROUND_SHADOW_LEGACY_WEIGHT. Multiplies the particle's own\n"
                              "drawn opacity to get the shadow it casts.");
        ImGui::TextDisabled("shaded %lld  left emissive %lld  ground blobs %lld", shading.shaded, shading.emissive,
                            shading.blobs);
    }
    // SMK-039: how a plume dies. Live -- these are applied per particle per step, so a
    // change here reaches smoke already in the air.
    {
        LegacySmokeDisperse& disperse = GLegacySmokeDisperse();
        Dev::Checkbox("Plumes disperse instead of winking out (SMK-039)", &disperse.enabled);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("POSEIDON_SMOKE_DISPERSE=0 restores the authored behaviour. CfgCloudlets grows a\n"
                              "puff to full size and then HOLDS it there for the rest of its life, fading it out\n"
                              "on a timer -- so a plume ends as a clump of still-solid blobs that wink out. Real\n"
                              "smoke keeps entraining air: the puffs keep growing, keep drifting apart, and the\n"
                              "same smoke spread through more air is thinner. The point of all three sliders is\n"
                              "that a particle should be INVISIBLE before it is deleted.");
        ImGui::SetNextItemWidth(110.0f);
        Dev::SliderFloat("Expand to x", &disperse.expand, 1.0f, 6.0f, "%.2f");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("POSEIDON_SMOKE_DISPERSE_EXPAND. How much bigger a puff is at death than at the\n"
                              "end of its grow-up. 1.0 = the authored behaviour, hold size and wink out.");
        Dev::PanelSameLine();
        ImGui::SetNextItemWidth(110.0f);
        Dev::SliderFloat("Thin with size", &disperse.thin, 0.0f, 2.0f, "%.2f");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("POSEIDON_SMOKE_DISPERSE_THIN. The density the expansion implies: a puff k times\n"
                              "wider holds the same smoke over k^2 more area, so opacity falls as k^-2 at 1.0.\n"
                              "0 swells the puff without thinning it, which looks like an inflating balloon.");
        ImGui::SetNextItemWidth(110.0f);
        Dev::SliderFloat("Drift apart", &disperse.spread, 0.0f, 5.0f, "%.2f");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("POSEIDON_SMOKE_DISPERSE_SPREAD. Outward acceleration away from the plume's axis,\n"
                              "ramped with the particle's age, so a column opens out as it rises instead of\n"
                              "staying the width it was born. Horizontal only -- the vertical is what the\n"
                              "cloudlet class's own climb rate is for.");
    }
    ImGui::SetNextItemWidth(110.0f);
    Dev::SliderFloat("Legacy density", &GLegacyDensity, 0.1f, 3.0f, "%.2f");
    Dev::PanelSameLine();
    ImGui::SetNextItemWidth(110.0f);
    Dev::SliderFloat("Legacy size", &GLegacySize, 0.2f, 6.0f, "%.2f");

    ImGui::Spacing();
    if (Dev::Button("Extinguish all"))
    {
        GSmokeSystem.ExtinguishAll();
    }
    Dev::PanelSameLine();
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
            if (Dev::SmallButton("Extinguish"))
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
    Dev::SliderFloat("Thickness", &GParams.opacity, 0.02f, 1.0f, "%.3f");
    // These two tooltips used to sit BOTH after "Thins as it grows", so the
    // second silently won: Thickness had no tooltip at all, and the falloff
    // slider explained the wrong control. Each now follows its own.
    if (Dev::PanelItemHovered())
    {
        ImGui::SetTooltip("Peak alpha per particle, and 1.0 is the physical maximum a single billboard has.\n"
                          "Thickness BEYOND that comes from overlap, not from here: more particles (Rate x\n"
                          "Lifetime, under Max particles) and bigger ones (End radius). A plume core reads\n"
                          "as solid well before this reaches 1.");
    }
    Dev::SliderFloat("Thins as it grows", &GParams.densityFalloff, 0.0f, 2.5f, "%.2f");
    if (Dev::PanelItemHovered())
    {
        ImGui::SetTooltip("Alpha scales by (start radius / current radius) to this power: the same soot spread over a "
                          "bigger puff. 0 = every puff keeps its birth opacity (a solid grey shape). ~0.85 = dense at "
                          "the source, faint cloud overhead.");
    }
    Dev::SliderFloat("Start radius (m)", &GParams.startRadius, 0.1f, 4.0f, "%.2f");
    Dev::SliderFloat("End radius (m)", &GParams.endRadius, 0.5f, 60.0f, "%.2f");
    Dev::SliderFloat("Self-shadow", &GParams.selfShadowStrength, 0.0f, 4.0f, "%.2f");
    if (Dev::PanelItemHovered())
    {
        ImGui::SetTooltip("Each particle marches toward the sun through the plume and darkens by the smoke it passes. "
                          "Dark core and underside, bright sun rim. 0 = flat sticker.");
    }
    Dev::SliderInt("Self-shadow reach", &GParams.selfShadowSteps, 0, 10);
    Dev::SliderFloat("Sky ambient", &GParams.skyAmbient, 0.0f, 1.5f, "%.2f");
    Dev::SliderFloat("Ground shadow (this plume)", &GParams.groundShadow, 0.0f, 2.0f, "%.2f");
    {
        float g = GSmokeSystem.GroundShadowStrength();
        if (Dev::SliderFloat("Ground shadow (global)", &g, 0.0f, 3.0f, "%.2f"))
        {
            GSmokeSystem.SetGroundShadowStrength(g);
        }
        if (Dev::PanelItemHovered())
        {
            ImGui::SetTooltip("Smoke particles are splatted into the sun shadow map the terrain and objects already "
                              "sample, so everything lit under a plume darkens. Texel is ~8 m: a soft ground shadow "
                              "for a plume, not a crisp one for a puff.");
        }
    }
    Dev::SliderFloat("Fade in", &GParams.fadeIn, 0.0f, 0.9f, "%.2f");
    Dev::SliderFloat("Fade out", &GParams.fadeOut, 0.0f, 0.9f, "%.2f");

    ImGui::SeparatorText("Emission");
    Dev::SliderFloat("Rate (/s)", &GParams.rate, 1.0f, 4000.0f, "%.0f", ImGuiSliderFlags_Logarithmic);
    Dev::SliderFloat("Lifetime (s)", &GParams.particleLifetime, 1.0f, 180.0f, "%.0f", ImGuiSliderFlags_Logarithmic);
    if (Dev::PanelItemHovered())
    {
        ImGui::SetTooltip("Up to 3 minutes. Log scale: fine control at the short end. Long lifetimes with a high rate "
                          "need Max particles raised too, or the cap throttles emission.");
    }
    Dev::SliderFloat("Nozzle speed (m/s)", &GParams.initialSpeed, 0.0f, 15.0f, "%.2f");
    Dev::SliderFloat("Nozzle radius (m)", &GParams.emitterRadius, 0.0f, 4.0f, "%.2f");
    Dev::SliderFloat("Spread", &GParams.spread, 0.0f, 1.5f, "%.2f");
    Dev::SliderInt("Max particles", &GParams.maxParticles, 10, 200000, "%d", ImGuiSliderFlags_Logarithmic);
    {
        // THE CEILING NOBODY COULD SEE. A plume holds about rate x lifetime
        // particles at steady state and emission simply STOPS at the cap, so the
        // Rate slider can be dragged to its top and change nothing, with no
        // indication of why. The stock defaults were already in that state:
        // 35/s x 14 s wants 490 against a cap of 400.
        //
        // Say the number rather than leaving it to be rediscovered. Same reason
        // the Grass tab now prints its mid-ring reach.
        const float wanted = GParams.rate * GParams.particleLifetime;
        const float cap = static_cast<float>(GParams.maxParticles);
        if (wanted > cap * 1.02f)
        {
            ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.35f, 1.0f),
                               "Cap is throttling: %.0f/s x %.0f s wants ~%.0f particles against a cap of %d. "
                               "Rate above ~%.0f/s does nothing until you raise the cap.",
                               GParams.rate, GParams.particleLifetime, wanted, GParams.maxParticles,
                               cap / std::max(GParams.particleLifetime, 0.01f));
        }
        else
        {
            ImGui::TextDisabled("~%.0f particles at steady state (%.0f/s x %.0f s) against a cap of %d.", wanted,
                                GParams.rate, GParams.particleLifetime, GParams.maxParticles);
        }
        ImGui::TextDisabled("Thickness comes from OVERLAP, so once Thickness is at 1 this and End radius ARE the "
                            "density controls. Each particle is one soft billboard and consecutive ones merge "
                            "into a single 2D batch, so the cost of a huge plume is fill rate, not draw calls.");
    }

    ImGui::SeparatorText("Motion");
    Dev::SliderFloat("Buoyancy (m/s^2)", &GParams.buoyancy, -2.0f, 10.0f, "%.2f");
    Dev::SliderFloat("Buoyancy decay", &GParams.buoyancyDecay, 0.02f, 1.0f, "%.2f");
    if (Dev::PanelItemHovered())
    {
        ImGui::SetTooltip("Fraction of lifetime over which the plume cools. Turn it up and\n"
                          "the plume rises forever; the flat drifting layer at the top of a\n"
                          "real plume is this term running out.");
    }
    Dev::SliderFloat("Calm rise boost", &GParams.calmRiseBoost, 1.0f, 8.0f, "%.1f");
    if (Dev::PanelItemHovered())
    {
        ImGui::SetTooltip("How much longer the plume stays buoyant in still air. Wind mixes and cools a plume; with no "
                          "wind it climbs far higher. Set the Weather override to 0 m/s to see it.");
    }
    Dev::SliderFloat("Drag (1/s)", &GParams.drag, 0.05f, 6.0f, "%.2f");
    if (Dev::PanelItemHovered())
    {
        ImGui::SetTooltip("How hard a particle is pulled toward the air velocity around it.\n"
                          "This is what makes wind work: raise it and the plume snaps to the\n"
                          "wind, lower it and it coasts through gusts.");
    }
    Dev::SliderFloat("Turbulence", &GParams.turbulence, 0.0f, 4.0f, "%.2f");
    Dev::SliderFloat("Diffusion", &GParams.diffusion, 0.0f, 5.0f, "%.2f");
    if (Dev::PanelItemHovered())
    {
        ImGui::SetTooltip("How fast the plume comes APART. A random walk growing as sqrt(age), so young smoke holds "
                          "the column and old smoke wanders into a cloud. 0 keeps the plume rigid forever.");
    }
    Dev::SliderFloat("Wind response", &GParams.windResponse, 0.0f, 2.0f, "%.2f");

    ImGui::SeparatorText("Balloons");
    Dev::Checkbox("Balloons instead of smoke", &GParams.balloons);
    if (Dev::PanelItemHovered())
    {
        ImGui::SetTooltip("Overrides the smoke terms rather than tuning them: no growth, no diffusion, no thinning, no "
                          "self-shadow. Set Thickness to 1.0 so they read as solid.");
    }
    Dev::SliderFloat("Balloon rise (m/s)", &GParams.balloonRise, 0.0f, 8.0f, "%.2f");
    Dev::SliderFloat("Balloon sway", &GParams.balloonSway, 0.0f, 3.0f, "%.2f");
    Dev::SliderFloat("Balloon size (m)", &GParams.balloonSize, 0.05f, 2.0f, "%.2f");
    Dev::SliderFloat("Hue spread", &GParams.balloonHueSpread, 0.0f, 1.0f, "%.2f");
    Dev::SliderFloat("Balloon opacity", &GParams.balloonOpacity, 0.3f, 1.0f, "%.2f");
    if (Dev::PanelItemHovered())
    {
        ImGui::SetTooltip("Balloons have their own alpha, independent of the smoke Thickness slider:\n"
                          "they are opaque objects, not gas.");
    }
    Dev::SliderFloat("Balloon specular", &GParams.balloonSpecular, 0.0f, 2.0f, "%.2f");
    Dev::SliderFloat("Balloon shininess", &GParams.balloonShininess, 4.0f, 96.0f, "%.0f");
    Dev::SliderFloat("Balloon limb darken", &GParams.balloonLimbDarken, 0.0f, 0.9f, "%.2f");
    if (Dev::PanelItemHovered())
    {
        ImGui::SetTooltip("How much the silhouette edge darkens. This is what makes the disc read\n"
                          "as a sphere instead of a flat sticker.");
    }
    Dev::SliderFloat("Balloon rim light", &GParams.balloonRimLight, 0.0f, 1.0f, "%.2f");

    ImGui::SeparatorText("Disturbance");
    Dev::Checkbox("Movers disturb smoke", &GParams.disturb);
    if (Dev::PanelItemHovered())
    {
        ImGui::SetTooltip("The player (or whatever the camera rides) and the free-fly camera push\n"
                          "particles aside as they move through a plume. Walk through spawned\n"
                          "smoke to see it part; fly the free camera through for the clearest view.");
    }
    Dev::SliderFloat("Disturb strength", &GParams.disturbStrength, 0.0f, 40.0f, "%.1f");
    Dev::SliderFloat("Disturb reach (m)", &GParams.disturbReach, 0.0f, 6.0f, "%.2f");
    Dev::SliderFloat("Disturb min speed (m/s)", &GParams.disturbMinSpeed, 0.0f, 5.0f, "%.2f");

    ImGui::SeparatorText("Fire");
    Dev::Checkbox("Fire enabled", &GParams.fire);
    Dev::SliderFloat("Flame rate (/s)", &GParams.fireRate, 5.0f, 300.0f, "%.0f");
    Dev::SliderFloat("Flame lifetime (s)", &GParams.fireLifetime, 0.2f, 3.0f, "%.2f");
    Dev::SliderFloat("Flame start radius", &GParams.fireStartRadius, 0.1f, 3.0f, "%.2f");
    Dev::SliderFloat("Flame end radius", &GParams.fireEndRadius, 0.1f, 5.0f, "%.2f");
    Dev::SliderFloat("Flame rise (m/s)", &GParams.fireRiseSpeed, 0.5f, 10.0f, "%.2f");
    Dev::SliderFloat("Flame intensity", &GParams.fireIntensity, 0.2f, 5.0f, "%.2f");
    Dev::SliderFloat("Smoke lift from fire", &GParams.fireSmokeLift, 0.0f, 8.0f, "%.2f");
    Dev::SliderFloat("Smoke yield", &GParams.fireSmokeYield, 0.0f, 1.0f, "%.2f");
    if (Dev::PanelItemHovered())
    {
        ImGui::SetTooltip("Fraction of dying flames that become smoke. With fire on this is\n"
                          "the ONLY smoke source - the plume is what the fire produces.\n"
                          "1 = heavy oil fire, 0.2 = clean hot burn with wisps.");
    }
    Dev::SliderFloat("Flame stretch", &GParams.fireStretch, 0.0f, 3.0f, "%.2f");
    Dev::SliderFloat("Column height (m)", &GParams.fireColumnHeight, 0.0f, 25.0f, "%.1f");
    if (Dev::PanelItemHovered())
    {
        ImGui::SetTooltip(
            "0 = independent puffs. Above 0, flames fill a column of this height as one body: born throughout it, "
            "pulled toward the axis, alive for the whole rise. Pair with a longer flame lifetime.");
    }
    Dev::SliderFloat("Column cohesion", &GParams.fireColumnCohesion, 0.0f, 10.0f, "%.1f");
    Dev::SliderFloat("Flame wind lean", &GParams.fireWindLean, 0.0f, 1.0f, "%.2f");
    Dev::SliderFloat("Fire light intensity", &GParams.fireLightIntensity, 0.0f, 4.0f, "%.2f");
    if (Dev::PanelItemHovered())
    {
        ImGui::SetTooltip("A warm point light at the flame centroid, flickering with the fire.\n"
                          "Point lights here cast no shadow maps, so 'flickering shadows' is\n"
                          "what the light does to nearby surfaces. Best judged at night.");
    }
    Dev::SliderFloat("Fire light radius (m)", &GParams.fireLightRadius, 2.0f, 60.0f, "%.0f");
    Dev::SliderFloat("Fire light HDR scale", &GParams.fireLightHdrScale, 0.5f, 20.0f, "%.1f");
    if (Dev::PanelItemHovered())
    {
        ImGui::SetTooltip("Radiance multiplier so the glow reads in DAYLIGHT. The renderer is\n"
                          "HDR; at 1.0 the fire's light vanished at noon. Raise for day, the\n"
                          "night exposure keeps it from blowing out.");
    }
    Dev::SliderInt("Max flames", &GParams.maxFlames, 10, 500);

    ImGui::SeparatorText("Collision and shelter");
    Dev::Checkbox("Collide with world", &GParams.collide);
    Dev::PanelSameLine();
    Dev::Checkbox("Hard contain", &GParams.contain);
    if (Dev::PanelItemHovered())
    {
        ImGui::SetTooltip("Sweep every tick and refuse to let a particle cross a surface it came from behind. Smoke "
                          "stays in the room and finds the door. Costs 7 rays per particle per frame.");
    }
    Dev::SliderFloat("Restitution", &GParams.restitution, 0.0f, 1.0f, "%.2f");
    if (Dev::PanelItemHovered())
    {
        ImGui::SetTooltip("0 slides along the surface, 1 is elastic. Smoke is not a ball -\n"
                          "above about 0.3 it stops reading as smoke.");
    }
    Dev::SliderFloat("Surface friction", &GParams.friction, 0.0f, 1.0f, "%.2f");
    Dev::SliderFloat("Collision radius x", &GParams.collisionRadiusScale, 0.05f, 1.0f, "%.2f");
    Dev::SliderFloat("Sweep interval (s)", &GParams.sweepInterval, 0.02f, 0.5f, "%.3f");
    if (Dev::PanelItemHovered())
    {
        ImGui::SetTooltip("Seconds between real world sweeps per particle. Between sweeps the\n"
                          "cached contact plane still blocks the particle, so raising this\n"
                          "trades exactness for cost - it does not switch collision off.\n"
                          "Watch the sweeps/frame counter below as you move it.");
    }
    Dev::SliderFloat("Sheltered wind x", &GParams.shelteredWindScale, 0.0f, 1.0f, "%.2f");
    if (Dev::PanelItemHovered())
    {
        ImGui::SetTooltip("What the prevailing wind is multiplied by when a particle has a\n"
                          "roof over it. 0 is dead-still indoor smoke; a little residual\n"
                          "keeps a room from looking airless.");
    }
    Dev::SliderFloat("Roof probe (m)", &GParams.shelterProbeHeight, 2.0f, 40.0f, "%.0f");

    ImGui::Spacing();
    if (Dev::Button("Reset to defaults"))
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
    if (Dev::Checkbox("Local airflow enabled", &enabled))
    {
        GAirflow.SetLocalEnabled(enabled);
    }

    float scale = GAirflow.LocalScale();
    if (Dev::SliderFloat("Strength x", &scale, 0.0f, 5.0f, "%.2f"))
    {
        GAirflow.SetLocalScale(scale);
    }
    if (Dev::PanelItemHovered())
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
        ImGui::BulletText("%.0f, %.0f, %.0f   disc %.1f m   %.1f m/s   %.0f m up", wash.position.X(), wash.position.Y(),
                          wash.position.Z(), wash.discRadius, wash.strength, wash.position.Y() - wash.groundY);
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
            row("per particle", "%.3f us sim + %.3f us draw", simAverage / static_cast<float>(stats.particles),
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
    Dev::SliderInt("Plumes", &stressCount, 1, 64);
    Dev::PanelSameLine();
    Dev::Checkbox("legacy", &stressLegacy);

    if (Dev::Button("Spawn grid"))
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
    Dev::PanelSameLine();
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
