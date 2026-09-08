// AIR-010: the dev-panel "Flight" tab -- the altitude falloff, live.
//
// The tab exists because the change it fronts is ON BY DEFAULT. The stock curve
// is one checkbox away, and the two aircraft's scale heights are sliders, so a
// claim about the new ceiling can be checked against the old one inside a single
// session rather than across two builds.
//
// The preview table is the part worth having. A slider on a scale height is not
// self-explanatory -- nobody knows what 4600 m of e-folding height feels like --
// so the table shows the resulting coefficient at a ladder of altitudes, and the
// ceiling estimate underneath turns that back into the number the owner actually
// asked about.

#include <Poseidon/Dev/Diag/FlightTab.hpp>

#include <Poseidon/World/Entities/Vehicles/Air/FlightCeiling.hpp>
#include <Poseidon/World/Scene/Camera/AerialRange.hpp>

// See the note in PictureModeTab.cpp: Foundation's DebugLog macro eats an ImGui
// member of the same name.
#undef DebugLog

#include <imgui.h>

#include <Poseidon/Dev/Debug/DevPanelWidgets.hpp>

#include <cmath>

namespace Poseidon::Dev
{

namespace
{

/// The coefficient at which each aircraft stops climbing, MEASURED under the
/// stock curve (FlightCeiling.cpp records the runs). It is a property of the
/// airframe and the rest of the flight model, so it does not move when the
/// falloff curve does -- which is exactly what makes a ceiling estimate possible
/// from the curve parameters alone.
///
/// An A-10 and a UH-60. Other airframes differ, so the estimate below is a guide
/// for these two rather than a promise for every aircraft in the game.
constexpr float kPlaneCriticalCoef = 0.35f;
constexpr float kHeliCriticalCoef = 0.36f;

/// Where the curve currently crosses the critical coefficient. This is an
/// ESTIMATE and the tooltip says so: it assumes the aircraft has the patience to
/// walk the asymptote, and a real climb gives up short of it once the climb rate
/// falls below what the pilot -- or the AI -- will sit through.
float EstimateCeiling(const Air::FlightCeilingSettings& s, bool plane)
{
    const float full = plane ? s.planeFullForceAlt : s.heliFullForceAlt;
    const float critical = plane ? kPlaneCriticalCoef : kHeliCriticalCoef;
    if (s.modernModel)
    {
        const float scale = plane ? s.planeScaleHeight : s.heliScaleHeight;
        return full + scale * std::log(1.0f / critical);
    }
    const float noForce = plane ? 13000.0f : 3000.0f;
    return full + (1.0f - critical) * (noForce - full);
}

void DrawAircraft(const char* name, bool plane, float* fullAlt, float* scaleHeight)
{
    Air::FlightCeilingSettings& s = Air::FlightCeiling();

    ImGui::SeparatorText(name);

    Dev::SliderFloat(plane ? "Full-force altitude##plane" : "Full-force altitude##heli", fullAlt, 0.0f, 8000.0f,
                     "%.0f m");
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Below this there is no penalty at all. Stock OFP used 5000 m for\n"
                          "planes and 1000 m for helicopters, and those were never the\n"
                          "problem -- the decay above them was.");
    }

    Dev::SliderFloat(plane ? "Scale height##plane" : "Scale height##heli", scaleHeight, 200.0f, 30000.0f, "%.0f m");
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("e-folding height: available force falls to ~37%% of sea level over\n"
                          "this distance above the full-force altitude. Larger = higher ceiling\n"
                          "and a slower, longer climb through the top of it.");
    }

    ImGui::Text("estimated ceiling %.0f m", (double)EstimateCeiling(s, plane));
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Where the curve crosses the coefficient at which this airframe\n"
                          "stopped climbing under the stock model (%.2f).\n\n"
                          "An UPPER BOUND, not a promise: climb rate collapses as the\n"
                          "coefficient falls, so a real climb gives up somewhat short of it.\n"
                          "Fly it with POSEIDON_FLIGHT_LOG=1 to get the measured number.",
                          (double)(plane ? kPlaneCriticalCoef : kHeliCriticalCoef));
    }

    if (ImGui::BeginTable(plane ? "planecoef" : "helicoef", 2,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_SizingFixedFit))
    {
        ImGui::TableSetupColumn("altitude");
        ImGui::TableSetupColumn("force coef");
        ImGui::TableHeadersRow();
        const float ladder[] = {1000.0f, 3000.0f, 5000.0f, 8000.0f, 12000.0f, 16000.0f, 20000.0f};
        for (const float alt : ladder)
        {
            const float coef = plane ? Air::PlaneAirCoef(alt) : Air::HeliAirCoef(alt);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Text("%.0f m", (double)alt);
            ImGui::TableNextColumn();
            // Red once the coefficient has gone negative -- which only the stock
            // curve can do, and which is the defect the modern one removes.
            if (coef < 0.0f)
            {
                ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "%.3f  INVERTED", (double)coef);
            }
            else
            {
                ImGui::Text("%.3f", (double)coef);
            }
        }
        ImGui::EndTable();
    }
}

} // namespace

void DrawFlightTab()
{
    Air::FlightCeilingSettings& s = Air::FlightCeiling();

    Dev::PanelHelp("Aircraft have no altitude clamp anywhere in this engine. The ceiling is\n"
                   "entirely aerodynamic: one coefficient scales thrust, lift, control authority\n"
                   "and drag, and stock OFP ramped it LINEARLY to zero -- 13 km for a plane,\n"
                   "3 km for a helicopter -- with no clamp, so past those it went NEGATIVE and\n"
                   "inverted every force it touched.\n\n"
                   "The default here is an exponential falloff with a positive floor: same idea,\n"
                   "no wall, no sign flip. Uncheck below to fly the stock curve for comparison.");

    Dev::Checkbox("Modern density falloff (default on)", &s.modernModel);
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Off reproduces the stock OFP linear ramp EXACTLY, sign flip included.\n"
                          "It is left unfixed on purpose so an A/B against it is honest.");
    }

    Dev::Checkbox("Log altitude records to the log file", &s.logSamples);
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("One line per new 25 m record, per aircraft class. The LAST line before\n"
                          "the record stops advancing is the measured ceiling.\n"
                          "Same as POSEIDON_FLIGHT_LOG=1.");
    }

    Dev::SliderFloat("Minimum coefficient", &s.minCoef, 0.0f, 0.25f, "%.3f");
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Floor on available force. This is the thing that stops thrust, lift\n"
                          "and drag inverting above the ceiling. Zero is safe; negative is not,\n"
                          "and the slider cannot go there.");
    }

    DrawAircraft("Fixed wing", true, &s.planeFullForceAlt, &s.planeScaleHeight);
    DrawAircraft("Rotary wing", false, &s.heliFullForceAlt, &s.heliScaleHeight);

    ImGui::SeparatorText("Helicopter collective cap (not aerodynamic)");
    Dev::PanelHelp("A SECOND helicopter ceiling, and for a cadet-mode player it is the one that\n"
                   "bites first. It caps the height the keyboard collective can ask for, in metres\n"
                   "ABOVE GROUND -- stock OFP used 150 m in cadet mode, which stops a player a long\n"
                   "way below anything the air is doing. Default here is the aerodynamic ceiling,\n"
                   "so cadet mode runs out of air rather than out of permission.");
    Dev::SliderFloat("Cadet-mode cap", &s.heliHelperMaxHeightEasy, 100.0f, 12000.0f, "%.0f m AGL");
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Set to 150 to restore the stock cadet guard rail.");
    }
    Dev::SliderFloat("Expert-mode cap", &s.heliHelperMaxHeight, 100.0f, 20000.0f, "%.0f m AGL");
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Stock value 10000, which never bound because the air ran out first.\n"
                          "Keep it above the rotary-wing ceiling above or it starts to.");
    }

    // FAR-001. This lives on the flight tab rather than in a tab of its own
    // because it is the other half of the same defect: the sliders above decide
    // how high an aircraft can climb, and these decide whether there is a world
    // underneath it when it gets there.
    Aerial::AerialRangeSettings& a = Aerial::Settings();

    ImGui::SeparatorText("Ground visibility from altitude (far plane)");
    Dev::PanelHelp("The camera far plane USED TO BE the view distance -- one number answered both\n"
                   "'what may be drawn' and 'what has faded into haze'. Ground 5 km below an\n"
                   "aircraft is 5 km away, so at the default 900 m view distance the world stopped\n"
                   "being drawn from about 900 m of altitude upwards: at 5,000 m, cloud and sky and\n"
                   "no ground at all.\n\n"
                   "They are separate numbers now, and both open with altitude ABOVE GROUND on a\n"
                   "law that does not touch the view-distance slider. Below the start altitude\n"
                   "nothing here runs, so ground play is bit-identical. Uncheck to restore the old\n"
                   "single-number behaviour.");

    Dev::Checkbox("Altitude-scaled far plane (default on)", &a.enabled);
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Off restores the pre-FAR-001 engine exactly: far plane == fog range ==\n"
                          "the view-distance-derived number, at every altitude.\n"
                          "Same as POSEIDON_AERIAL_RANGE=0.");
    }

    Dev::Checkbox("Coarse terrain in the far ring (default on)", &a.coarseTerrain);
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Terrain beyond the ordinary view distance is generated at a reduced LOD.\n"
                          "Off draws the whole opened-up rectangle at full detail -- which is the\n"
                          "A/B that prices the coarse tier, and is expensive.\n"
                          "Same as POSEIDON_AERIAL_COARSE_TERRAIN=0.");
    }

    Dev::SliderInt("Far-ring LOD", &a.coarseLod, 0, 2);
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("0 = full detail (equivalent to unchecking the box above), 2 = coarsest.\n"
                          "On Everon's 50 m land grid, LOD 2 is one quad per land cell.");
    }

    Dev::SliderFloat("Reach per metre of altitude", &a.reachFactor, 1.0f, 6.0f, "%.1f x");
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Slant reach as a multiple of height above ground. 3 means an eye at\n"
                          "5 km sees 15 km, which is about the whole of Everon (12.8 km across).\n"
                          "Costs roughly with the square of this. Same as POSEIDON_AERIAL_REACH.");
    }

    Dev::SliderFloat("Start altitude", &a.startAlt, 0.0f, 3000.0f, "%.0f m AGL");
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Below this the policy is completely inert and the engine runs exactly\n"
                          "the code it ran before. Raise it to push the change further away from\n"
                          "helicopter altitudes.");
    }

    Dev::SliderFloat("Full-strength altitude", &a.fullAlt, 100.0f, 6000.0f, "%.0f m AGL");
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("The reach ramps in smoothly between the start altitude and this one.\n"
                          "Set them close together and you will see the haze step as you climb.");
    }

    Dev::SliderFloat("Maximum reach", &a.maxReach, 2000.0f, 40000.0f, "%.0f m");
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Hard ceiling on the far plane, and so on the terrain rectangle and the\n"
                          "water surface. This is the number that bounds the cost at 14 km.");
    }

    Dev::SliderFloat("Water reach cap (0 = follow)", &a.waterReach, 0.0f, 24000.0f, "%.0f m");
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("How far the WATER surface follows the opened far plane.\n\n"
                          "The water IS where the whole cost lives -- at 5,000 m the GPU frame went\n"
                          "6.2 -> 11.2 ms, of which terrain was +0.23 and water +5.2 -- but this\n"
                          "slider does NOT buy it back. Measured at 8,000 m: no cap 5.44 ms,\n"
                          "12 km 5.54, 6 km 4.96. That is run spread. The cost is FILL, not area:\n"
                          "the sea covers the same screen however far the rectangle goes.\n\n"
                          "Kept as a diagnostic handle on the water from outside the water\n"
                          "renderer. Same as POSEIDON_AERIAL_WATER_REACH.");
    }

    Dev::SliderFloat("Maximum reach (GL33)", &a.finiteFarReach, 2000.0f, 20000.0f, "%.0f m");
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("GL33 keeps a FINITE forward-Z projection, where depth precision is a\n"
                          "far/near ratio -- so the far plane costs precision there and z-fights if\n"
                          "opened as far as wgpu's. wgpu's projection is infinite-far reversed-Z\n"
                          "and is insensitive to the far plane, so it uses the slider above.");
    }
}

} // namespace Poseidon::Dev
