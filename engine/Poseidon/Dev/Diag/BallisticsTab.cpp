// The dev-panel "Ballistics" tab — a pure view over BallisticsRecorder's
// store and DiagPause's flag.  It holds no state of its own beyond ImGui's,
// which is what lets the tab be closed with genuinely zero residual cost.

#include <Poseidon/Foundation/Framework/Log.hpp>
#include <Poseidon/World/Entities/Weapons/Ballistics.hpp>
#include <Poseidon/Foundation/platform.hpp>

// The PCH pulls in Logging.hpp, which #defines DebugLog() as a macro and
// collides with the ImGui::DebugLog() method.  Same dance as DebugOverlay.cpp.
#ifdef DebugLog
#undef DebugLog
#endif

#include <imgui.h>

#include <Poseidon/Dev/Diag/BallisticsTab.hpp>
#include <Poseidon/Dev/Diag/BallisticsRecorder.hpp>
#include <Poseidon/Dev/Diag/DiagPause.hpp>
#include <Poseidon/Core/Application.hpp>
#include <Poseidon/Core/Global.hpp>
#include <Poseidon/World/World.hpp>
#include <Poseidon/World/Weather/WindModel.hpp>

#include <cmath>
#include <cstdio>

namespace Poseidon::Dev
{

namespace
{

void DrawPauseSection()
{
    ImGui::SeparatorText("Simulation");

    const bool available = DiagPauseAvailable();
    bool paused = DiagPauseActive();

    ImGui::BeginDisabled(!available);
    if (ImGui::Checkbox("Pause simulation (Ctrl+P)", &paused))
    {
        SetDiagPause(paused);
    }
    ImGui::EndDisabled();

    if (!available)
    {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), "unavailable");
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("No world loaded, or this is a network game.\n"
                              "World::IsSimulationEnabled takes a different branch in\n"
                              "GModeNetware and never reads the local pause, so pausing\n"
                              "here would do nothing (and must not: it would desync).");
        }
    }

    if (DiagPauseActive())
    {
        ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.2f, 1.0f), "PAUSED  -  mission clock held at t = %.2f s",
                           Glob.time.toFloat());
        ImGui::TextDisabled("Free-fly camera still flies. Everything else - entities, AI,");
        ImGui::TextDisabled("projectiles, sun, weather, clouds, sea - is frozen.");
    }
    else
    {
        ImGui::TextDisabled("Mission clock t = %.2f s", Glob.time.toFloat());
    }

    ImGui::TextDisabled("Free-fly: Zeus tab -> Enable free camera, then fly with the");
    ImGui::TextDisabled("movement keys; pause and fly in either order.");
}

void DrawWindSection()
{
    ImGui::SeparatorText("Wind");

    const bool ballisticsWind = WindModel::BallisticsEnabled();
    if (ballisticsWind)
    {
        ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "Ballistics wind: ON");
    }
    else
    {
        ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "Ballistics wind: OFF (OFP fidelity default)");
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("POSEIDON_WIND_BALLISTICS / WindModel::SetBallisticsEnabled.\n"
                          "With it off, ShotShell::Simulate computes exactly the\n"
                          "windless expression it always did, and the 'lateral' column\n"
                          "below reads ~0 for every shot. That is the check: if lateral\n"
                          "is non-zero here, wind IS acting on the round.");
    }

    const WindSample& wind = GWind.Sample();
    ImGui::Text("Field: %.2f m/s toward %.0f deg  (mean %.2f m/s, gust %+.0f%%)", wind.speed,
                wind.directionRad * (180.0f / 3.14159265f), wind.meanSpeed, wind.gustFraction * 100.0f);
    if (!GWind.IsActive())
    {
        ImGui::TextDisabled("(wind model not ticked yet - no world?)");
    }
}

void DrawModelSection()
{
    ImGui::SeparatorText("Ballistic model");

    // The A/B the owner asked for. Both levers are live: they change the next solve and the next
    // round fired, with no restart. Rounds already in the air keep what they were fired with,
    // which is why a trail drawn before a toggle still shows the old behaviour -- that is useful
    // for comparison, not a glitch.
    bool dragAim = ::Poseidon::Ballistics::DragCorrectAimEnabled();
    if (ImGui::Checkbox("Drag-correct aiming (new)", &dragAim))
    {
        ::Poseidon::Ballistics::SetDragCorrectAim(dragAim);
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip(
                          "ON  - the AI and the player's sights solve the REAL trajectory,\n"
                          "      integrating the same drag the bullet flies under.\n"
                          "OFF - LEGACY: the drag-free vacuum parabola the engine shipped\n"
                          "      with, fall = 0.5*g*(d/v0)^2. It assumes the round still\n"
                          "      carries muzzle velocity all the way out, so sights and AI\n"
                          "      under-elevate, and the error grows with range.\n"
                          "\n"
                          "Covers the player's sight zeroing, infantry AI, and the six\n"
                          "vehicle AI sites (tank, helicopter, plane, car).\n"
                          "Also settable with POSEIDON_BALLISTIC_AIM=0.");
    }
    ImGui::SameLine();
    if (dragAim)
    {
        ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "NEW");
    }
    else
    {
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.2f, 1.0f), "LEGACY");
    }

    // Forced drag function. Without this the Mach-dependent path cannot be reached at all: no
    // shipped ammo declares dragModel, so there would be nothing to compare against.
    static int forced = 0; // 0 Constant, 1 G1, 2 G7
    static float forcedBc = 0.304f;
    const char* items[] = {"Constant (authored)", "Force G1", "Force G7"};

    ImGui::SetNextItemWidth(200.0f);
    bool changed = ImGui::Combo("Drag function", &forced, items, 3);
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip(
                          "Constant - each round uses its own config: airFriction, or a\n"
                          "           dragModel if it declares one. Nothing shipped does yet.\n"
                          "Force G1/G7 - override EVERY round onto that standard drag\n"
                          "           function with the coefficient below, so the\n"
                          "           Mach-dependent model can be seen and felt before\n"
                          "           per-cartridge coefficients exist.\n"
                          "\n"
                          "One coefficient for every round is wrong on purpose. This is a\n"
                          "comparison tool, not a setting.");
    }
    if (forced != 0)
    {
        ImGui::SetNextItemWidth(200.0f);
        changed |= ImGui::SliderFloat("BC (lb/in2)", &forcedBc, 0.02f, 1.00f, "%.3f");
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip(
                          "G1 reference points: 9mm 0.147, 7.62x39 0.339, 5.56 M855 0.304,\n"
                          "7.62 M80 0.393, 7.62 M118 0.494, .50 BMG M33 0.670.\n"
                          "G7 runs roughly half the G1 number for the same bullet.");
        }
    }
    if (changed)
    {
        const ::Poseidon::Ballistics::DragModel model = forced == 1   ? ::Poseidon::Ballistics::DragModel::G1
                                       : forced == 2 ? ::Poseidon::Ballistics::DragModel::G7
                                                     : ::Poseidon::Ballistics::DragModel::Constant;
        ::Poseidon::Ballistics::SetForcedDragModel(model, forced == 0 ? 0.0f : forcedBc);
    }

    if (forced != 0)
    {
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.2f, 1.0f),
                           "OVERRIDE ACTIVE - every round is flying on %s at BC %.3f",
                           forced == 1 ? "G1" : "G7", forcedBc);
    }
    else
    {
        ImGui::TextDisabled("Rounds use their authored drag. No shipped ammo declares a");
        ImGui::TextDisabled("drag function yet, so that means the constant law throughout.");
    }
}

const char* SourceLabel(const BallisticTrack& track)
{
    return track.byPlayer ? "PLAYER" : "AI";
}

void DrawTrackTable()
{
    BallisticsTrackStore& store = Ballistics::Store();

    ImGui::SeparatorText("Shots");

    const int count = store.Size();
    ImGui::Text("%d retained  |  %d samples  |  %d trail segments drawn last frame", count, store.TotalSamples(),
                Ballistics::LastSegmentsDrawn());

    constexpr ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                                      ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_Resizable;

    if (!ImGui::BeginTable("ballistics_tracks", 12, flags, ImVec2(0.0f, 260.0f)))
    {
        return;
    }

    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("#");
    ImGui::TableSetupColumn("src");
    ImGui::TableSetupColumn("ammo");
    ImGui::TableSetupColumn("shooter");
    ImGui::TableSetupColumn("V0 m/s");
    ImGui::TableSetupColumn("Vend m/s");
    ImGui::TableSetupColumn("TOF s");
    ImGui::TableSetupColumn("range m");
    ImGui::TableSetupColumn("path m");
    ImGui::TableSetupColumn("drop m");
    ImGui::TableSetupColumn("lateral m");
    ImGui::TableSetupColumn("terminus");
    ImGui::TableHeadersRow();

    // Newest first: in a firefight the interesting shot is the last one.
    for (int i = count - 1; i >= 0; --i)
    {
        const BallisticTrack& track = store.At(i);
        ImGui::TableNextRow();
        ImGui::PushID(static_cast<int>(track.id));

        ImGui::TableNextColumn();
        char label[32];
        std::snprintf(label, sizeof(label), "%u", track.id);
        const bool selected = (Ballistics::SelectedTrack() == track.id);
        if (ImGui::Selectable(label, selected, ImGuiSelectableFlags_SpanAllColumns))
        {
            Ballistics::SetSelectedTrack(selected ? 0u : track.id);
        }

        ImGui::TableNextColumn();
        if (track.byPlayer)
        {
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.15f, 1.0f), "%s", SourceLabel(track));
        }
        else
        {
            ImGui::TextColored(ImVec4(0.25f, 0.8f, 1.0f, 1.0f), "%s", SourceLabel(track));
        }

        ImGui::TableNextColumn();
        ImGui::TextUnformatted(track.ammo.Get());
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(track.shooter.Get());
        ImGui::TableNextColumn();
        ImGui::Text("%.1f", track.muzzleSpeed);
        ImGui::TableNextColumn();
        ImGui::Text("%.1f", track.TerminalSpeed());
        ImGui::TableNextColumn();
        ImGui::Text("%.3f", track.TimeOfFlight());
        ImGui::TableNextColumn();
        ImGui::Text("%.1f", track.StraightDistance());
        ImGui::TableNextColumn();
        ImGui::Text("%.1f", track.PathLength());
        ImGui::TableNextColumn();
        ImGui::Text("%.2f", track.MaxDrop());
        ImGui::TableNextColumn();
        // Highlight a lateral deviation that is large enough to be wind rather
        // than sampling noise, so "is wind acting?" is answerable at a glance.
        if (std::fabs(track.MaxLateral()) > 0.05f)
        {
            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "%+.2f", track.MaxLateral());
        }
        else
        {
            ImGui::Text("%+.2f", track.MaxLateral());
        }
        ImGui::TableNextColumn();
        if (!track.hit.Empty())
        {
            ImGui::Text("%s: %s", BallisticTerminusName(track.terminus), track.hit.Get());
        }
        else
        {
            ImGui::TextUnformatted(BallisticTerminusName(track.terminus));
        }

        ImGui::PopID();
    }

    ImGui::EndTable();

    ImGui::TextDisabled("Click a row to highlight that trail white in the world.");
}

void DrawSelectedDetail()
{
    const uint32_t id = Ballistics::SelectedTrack();
    if (id == 0)
    {
        return;
    }
    const BallisticTrack* track = Ballistics::Store().Find(id);
    if (!track)
    {
        return;
    }

    ImGui::SeparatorText("Selected shot");
    ImGui::Text("muzzle  [%.2f, %.2f, %.2f]  dir [%.4f, %.4f, %.4f]", track->originX, track->originY, track->originZ,
                track->dirX, track->dirY, track->dirZ);
    if (!track->samples.empty())
    {
        const BallisticSample& end = track->samples.back();
        ImGui::Text("end     [%.2f, %.2f, %.2f]  at t+%.3f s", end.x, end.y, end.z, end.t);
    }
    ImGui::Text("samples %d (1 kept in %d)  |  wind at fire: %s, %.2f m/s toward %.0f deg",
                static_cast<int>(track->samples.size()), track->decimation, track->windActive ? "ACTING" : "ignored",
                track->windSpeed, track->windDirRad * (180.0f / 3.14159265f));
    if (ImGui::Button("Copy summary"))
    {
        char buffer[512];
        std::snprintf(buffer, sizeof(buffer),
                      "shot %u %s %s by %s: V0=%.1f Vend=%.1f TOF=%.3f range=%.1f path=%.1f drop=%.2f "
                      "lateral=%+.2f terminus=%s %s",
                      track->id, SourceLabel(*track), track->ammo.Get(), track->shooter.Get(), track->muzzleSpeed,
                      track->TerminalSpeed(), track->TimeOfFlight(), track->StraightDistance(), track->PathLength(),
                      track->MaxDrop(), track->MaxLateral(), BallisticTerminusName(track->terminus), track->hit.Get());
        ImGui::SetClipboardText(buffer);
    }
}

} // namespace

void DrawBallisticsTab()
{
    bool enabled = Ballistics::Enabled();
    if (ImGui::Checkbox("Record projectile trajectories", &enabled))
    {
        Ballistics::SetEnabled(enabled);
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Off by default and off after every restart.\n"
                          "While off, the per-frame sampler and the impact hooks in\n"
                          "ShotShell::Simulate return on a single bool, no memory is\n"
                          "held, and no line object exists. Turning it off frees the\n"
                          "store and the line-object pool.");
    }

    if (!Ballistics::Enabled())
    {
        ImGui::TextDisabled("Recording is off - nothing is sampled, drawn or retained.");
        ImGui::Separator();
        DrawPauseSection();
        DrawModelSection();
        return;
    }

    ImGui::SameLine();
    bool trails = Ballistics::DrawTrails();
    if (ImGui::Checkbox("Draw trails", &trails))
    {
        Ballistics::SetDrawTrails(trails);
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear"))
    {
        Ballistics::ClearAll();
    }

    BallisticsTrackStore& store = Ballistics::Store();

    int capacity = store.Capacity();
    if (ImGui::SliderInt("Retained shots", &capacity, 1, 64))
    {
        store.SetCapacity(capacity);
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Hard ring-buffer bound. The oldest shot is discarded the\n"
                          "moment an extra one is recorded; the store cannot grow past it.");
    }

    int maxSamples = store.MaxSamples();
    if (ImGui::SliderInt("Samples per shot", &maxSamples, 16, 1024))
    {
        store.SetMaxSamples(maxSamples);
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("When a shot exceeds this, every other sample is dropped and\n"
                          "the stride doubles - so a long-flight shell keeps the shape of\n"
                          "its whole trajectory instead of a truncated head.");
    }

    int budget = Ballistics::SegmentBudget();
    if (ImGui::SliderInt("Trail segment budget", &budget, 64, 8192))
    {
        Ballistics::SetSegmentBudget(budget);
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Ceiling on line segments submitted per frame, so a magazine\n"
                          "dump into the store cannot become the frame cost.");
    }

    DrawPauseSection();
    DrawModelSection();
    DrawWindSection();
    DrawTrackTable();
    DrawSelectedDetail();
}

} // namespace Poseidon::Dev
