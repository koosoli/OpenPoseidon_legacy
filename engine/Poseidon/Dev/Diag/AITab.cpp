// AI: the settings that a measurement cannot decide.
//
// Everything else in this panel reports. This tab changes what the AI does, and it exists
// because PERF-017 reached a wall no benchmark gets past: rationing the near-target scan
// removes 91% of its cost and it also changes what the AI perceives. Cheaper and different
// is not automatically better, and only a person watching a firefight can say which.
//
// So the tab's job is to make the comparison easy to run and hard to misread: one control,
// the measured cost of each setting written next to it, and a plain statement of which value
// is the 2001 behaviour.

#include <Poseidon/Dev/Diag/AITab.hpp>

#include <Poseidon/AI/AITuning.hpp>
#include <Poseidon/AI/AICostAccum.hpp>

// See the note in FixedStepTab.cpp: Foundation's DebugLog macro eats ImGui's member of the
// same name. Nothing here logs.
#undef DebugLog

#include <imgui.h>

#include <Poseidon/Dev/Debug/DevPanelWidgets.hpp>

namespace Poseidon::Dev
{

void DrawAITab()
{
    ImGui::TextUnformatted("Near-target scanning");
    Dev::PanelSameLine();
    Dev::PanelHelp("How often a unit re-scans the enemies it can see within 100 m. Measured on "
                   "perf_combat: this one loop was 68%% of the whole AI stage, because the "
                   "throttle the 2001 code wrote (_trackNearTargetsTime) was never read by "
                   "anything. Rationing it is a BEHAVIOUR change, not just a saving -- the AI "
                   "notices movement later. That is why it is a slider and not a constant.");

    float period = AITuning::TrackNearTargetsPeriod();
    if (Dev::SliderFloat("Rescan period (s)", &period, 0.0f, 2.0f, "%.2f"))
    {
        AITuning::SetTrackNearTargetsPeriod(period);
    }

    // The two values worth knowing by name, as buttons, because typing 0.00 exactly on a
    // slider is fiddly and "did I actually turn it off" is the question this tab must never
    // leave ambiguous.
    if (ImGui::Button("Off (2001 behaviour)"))
    {
        AITuning::SetTrackNearTargetsPeriod(AITuning::kTrackNearTargetsUnrationed);
    }
    Dev::PanelSameLine();
    if (ImGui::Button("Default (0.5 s)"))
    {
        AITuning::SetTrackNearTargetsPeriod(AITuning::kTrackNearTargetsDefault);
    }

    const float now = AITuning::TrackNearTargetsPeriod();
    if (now <= 0.0f)
    {
        ImGui::TextUnformatted("Unrationed -- every group think rescans. track 3.07 ms/tick.");
    }
    else
    {
        ImGui::Text("Rationed to %.2f s. Measured: 0.5 s -> track 0.27 ms/tick, 1.0 s -> 0.15.", now);
    }

    if (AITuning::TrackNearTargetsFromEnvironment())
    {
        // Says so explicitly. A panel that silently disagrees with the environment its
        // process was launched under is how a measurement gets filed under the wrong setting.
        ImGui::TextUnformatted("(started from POSEIDON_AI_TRACK_PERIOD; changes here override it)");
    }

    ImGui::Separator();
    ImGui::TextUnformatted("What it costs, live");
    Dev::PanelSameLine();
    Dev::PanelHelp("The same counters the once-a-second log row uses, read where they stand in "
                   "the CURRENT window -- so they climb and then drop back to zero when the row "
                   "rolls. `trackPairs` is the one that moves: a scan's cost is the size of the "
                   "group's target list, not the fact that it ran.");

    const AICost::CostAccum& costs = AICost::AICosts();
    ImGui::Text("track  %8.3f ms   calls %llu   pairs %llu", costs.ms[static_cast<std::size_t>(AICost::Bucket::GroupTrack)],
                static_cast<unsigned long long>(costs.trackCalls),
                static_cast<unsigned long long>(costs.trackPairs));
    ImGui::Text("ticks in this window  %llu", static_cast<unsigned long long>(costs.ticks));
}

} // namespace Poseidon::Dev
