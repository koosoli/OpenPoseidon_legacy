// Fixed Step: what the simulation accumulator actually did, as opposed to what
// the code says it should do.
//
// The tab exists to answer one question before anyone writes interpolation code:
// IS RENDERING OUTRUNNING THE SIMULATION? Interpolation only buys something when
// it is. On a GPU-bound machine -- and our own measurements say stock worlds are
// GPU-bound, with grass alone a third of the frame -- the simulation is often the
// faster clock, and then there is nothing to interpolate and no judder to fix.
//
// So the headline number is ticks per frame, and the verdict line below it says
// plainly which side of 1.0 the machine is on.

#include <Poseidon/Dev/Diag/FixedStepTab.hpp>

#include <Poseidon/Dev/Diag/FixedStepStats.hpp>
#include <Poseidon/Dev/Diag/SnapshotDiag.hpp>

// Foundation/Framework/DebugLog.hpp defines `DebugLog` as a function-like macro
// and ImGui declares a member of the same name, so the macro eats it. See the
// longer note in PictureModeTab.cpp. Nothing here logs.
#undef DebugLog

#include <imgui.h>

#include <Poseidon/Dev/Debug/DevPanelWidgets.hpp>

namespace Poseidon::Dev
{

void DrawFixedStepTab()
{
    const FixedStepFrame  frame = LastFixedStepFrame();
    const std::uint64_t   frames = FixedStepFrameCount();
    const std::uint64_t   ticks = FixedStepTickCount();
    const std::uint64_t   capped = FixedStepCappedFrames();
    const std::uint32_t   maxSteps = FixedStepMaxStepsInFrame();

    ImGui::SeparatorText("Simulation accumulator");

    if (frame.stepSeconds <= 0.0f)
    {
        ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), "No simulation frame published yet.");
        ImGui::TextDisabled("Load a world; this fills in on the first frame World::Simulate runs.");
        return;
    }

    const double rate = 1.0 / static_cast<double>(frame.stepSeconds);
    ImGui::Text("Fixed rate: %.1f Hz  (%.2f ms per tick)", rate, frame.stepSeconds * 1000.0f);

    if (!frame.running)
    {
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "Simulation is PAUSED — counters are frozen.");
        ImGui::TextDisabled("A paused world is not counted: it would drag the average toward zero\n"
                            "and make a healthy session look starved.");
    }

    ImGui::Separator();
    ImGui::Text("This frame: %u tick(s)", frame.steps);
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("0 means the frame was shorter than one simulation step, so its\n"
                          "time was carried forward. That is normal and expected above\n"
                          "the fixed rate — it is not a stall.");
    }

    ImGui::Text("Alpha: %.3f", frame.alpha);
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Leftover fraction of a step, 0..1 — the interpolation factor a\n"
                          "renderer would use to blend the previous tick into the next.\n"
                          "\n"
                          "NOTHING READS THIS YET. It is published so that the value can be\n"
                          "watched before anyone builds interpolation against it.");
    }

    ImGui::Separator();

    if (frames == 0)
    {
        ImGui::TextDisabled("No running frames sampled yet.");
        return;
    }

    const double perFrame = static_cast<double>(ticks) / static_cast<double>(frames);
    ImGui::Text("Ticks per frame: %.3f   (%llu ticks / %llu frames)", perFrame,
                static_cast<unsigned long long>(ticks), static_cast<unsigned long long>(frames));
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("The honest average. A single frame's tick count is always a whole\n"
                          "number, so only the ratio shows where the two clocks really sit.");
    }

    ImGui::Text("Implied frame rate: %.1f fps", rate / (perFrame > 0.0 ? perFrame : 1.0));

    ImGui::Spacing();
    // The verdict. This is the reason the tab exists.
    if (perFrame < 0.98)
    {
        ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.5f, 1.0f), "Rendering OUTRUNS simulation.");
        ImGui::TextWrapped("Frames are being drawn that repeat the previous tick's state, so motion "
                           "can judder. This is the case where entity interpolation would pay off.");
    }
    else if (perFrame > 1.02)
    {
        ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "Simulation outruns rendering.");
        ImGui::TextWrapped("Several ticks run per drawn frame. Interpolation would buy nothing here — "
                           "every frame already shows fresh state. Frame rate is the limit, not the "
                           "simulation.");
    }
    else
    {
        ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), "Roughly in step.");
        ImGui::TextWrapped("Rendering and simulation are close to the same rate. Interpolation would "
                           "be a marginal win at best.");
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Text("Worst frame: %u tick(s)", maxSteps);

    if (capped > 0)
    {
        const double pct = 100.0 * static_cast<double>(capped) / static_cast<double>(frames);
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Catch-up cap hit on %llu frame(s) — %.2f %%",
                           static_cast<unsigned long long>(capped), pct);
        ImGui::TextWrapped("The accumulator refused to run more steps and DISCARDED simulation time. "
                           "Gameplay ran slower than wall clock on those frames. This is the one "
                           "number here that reports a real problem rather than a measurement.");
    }
    else
    {
        ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.5f, 1.0f), "Catch-up cap never hit.");
        ImGui::TextDisabled("No simulation time has been discarded.");
    }

    ImGui::SeparatorText("Experimental");

    bool scripts = FixedStepScriptsEnabled();
    if (Dev::Checkbox("Run scripts in the fixed tick", &scripts))
    {
        SetFixedStepScripts(scripts);
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip(
            "OFF by default, and it should stay off until missions have been played.\n"
            "\n"
            "Scripts are polled once per FRAME today. On, they are polled 60 times a\n"
            "second instead: fewer times above 60 fps, and MORE below it via catch-up.\n"
            "OFP scripts get a time budget per call, so this changes how fast missions\n"
            "progress. Time-based is the more correct behaviour, but it is a gameplay\n"
            "change that needs playing to judge, not reading.\n"
            "\n"
            "Cutscenes are safe either way: while the simulation is disabled the\n"
            "once-per-frame call is kept regardless of this switch, because camera\n"
            "scripts have to keep running when everything else is frozen.\n");
    }
    if (scripts)
    {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "(experimental)");
    }

    // --- The other half of the sim/render boundary (roadmap 5.3) --------------------
    //
    // The accumulator above says how often simulation ran per frame. THIS says what
    // crossed the boundary between them. 5.3 forbids an unbounded producer/consumer
    // queue and asks for "visible counters for snapshot age, queue depth and renderer
    // wait time"; the counters have existed since the ring landed and nothing displayed
    // them, which is the same as not having them.
    ImGui::Spacing();
    ImGui::SeparatorText("Presentation snapshot");

    const SnapshotCounters& snap = GSnapshotCounters();
    if (snap.published == 0)
    {
        ImGui::TextDisabled("Nothing published yet.");
    }
    else
    {
        // Depth is published-minus-consumed and CANNOT exceed the ring while producer
        // and consumer are one thread; showing the bound next to it is the point, so a
        // reader can see the headroom rather than a bare number.
        const std::uint64_t depth = snap.published - snap.consumed;
        ImGui::Text("Published %llu / consumed %llu   depth %llu of %u slot(s)",
                    (unsigned long long)snap.published, (unsigned long long)snap.consumed,
                    (unsigned long long)depth, snap.ringSize);
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Depth is the queue. The ring is BOUNDED at the slot count, so\n"
                              "simulation cannot run arbitrarily far ahead of presentation and\n"
                              "silently add input latency -- it drops instead of queueing.\n"
                              "That drop is the counter below, not this one.");
        }

        ImGui::Text("Consumed age: %llu frame(s)", (unsigned long long)snap.consumedAge);
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("How old the snapshot the renderer last consumed was, in publishes.\n"
                              "0 is the only value the synchronous path can produce: the world\n"
                              "publishes and the renderer consumes that same frame's values.\n"
                              "Anything above 0 means presentation is running behind simulation,\n"
                              "which only becomes possible once the render thread splits off.");
        }

        if (snap.publishedWithoutConsume > 0)
        {
            ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "Dropped (published without consume): %llu"
                                                               "  (first at publish %llu)",
                               (unsigned long long)snap.publishedWithoutConsume,
                               (unsigned long long)snap.firstDropGeneration);
        }
        else
        {
            ImGui::TextDisabled("Dropped (published without consume): 0");
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("A publish that found the previous snapshot still un-retired.\n"
                              "\n"
                              "EXPECT ONE PER WORLD LOAD, not zero. Through the menu and the\n"
                              "loading screen the RENDERER publishes its own snapshot; the first\n"
                              "frame the world publishes finds that frame's fallback snapshot\n"
                              "already there. Measured: the first drop lands at exactly\n"
                              "fallback + 1, in both runs of a two-run capture. The snapshot it\n"
                              "drops is a menu snapshot nothing was going to draw.\n"
                              "\n"
                              "A count ABOVE the number of world loads is the real event: it\n"
                              "means simulation and presentation have come apart.");
        }

        ImGui::Text("Fallback captures: %llu", (unsigned long long)snap.fallback);
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Frames no world drove, where the RENDERER captured the snapshot\n"
                              "itself (main menu, tools). Expected to be nonzero before a world\n"
                              "loads and flat afterwards; a rising count in play means the world\n"
                              "is missing its publish.");
        }

        ImGui::TextDisabled("Resource epoch: %u", snap.resourceEpoch);
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Bumped when GPU-visible resources are invalidated wholesale.\n"
                              "Nothing bumps it yet, by design: the snapshot carries only scalars\n"
                              "today, so there is nothing an epoch could invalidate. It is the\n"
                              "declared seam for when snapshots start carrying handles.");
        }

        // Renderer wait time is 5.3's third counter and it is NOT displayed, because on
        // this path it is not a measurement -- the consumer IS the producer's caller, so
        // the wait is structurally zero and a row reading "0.00 ms" would look like a
        // measured result rather than an arithmetic identity.
        ImGui::TextDisabled("Renderer wait: n/a (one thread — see tooltip)");
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Not shown rather than shown as zero. Simulation calls the renderer\n"
                              "directly, so there is no point at which presentation waits for a\n"
                              "snapshot; the number would be an identity, not evidence. This row\n"
                              "becomes real when Phase 5 step 6 gives the renderer its own thread.");
        }
    }

    ImGui::Spacing();
    if (ImGui::Button("Reset counters"))
    {
        ResetFixedStepStats();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("Averages are since load or last reset — reset after a loading screen.");
}

} // namespace Poseidon::Dev
